/*
 * stub_main.cpp  -  runtime unpacker embedded in every packed executable.
 *
 * The packed output is a copy of this stub with one extra PE section appended.
 * That section's NAME is randomized at pack time, so the stub does not look it
 * up by name: it walks its own section table and matches the first section
 * whose first dword equals PACK2_MAGIC.
 *
 * At launch the stub:
 *   1. locates its own appended payload section (by magic),
 *   2. LZMA-decompresses and un-BCJs the original file bytes in memory,
 *   3. manually maps the recovered image (sections, page protections,
 *      base relocations, imports, TLS) exactly like the Windows loader,
 *   4. transfers control to the original entry point.
 *
 * The stub is a raw Win32 image: NO C runtime, NO CRT startup. It links with
 * /NODEFAULTLIB and imports only kernel32, so it stays tiny and portable. Its
 * bitness must match the packed program's bitness; 2pack picks the matching
 * stub (stub32.exe / stub64.exe) when packing.
 *
 * The stub is built as a GUI-subsystem image. At run time it calls AllocConsole()
 * only when the ORIGINAL is a console (CUI) app, so packed GUI apps produce no
 * stray console window and packed console apps get one.
 *
 * Known limitations (see README):
 *   - Requires the original to be mappable at its preferred base OR to carry a
 *     .reloc section (for base relocations).
 *   - Runs the image inside the stub's own process (shared address space).
 *   - No delay-import, no forwarded-export, no IAT-thunk relocations beyond the
 *     standard types, no CET/CFG shadow-stack fixups.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include "../common/payload.h"

#ifdef PACK2_USE_LZMA
extern "C" {
#include "LzmaDec.h"
#include "Bra.h"
}
/* SDK memory hooks -> process heap (kernel32 only). */
static void* SzAlloc(ISzAllocPtr, size_t size) { return HeapAlloc(GetProcessHeap(), 0, size); }
static void  SzFree(ISzAllocPtr, void* p)       { if (p) HeapFree(GetProcessHeap(), 0, p); }
static ISzAlloc g_alloc = { SzAlloc, SzFree };
#endif

/* ------------------------------------------------------------------ */
/* Diagnostics. Errors go to the debugger and, if a console exists,    */
/* to stderr. Phase markers (dbg) are only emitted when the            */
/* PACK2_DEBUG environment variable is set, so that a packed binary  */
/* behaves byte-identically to the original by default.                */
/* ------------------------------------------------------------------ */
static int debug_on() {
    static int v = -1;
    if (v < 0) v = (GetEnvironmentVariableA("PACK2_DEBUG", nullptr, 0) != 0) ? 1 : 0;
    return v;
}

static void dbg(const char* m) {
    /* Diagnostics go to stderr, only when PACK2_DEBUG is set, so a packed
     * binary is byte-identical to the original by default.
     *
     * Note: we deliberately do NOT call OutputDebugStringA here. In this
     * no-CRT (/NODEFAULTLIB) stub the process heap is never initialized,
     * and OutputDebugStringA reaches RtlAllocateHeap when a debugger is
     * attached (via CreateDBWinMutex -> RtlAllocateAndInitializeSid) and
     * faults on the NULL heap. The stderr write below uses only kernel32
     * (GetStdHandle/WriteFile) and never touches the heap. */
    if (!debug_on()) return;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, m, (DWORD)lstrlenA(m), &w, nullptr);
    }
}

static void fail(const char* msg) {
    char buf[160];
    dbg("2pack stub: ");
    dbg(msg);
    dbg("\n");
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        const char* p = "2pack stub error: ";
        WriteFile(h, p, (DWORD)lstrlenA(p), &w, nullptr);
        WriteFile(h, msg, (DWORD)lstrlenA(msg), &w, nullptr);
        WriteFile(h, "\r\n", 2, &w, nullptr);
    }
    (void)buf;
    ExitProcess(1);
}

/* Base address of the stub's own image (linker-provided, no import needed). */
extern "C" IMAGE_DOS_HEADER __ImageBase;
static BYTE* module_base() { return (BYTE*)&__ImageBase; }

/* memcpy is provided by crt.c (compiled as C) because this translation unit
 * is C++ and cannot redefine the extern "C" memcpy that the runtime headers
 * already declare. */

/* ------------------------------------------------------------------ */
/* Locate the payload section inside our own mapped image.             */
/* ------------------------------------------------------------------ */
static const Pack2Header* find_payload() {
    BYTE* base = module_base();
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (s[i].VirtualAddress == 0 || s[i].Misc.VirtualSize < sizeof(Pack2Header))
            continue;
        auto* h = (const Pack2Header*)(base + s[i].VirtualAddress);
        if (h->magic == PACK2_MAGIC && h->version == PACK2_VERSION)
            return h;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* Recover the original file bytes from the payload body.              */
/* ------------------------------------------------------------------ */
static BYTE* recover_original(const Pack2Header* hdr, SIZE_T& out_size) {
    const BYTE* body = (const BYTE*)(hdr + 1);
    out_size = (SIZE_T)hdr->original_size;
    BYTE* dst = (BYTE*)VirtualAlloc(nullptr, out_size, MEM_COMMIT, PAGE_READWRITE);
    if (!dst) fail("out of memory recovering original");

    if (hdr->method == PACK2_METHOD_STORE) {
        if (hdr->packed_size != hdr->original_size) fail("store size mismatch");
        memcpy(dst, body, out_size);
    } else if (hdr->method == PACK2_METHOD_LZMA) {
#ifdef PACK2_USE_LZMA
        SizeT destLen = (SizeT)hdr->original_size;
        SizeT srcLen  = (SizeT)hdr->packed_size;
        ELzmaStatus status;
        SRes res = LzmaDecode(dst, &destLen, body, &srcLen,
                              hdr->lzma_props, LZMA_PROPS_SIZE,
                              LZMA_FINISH_END, &status, &g_alloc);
        if (res != SZ_OK || destLen != (SizeT)hdr->original_size)
            fail("LZMA decode failed");
#else
        fail("payload is LZMA but stub built without PACK2_USE_LZMA");
#endif
    } else {
        fail("unknown compression method");
    }

    if (hdr->flags & PACK2_FLAG_BCJ_X86) {
#ifdef PACK2_USE_LZMA
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Dec(dst, (SizeT)hdr->original_size, 0, &state);
#else
        fail("payload is BCJ-filtered but stub built without the SDK filter");
#endif
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* Read the optional-header fields we need, in a bitness-aware way.    */
/* ------------------------------------------------------------------ */
struct OptFields {
    ULONG_PTR imageBase;
    ULONG_PTR sizeOfImage;
    ULONG_PTR entryRVA;
    ULONG_PTR sizeOfHeaders;
    WORD      subsystem;
    bool      is64;
};

static bool read_opt_fields(const BYTE* orig, OptFields& f) {
    auto* dos = (IMAGE_DOS_HEADER*)orig;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if ((SIZE_T)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > 0x100000000ull) return false;
    auto* nt = (IMAGE_NT_HEADERS*)(orig + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    f.is64 = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    if (f.is64) {
        auto* o = &((IMAGE_NT_HEADERS64*)nt)->OptionalHeader;
        f.imageBase     = o->ImageBase;
        f.sizeOfImage   = o->SizeOfImage;
        f.entryRVA      = o->AddressOfEntryPoint;
        f.sizeOfHeaders = o->SizeOfHeaders;
        f.subsystem     = o->Subsystem;
    } else {
        auto* o = &((IMAGE_NT_HEADERS32*)nt)->OptionalHeader;
        f.imageBase     = (ULONG_PTR)o->ImageBase;
        f.sizeOfImage   = o->SizeOfImage;
        f.entryRVA      = o->AddressOfEntryPoint;
        f.sizeOfHeaders = o->SizeOfHeaders;
        f.subsystem     = o->Subsystem;
    }
    return true;
}

static bool data_dir_present(const BYTE* orig, int idx) {
    auto* dos = (IMAGE_DOS_HEADER*)orig;
    auto* nt  = (IMAGE_NT_HEADERS*)(orig + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[idx];
    return d.VirtualAddress != 0 && d.Size != 0;
}

/* ------------------------------------------------------------------ */
/* Manually map the recovered image.                                   */
/* Everything is copied into a fully writable region; final per-section */
/* protections are applied later (set_protections), after relocations,  */
/* imports and TLS have finished writing (the IAT lives in a read-only  */
/* section and must be writable while it is being filled in).           */
/* ------------------------------------------------------------------ */
static BYTE* map_image(const BYTE* orig, ULONG_PTR& out_base, OptFields& f) {
    auto* dos = (IMAGE_DOS_HEADER*)orig;
    auto* nt  = (IMAGE_NT_HEADERS*)(orig + dos->e_lfanew);

    /* Try the preferred base first; fall back to any free region (which then
     * requires a .reloc section to fix up the addresses). */
    BYTE* base = (BYTE*)VirtualAlloc((LPVOID)f.imageBase, f.sizeOfImage,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        if (!data_dir_present(orig, IMAGE_DIRECTORY_ENTRY_BASERELOC))
            fail("cannot map at preferred base and image has no .reloc");
        base = (BYTE*)VirtualAlloc(nullptr, f.sizeOfImage,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) fail("VirtualAlloc for image failed");
    }

    /* PE headers. */
    memcpy(base, orig, (SIZE_T)f.sizeOfHeaders);

    /* Sections: copy raw data; the [copy, vsz) tail stays zero. */
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!s[i].VirtualAddress) continue;
        BYTE* dest = base + s[i].VirtualAddress;
        SIZE_T vsz = (SIZE_T)s[i].Misc.VirtualSize;
        SIZE_T rsz = (SIZE_T)s[i].SizeOfRawData;
        SIZE_T copy = (rsz < vsz) ? rsz : vsz;
        if (copy) memcpy(dest, orig + s[i].PointerToRawData, copy);
    }

    out_base = (ULONG_PTR)base;
    return base;
}

/* ------------------------------------------------------------------ */
/* Apply the final per-section memory protections. Call after all      */
/* fixups that write into the image are complete.                      */
/* ------------------------------------------------------------------ */
static void set_protections(BYTE* base) {
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!s[i].VirtualAddress) continue;
        SIZE_T vsz = (SIZE_T)s[i].Misc.VirtualSize;
        if (!vsz) continue;
        BOOL read  = (s[i].Characteristics & IMAGE_SCN_MEM_READ)    != 0;
        BOOL write = (s[i].Characteristics & IMAGE_SCN_MEM_WRITE)   != 0;
        BOOL exec  = (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        DWORD prot;
        if (read && write && exec)      prot = PAGE_EXECUTE_READWRITE;
        else if (read && exec)          prot = PAGE_EXECUTE_READ;
        else if (read && write)         prot = PAGE_READWRITE;
        else if (read)                  prot = PAGE_READONLY;
        else                             prot = PAGE_NOACCESS;
        DWORD old;
        if (!VirtualProtect(base + s[i].VirtualAddress, vsz, prot, &old))
            fail("VirtualProtect on a section failed");
    }
}

/* ------------------------------------------------------------------ */
/* Base relocations.                                                   */
/* ------------------------------------------------------------------ */
static void apply_relocations(BYTE* base, ULONG_PTR mappedBase, ULONG_PTR preferredBase) {
    LONG_PTR delta = (LONG_PTR)mappedBase - (LONG_PTR)preferredBase;
    if (delta == 0) return;

    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!d.VirtualAddress || !d.Size)
        fail("image relocated but has no relocation table");

    IMAGE_BASE_RELOCATION* rel = (IMAGE_BASE_RELOCATION*)(base + d.VirtualAddress);
    BYTE* end = base + d.VirtualAddress + d.Size;
    while ((BYTE*)rel < end && rel->SizeOfBlock) {
        DWORD count = (rel->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        WORD* entry = (WORD*)(rel + 1);
        BYTE* page  = base + rel->VirtualAddress;
        for (DWORD i = 0; i < count; ++i) {
            WORD type = entry[i] >> 12;
            WORD off  = entry[i] & 0x0FFF;
            switch (type) {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;
                case IMAGE_REL_BASED_HIGHLOW:
                    *(DWORD*)(page + off) += (DWORD)delta;
                    break;
                case IMAGE_REL_BASED_DIR64:
                    *(ULONGLONG*)(page + off) += (ULONGLONG)delta;
                    break;
                case IMAGE_REL_BASED_HIGH:
                    *(WORD*)(page + off) += (WORD)((delta >> 16) & 0xFFFF);
                    break;
                case IMAGE_REL_BASED_LOW:
                    *(BYTE*)(page + off) += (BYTE)(delta & 0xFF);
                    break;
                default:
                    fail("unsupported relocation type");
            }
        }
        rel = (IMAGE_BASE_RELOCATION*)((BYTE*)rel + rel->SizeOfBlock);
    }
}

/* ------------------------------------------------------------------ */
/* Import resolution.                                                  */
/* ------------------------------------------------------------------ */
static void resolve_imports(BYTE* base) {
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress || !d.Size) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        HMODULE hmod = LoadLibraryA(dll);
        if (!hmod) fail("LoadLibraryA failed for an imported DLL");

        DWORD lookupRVA = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        IMAGE_THUNK_DATA* lookup = (IMAGE_THUNK_DATA*)(base + lookupRVA);
        IMAGE_THUNK_DATA* iat    = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            FARPROC fn;
            const char* sym = "?";
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal)) {
                sym = "<ordinal>";
                fn = GetProcAddress(hmod, (LPCSTR)(SIZE_T)IMAGE_ORDINAL(lookup->u1.Ordinal));
            } else {
                /* AddressOfData is an RVA to IMAGE_IMPORT_BY_NAME:
                 *   { WORD Hint; char Name[]; }
                 * GetProcAddress wants the Name string, i.e. 2 bytes in. */
                sym = (const char*)(base + lookup->u1.AddressOfData + 2);
                fn = GetProcAddress(hmod, sym);
            }
            if (!fn) {
                HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
                if (h && h != INVALID_HANDLE_VALUE) {
                    DWORD w = 0;
                    const char* t1 = "2pack stub: GetProcAddress failed  DLL=";
                    WriteFile(h, t1, (DWORD)lstrlenA(t1), &w, nullptr);
                    WriteFile(h, dll, (DWORD)lstrlenA(dll), &w, nullptr);
                    const char* t2 = "  SYM=";
                    WriteFile(h, t2, (DWORD)lstrlenA(t2), &w, nullptr);
                    WriteFile(h, sym, (DWORD)lstrlenA(sym), &w, nullptr);
                    WriteFile(h, "\r\n", 2, &w, nullptr);
                }
                fail("GetProcAddress failed for an import");
            }
            iat->u1.Function = (ULONG_PTR)fn;
        }
    }
}

/* ------------------------------------------------------------------ */
/* TLS: replicate the loader's per-thread setup.                       */
/*                                                                      */
/* Two directory formats exist in the wild:                             */
/*  - MSVC: all four fields are RVAs.                                   */
/*  - Go:   all four fields are ABSOLUTE addresses (preferred base +    */
/*    RVA). Go also allocates a TLS index ABOVE the 1024 fixed TEB      */
/*    slots (handled by TlsExpansion).                                  */
/*                                                                      */
/* Both MSVC and Go images carry real PE TLS callbacks that the Windows */
/* loader invokes at process init, so the stub must invoke them too     */
/* (after relocations + imports, once the index/block are set up).      */
/*                                                                      */
/* We auto-detect: if every field is a valid RVA we treat them as RVAs, */
/* otherwise as absolute addresses (base + delta). We use TlsSetValue   */
/* to publish the per-thread block, which correctly places it in the    */
/* TEB (TlsSlots for index < 1024, TlsExpansion above) without us       */
/* having to touch TEB offsets by hand.                                 */
/* ------------------------------------------------------------------ */
static bool rva_in_image(ULONG_PTR rva, ULONG_PTR sizeOfImage) {
    return rva != 0 && rva < sizeOfImage;
}

static void setup_tls(BYTE* base, ULONG_PTR mappedBase, const OptFields& f) {
    (void)mappedBase;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!d.VirtualAddress || !d.Size) return;

    bool is64 = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    ULONG_PTR start, end, addrOfIndex, addrOfCallbacks;
    SIZE_T zerofill;
    if (is64) {
        auto* tls = (IMAGE_TLS_DIRECTORY64*)(base + d.VirtualAddress);
        start         = (ULONG_PTR)tls->StartAddressOfRawData;
        end           = (ULONG_PTR)tls->EndAddressOfRawData;
        addrOfIndex   = (ULONG_PTR)tls->AddressOfIndex;
        addrOfCallbacks = (ULONG_PTR)tls->AddressOfCallBacks;
        zerofill      = tls->SizeOfZeroFill;
    } else {
        auto* tls = (IMAGE_TLS_DIRECTORY32*)(base + d.VirtualAddress);
        start         = (ULONG_PTR)tls->StartAddressOfRawData;
        end           = (ULONG_PTR)tls->EndAddressOfRawData;
        addrOfIndex   = (ULONG_PTR)tls->AddressOfIndex;
        addrOfCallbacks = (ULONG_PTR)tls->AddressOfCallBacks;
        zerofill      = tls->SizeOfZeroFill;
    }

    /* Format detection: MSVC RVAs must all fall inside the image. Go's
     * absolute fields sit far outside it (and are all-or-nothing). */
    bool asRva = rva_in_image(start, f.sizeOfImage) &&
                 rva_in_image(end, f.sizeOfImage) &&
                 rva_in_image(addrOfIndex, f.sizeOfImage) &&
                 rva_in_image(addrOfCallbacks, f.sizeOfImage);
    auto toRva = [asRva, imageBase = f.imageBase](ULONG_PTR v) -> ULONG_PTR {
        if (v == 0) return 0;
        return asRva ? v : (ULONG_PTR)((LONG_PTR)v - (LONG_PTR)imageBase);
    };
    ULONG_PTR startRva = toRva(start);
    ULONG_PTR endRva   = toRva(end);
    ULONG_PTR idxRva   = toRva(addrOfIndex);
    if (!startRva || !idxRva)
        fail("TLS directory points outside the image (unrecognized format)");

    SIZE_T rawSize = (SIZE_T)(endRva - startRva);
    SIZE_T total   = rawSize + zerofill;

    /* Writable per-thread block with the image's initial TLS values.
     *
     * Allocate 8 EXTRA bytes before the block and publish (alloc+8) as the
     * block pointer. The loader's LdrpHandleTlsData (run on worker threads
     * during background module loads) stores the per-module TLS vector it
     * reads from TLP[index] and, on thread teardown, frees it via
     *   r8 = [vector-8];  RtlFreeHeap(LdrpTlsHeap, 0, r8);
     * A page-aligned VirtualAlloc block has [block-8] in the previous
     * UNMAPPED page → access violation. By reserving an 8-byte prefix,
     * [block-8] is readable and zero (MEM_COMMIT pages are zero-init), so
     * RtlFreeHeap(LdrpTlsHeap, 0, 0) is a no-op and the loader proceeds. */
    BYTE* block = nullptr;
    if (total) {
        BYTE* alloc = (BYTE*)VirtualAlloc(nullptr, total + 8, MEM_COMMIT, PAGE_READWRITE);
        if (!alloc) fail("out of memory allocating TLS block");
        block = alloc + 8;
        if (rawSize) memcpy(block, base + startRva, rawSize);
    }

    /* Choose a TLS slot and publish the block in the TEB.
     *
     * We deliberately do NOT call TlsAlloc/TlsSetValue. This is a no-CRT
     * (/NODEFAULTLIB) stub, so the process heap is never initialized;
     * TlsAlloc reaches RtlAllocateHeap (to grow the TlsExpansion array)
     * and faults on the NULL heap. Instead we do exactly what the Windows
     * loader does for fixed slots: scan the TEB's TlsSlots array for a free
     * (NULL) slot and store the block pointer there directly. The app's TLS
     * callbacks (Rust std, MSVC CRT, Go) fetch the block with a raw
     * `gs:[TlsSlots + index*8]` read, so the block must physically sit in
     * that array slot. We publish the chosen index in the image's
     * AddressOfIndex variable, so the callbacks read the slot we filled. */
    {
        /* Publish the per-thread block where the app's TLS callback reads it.
         *
         * The callback (Rust std / MSVC CRT) does a DOUBLE indirection:
         *   TLP = gs:[0x58]            ; TEB->ThreadLocalStoragePointer
         *   block = TLP[index]         ; index from the image's AddressOfIndex
         * In this process TLP is a per-thread heap array that is DISTINCT from
         * the embedded TEB->TlsSlots array (TEB+0x1480 on x64). TlsSetValue
         * writes the embedded array, so it does NOT satisfy the callback. We
         * therefore do three things, all for the same index:
         *   1. TlsAlloc()      -> sets the PEB TlsBitmap bit, so the loader's
         *                          LdrpHandleTlsData (which runs on worker
         *                          threads) knows the slot is taken. Without
         *                          this, a direct TLP write left the bitmap
         *                          stale and a worker thread crashed on exit.
         *   2. TlsSetValue()   -> writes the embedded TlsSlots[index].
         *   3. TLP[index]=blk  -> writes the array the callback actually reads.
         * For the first 64 slots TlsAlloc is bitmap-only (no RtlAllocateHeap),
         * so it is safe in this no-CRT stub. */
        DWORD index = TlsAlloc();
        if (index == TLS_OUT_OF_INDEXES)
            fail("TlsAlloc failed");
        if (block) {
            TlsSetValue(index, block);
#ifdef _M_X64
            BYTE* teb = (BYTE*)(ULONG_PTR)__readgsqword(0x30);
            ULONG_PTR tlp = *(ULONG_PTR*)(teb + 0x58);   /* ThreadLocalStoragePointer */
            if (tlp) ((ULONG_PTR*)tlp)[index] = (ULONG_PTR)block;
#endif
        }
        /* Publish the index where the app's runtime expects it. */
        if (is64) *(DWORD64*)(base + idxRva) = (DWORD64)index;
        else      *(DWORD*)  (base + idxRva) = (DWORD)  index;
    }

    /* Invoke the image's TLS callbacks, exactly as the Windows loader does
     * at process/thread init: after relocations and imports are applied and
     * after the index + per-thread block are in place. The callbacks are
     * already relocated (apply_relocations ran earlier), so calling them
     * directly is safe. They run while the image is still fully writable.
     *
     * This is required for MSVC images (the MSVC CRT performs its TLS init
     * here) and for Go images (the Go runtime registers PE TLS callbacks
     * that the loader invokes at startup). The callback array is
     * NULL-terminated; an empty/absent list is a no-op. */
    ULONG_PTR cbRva = toRva(addrOfCallbacks);
    if (cbRva) {
        typedef void (WINAPI *TlsCbFn)(ULONG_PTR, DWORD, LPVOID);
        SIZE_T cbCount = 0;
        const ULONG_PTR* cbs = (const ULONG_PTR*)(base + cbRva);
        while (cbs[cbCount] != 0) {
            if (cbCount >= 0x1000) break;   /* sanity cap */
            ++cbCount;
        }
        for (SIZE_T i = 0; i < cbCount; ++i) {
            TlsCbFn cb = (TlsCbFn)(base + toRva(cbs[i]));
            dbg("2pack stub: tls callback\n");
            /* Match the loader: process-attach then thread-attach. The
             * per-thread init actually runs on reason 3 (THREAD_ATTACH);
             * reason 2 (PROCESS_ATTACH) is typically a no-op. */
            cb((ULONG_PTR)base, 2 /* TLS_PROCESS */, nullptr);
            cb((ULONG_PTR)base, 3 /* TLS_THREAD  */, nullptr);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry.                                                              */
/* ------------------------------------------------------------------ */
extern "C" int main(void) {
    const Pack2Header* hdr = find_payload();
    if (!hdr) fail("payload section not found (run a packed image?)");

    SIZE_T orig_size = 0;
    BYTE* orig = recover_original(hdr, orig_size);

    OptFields f;
    if (!read_opt_fields(orig, f)) fail("recovered image is not a valid PE");
    if (f.subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI &&
        f.subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI)
        fail("unsupported subsystem (only CUI/GUI exes)");

    /* Give a console app a console; GUI apps stay console-free. */
    if (f.subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI) {
        AllocConsole();
    }

    ULONG_PTR mappedBase = 0;
    BYTE* base = map_image(orig, mappedBase, f);
    dbg("2pack stub: mapped\n");

    /* Loader order: fix up addresses, then imports, then run TLS callbacks
     * (which expect a fully relocated, import-resolved image). The image is
     * still fully writable here; final protections go on last. */
    apply_relocations(base, mappedBase, f.imageBase);
    dbg("2pack stub: relocations done\n");
    resolve_imports(base);
    dbg("2pack stub: imports done\n");
    /* Set final section protections BEFORE the TLS callbacks: the callbacks
     * execute code in .text, which must already be executable. Relocations
     * and import fixups (the only things that write into the image) are done,
     * so it is safe to make .rdata/.text read-only/executable now. This
     * matches the Windows loader, which maps .text executable before running
     * TLS callbacks. */
    set_protections(base);
    dbg("2pack stub: protections set\n");
    setup_tls(base, mappedBase, f);
    dbg("2pack stub: tls done\n");

    typedef int (WINAPI *EntryFn)(void);
    EntryFn entry = (EntryFn)(base + f.entryRVA);
    dbg("2pack stub: calling original entry\n");
    int rc = entry();
    ExitProcess(rc);
    return rc; /* unreachable */
}
