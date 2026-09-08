#include "pe_build.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

/* ---------- file I/O ---------- */

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        err = "cannot open '" + path + "' for reading";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); err = "ftell failed on '" + path + "'"; return false; }
    out.resize((size_t)n);
    size_t rd = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    if (rd != (size_t)n) { err = "short read on '" + path + "'"; return false; }
    return true;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        err = "cannot open '" + path + "' for writing";
        return false;
    }
    size_t wr = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (wr != data.size()) { err = "short write on '" + path + "'"; return false; }
    return true;
}

/* ---------- helpers ---------- */

static uint32_t align_up(uint32_t v, uint32_t a) {
    return a ? ((v + a - 1) / a) * a : v;
}

static IMAGE_NT_HEADERS* nt_headers(const std::vector<uint8_t>& image, std::string& err) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) { err = "file too small for DOS header"; return nullptr; }
    auto* dos = (IMAGE_DOS_HEADER*)image.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err = "bad MZ signature"; return nullptr; }
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > image.size()) { err = "e_lfanew out of range"; return nullptr; }
    auto* nt = (IMAGE_NT_HEADERS*)(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { err = "bad PE signature"; return nullptr; }
    return nt;
}

bool inspect_pe(const std::vector<uint8_t>& image, PeInfo& info, std::string& err) {
    IMAGE_NT_HEADERS* nt = nt_headers(image, err);
    if (!nt) return false;
    info.machine = nt->FileHeader.Machine;
    WORD magic = nt->OptionalHeader.Magic;
    info.is64 = (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    /* Managed (.NET/CLR) images: the CLR runtime is bootstrapped by the Windows
     * loader, which our manual stub does not replicate. Detect via the COM
     * descriptor (CLR header) data directory (index 14).
     *
     * NOTE: read subsystem + CLR dir through the bitness-specific struct. In this
     * SDK `IMAGE_NT_HEADERS` is a conditional typedef that is ALWAYS the 64-bit
     * struct on an x64 compiler, so reading `nt->OptionalHeader.<field>` for an
     * x86 image uses the wrong offsets (a false "managed" result). */
    if (info.is64) {
        auto* nt64 = (IMAGE_NT_HEADERS64*)nt;
        info.subsystem = nt64->OptionalHeader.Subsystem;
        info.is_managed = (nt64->OptionalHeader.NumberOfRvaAndSizes > 14) &&
                          (nt64->OptionalHeader.DataDirectory[14].Size != 0);
    } else {
        auto* nt32 = (IMAGE_NT_HEADERS32*)nt;
        info.subsystem = nt32->OptionalHeader.Subsystem;
        info.is_managed = (nt32->OptionalHeader.NumberOfRvaAndSizes > 14) &&
                          (nt32->OptionalHeader.DataDirectory[14].Size != 0);
    }
    return true;
}

static void random_section_name(char name[IMAGE_SIZEOF_SHORT_NAME]) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static bool seeded = false;
    if (!seeded) { srand((unsigned)(time(nullptr) ^ GetTickCount())); seeded = true; }
    for (int i = 0; i < IMAGE_SIZEOF_SHORT_NAME; ++i)
        name[i] = cs[rand() % (int)(sizeof(cs) - 1)];
}

/* ---------- output assembly ---------- */

bool build_packed(const std::vector<uint8_t>& stub_template,
                  const Pack2Header& hdr,
                  const std::vector<uint8_t>& body,
                  uint16_t orig_subsystem,
                  std::vector<uint8_t>& out,
                  std::string& err)
{
    out = stub_template;   /* mutate a copy of the stub */

    IMAGE_NT_HEADERS* nt = nt_headers(out, err);
    if (!nt) return false;

    const bool is64 = (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    /* Pull alignment + section table location in a bitness-independent way. */
    uint32_t file_align, sect_align;
    if (is64) {
        auto* o = &((IMAGE_NT_HEADERS64*)nt)->OptionalHeader;
        file_align = o->FileAlignment;
        sect_align = o->SectionAlignment;
    } else {
        auto* o = &((IMAGE_NT_HEADERS32*)nt)->OptionalHeader;
        file_align = o->FileAlignment;
        sect_align = o->SectionAlignment;
    }

    WORD num = nt->FileHeader.NumberOfSections;
    IMAGE_SECTION_HEADER* sects = IMAGE_FIRST_SECTION(nt);

    /* Ensure there is room in the headers for one more section entry, i.e. the
     * new entry must not collide with the first section's raw data on disk. */
    uint8_t* new_entry = (uint8_t*)(&sects[num]);
    uint32_t headers_end = (uint32_t)(new_entry + sizeof(IMAGE_SECTION_HEADER) - out.data());
    uint32_t first_raw = 0xFFFFFFFFu;
    for (WORD i = 0; i < num; ++i)
        if (sects[i].PointerToRawData && sects[i].PointerToRawData < first_raw)
            first_raw = sects[i].PointerToRawData;
    if (first_raw != 0xFFFFFFFFu && headers_end > first_raw) {
        err = "stub has no spare room in its PE header for another section; "
              "rebuild the stub or reserve header space";
        return false;
    }

    /* Compute geometry for the new section. */
    uint32_t max_va_end = 0;
    for (WORD i = 0; i < num; ++i) {
        uint32_t e = sects[i].VirtualAddress + sects[i].Misc.VirtualSize;
        if (e > max_va_end) max_va_end = e;
    }
    uint32_t new_va = align_up(max_va_end, sect_align);

    std::vector<uint8_t> raw;
    raw.resize(sizeof(Pack2Header) + body.size());
    memcpy(raw.data(), &hdr, sizeof(Pack2Header));
    if (!body.empty()) memcpy(raw.data() + sizeof(Pack2Header), body.data(), body.size());

    uint32_t virtual_size = (uint32_t)raw.size();
    uint32_t raw_size = align_up((uint32_t)raw.size(), file_align);
    uint32_t ptr_raw = align_up((uint32_t)out.size(), file_align);

    /* Fill the new section header. */
    IMAGE_SECTION_HEADER sh; memset(&sh, 0, sizeof(sh));
    random_section_name((char*)sh.Name);
    sh.Misc.VirtualSize = virtual_size;
    sh.VirtualAddress   = new_va;
    sh.SizeOfRawData    = raw_size;
    sh.PointerToRawData = ptr_raw;
    sh.Characteristics  = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    memcpy(new_entry, &sh, sizeof(sh));

    /* Patch headers (nt pointer is still valid; out not resized yet). */
    nt->FileHeader.NumberOfSections = num + 1;

    uint32_t new_size_of_image = align_up(new_va + virtual_size, sect_align);
    if (is64) {
        auto* o = &((IMAGE_NT_HEADERS64*)nt)->OptionalHeader;
        o->SizeOfImage = new_size_of_image;
        o->Subsystem   = orig_subsystem;
        o->CheckSum    = 0;
    } else {
        auto* o = &((IMAGE_NT_HEADERS32*)nt)->OptionalHeader;
        o->SizeOfImage = new_size_of_image;
        o->Subsystem   = orig_subsystem;
        o->CheckSum    = 0;
    }

    /* Append: pad to PointerToRawData, then the raw section, padded to raw_size. */
    if (out.size() < ptr_raw) out.resize(ptr_raw, 0);
    out.insert(out.end(), raw.begin(), raw.end());
    out.resize(ptr_raw + raw_size, 0);
    return true;
}
