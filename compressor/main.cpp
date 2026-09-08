/*
 * 2pack  -  offline executable compressor (packer).
 *
 * Usage:
 *   2pack <input.exe> [-o <output.exe>] [--no-bcj] [--stub-dir <dir>]
 *
 * Reads a PE executable, LZMA-compresses it (with an x86 BCJ pre-filter),
 * and emits a self-extracting executable: a copy of the matching stub with the
 * compressed original appended in a randomly-named section. There is no
 * unpack-to-file command by design; unpacking happens only in the stub at
 * run time, in memory.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "compress.h"
#include "pe_build.h"
#include "../common/payload.h"

static std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n ? n : 0);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

static void usage() {
    fprintf(stderr,
        "2pack - executable compressor\n"
        "usage: 2pack <input.exe> [-o <output.exe>] [--no-bcj] [--stub-dir <dir>]\n"
        "  -o <file>       output path (default: <input>.packed.exe)\n"
        "  --no-bcj        disable the x86 branch pre-filter\n"
        "  --stub-dir <d>  where to find stub32.exe / stub64.exe (default: 2pack's dir)\n");
}

int main(int argc, char** argv) {
    std::string input, output, stub_dir = exe_dir();
    bool use_bcj = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc)              output = argv[++i];
        else if (a == "--no-bcj")                   use_bcj = false;
        else if (a == "--stub-dir" && i + 1 < argc) stub_dir = argv[++i];
        else if (a == "-h" || a == "--help")        { usage(); return 0; }
        else if (!a.empty() && a[0] == '-')         { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
        else if (input.empty())                     input = a;
        else                                        { fprintf(stderr, "unexpected argument: %s\n", a.c_str()); usage(); return 2; }
    }
    if (input.empty()) { usage(); return 2; }
    if (output.empty()) output = input + ".packed.exe";

    std::string err;
    std::vector<uint8_t> original;
    if (!read_file(input, original, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    PeInfo info;
    if (!inspect_pe(original, info, err)) { fprintf(stderr, "error: not a valid PE: %s\n", err.c_str()); return 1; }
    if (info.is_managed) {
        fprintf(stderr, "error: managed (.NET/CLR) executable is not supported.\n");
        fprintf(stderr, "       2pack's stub maps the PE manually and does not bootstrap the CLR\n");
        fprintf(stderr, "       runtime, so a packed managed binary cannot run.\n");
        return 1;
    }

    std::string stub_path = stub_dir + (info.is64 ? "\\stub64.exe" : "\\stub32.exe");
    std::vector<uint8_t> stub;
    if (!read_file(stub_path, stub, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        fprintf(stderr, "       build the %d-bit stub first (see README).\n", info.is64 ? 64 : 32);
        return 1;
    }

    PackResult pr;
    if (!pack_bytes(original, use_bcj, pr, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    Pack2Header hdr; memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = PACK2_MAGIC;
    hdr.version       = PACK2_VERSION;
    hdr.method        = pr.method;
    hdr.flags         = pr.flags;
    hdr.original_size = original.size();
    hdr.packed_size   = pr.body.size();
    memcpy(hdr.lzma_props, pr.lzma_props, sizeof(hdr.lzma_props));

    std::vector<uint8_t> out;
    if (!build_packed(stub, hdr, pr.body, info.subsystem, out, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!write_file(output, out, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    double ratio = original.empty() ? 0.0 : 100.0 * (double)out.size() / (double)original.size();
    printf("packed %s (%zu bytes) -> %s (%zu bytes, %.1f%% of original)\n",
           input.c_str(), original.size(), output.c_str(), out.size(), ratio);
    printf("  method=%s  bcj=%s  arch=%s\n",
           pr.method == PACK2_METHOD_LZMA ? "lzma" : "store",
           (pr.flags & PACK2_FLAG_BCJ_X86) ? "yes" : "no",
           info.is64 ? "x64" : "x86");
    return 0;
}
