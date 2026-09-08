#include "compress.h"

#include <string>

#ifdef PACK2_USE_LZMA
/* From the LZMA SDK (third_party/lzma). Add these to the project's include
 * path and sources: LzmaEnc.c, LzmaDec.c, Bra86.c, LzFind.c, Alloc.c, etc. */
extern "C" {
#include "LzmaEnc.h"
#include "Bra.h"
}

/* SDK memory hooks -> plain malloc/free. */
static void* SzAlloc(ISzAllocPtr, size_t size) { return ::malloc(size); }
static void  SzFree(ISzAllocPtr, void* p)       { ::free(p); }
static ISzAlloc g_alloc = { SzAlloc, SzFree };
#endif

bool pack_bytes(const std::vector<uint8_t>& input,
                bool apply_bcj,
                PackResult& out,
                std::string& err)
{
#ifdef PACK2_USE_LZMA
    /* Work on a mutable copy so the BCJ filter can rewrite branch operands. */
    std::vector<uint8_t> work = input;

    if (apply_bcj && !work.empty()) {
        /* 7-zip LZMA SDK BCJ (x86) filter: relative -> absolute branch targets.
         * The converter is deterministic and its _Dec is the exact inverse of
         * _Enc for the same input bytes + state, so it is safely reversible on
         * the whole file (pc = 0, state = 0, matching the SDK's own usage). */
        UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
        z7_BranchConvSt_X86_Enc((Byte*)work.data(), (SizeT)work.size(), 0, &state);
        out.flags |= PACK2_FLAG_BCJ_X86;
    }

    CLzmaEncProps props;
    LzmaEncProps_Init(&props);
    props.level = 9;                 /* max ratio */
    props.dictSize = 1 << 26;        /* 64 MiB dictionary */
    props.writeEndMark = 0;
    props.numThreads = 1;            /* single-threaded: portable, no LzFindMt/Threads */

    /* LZMA can expand incompressible data slightly; give generous headroom. */
    size_t dstCap = work.size() + work.size() / 2 + (1 << 16);
    out.body.resize(dstCap);

    SizeT destLen = dstCap;
    SizeT propsSize = sizeof(out.lzma_props);
    SRes res = LzmaEncode(out.body.data(), &destLen,
                          work.data(), work.size(),
                          &props, out.lzma_props, &propsSize, 0,
                          nullptr, &g_alloc, &g_alloc);
    if (res != SZ_OK || propsSize != sizeof(out.lzma_props)) {
        err = "LzmaEncode failed (SRes=" + std::to_string(res) + ")";
        return false;
    }
    out.body.resize(destLen);
    out.method = PACK2_METHOD_LZMA;
    return true;
#else
    (void)apply_bcj;
    (void)err;
    /* STORE fallback: raw copy, no filter. */
    out.body = input;
    out.method = PACK2_METHOD_STORE;
    out.flags = 0;
    return true;
#endif
}
