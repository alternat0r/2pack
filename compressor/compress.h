/*
 * compress.h  -  Compression front-end for the packer.
 *
 * Exposes a single entry point that transforms the original file bytes into the
 * packed body plus the metadata the stub needs to reverse it.
 *
 * Build modes:
 *   - Define PACK2_USE_LZMA and add the LZMA SDK to the project to get LZMA
 *     compression and the x86 BCJ filter (recommended).
 *   - Otherwise the packer falls back to STORE (no compression, no filter) so
 *     the whole pipeline still builds and runs end-to-end.
 */
#ifndef PACK2_COMPRESS_H
#define PACK2_COMPRESS_H

#include <cstdint>
#include <string>
#include <vector>

#include "../common/payload.h"

struct PackResult {
    std::vector<uint8_t> body;   /* transformed payload bytes                */
    uint32_t method = PACK2_METHOD_STORE;
    uint32_t flags  = 0;
    uint8_t  lzma_props[5] = {0, 0, 0, 0, 0};
};

/*
 * Transform `input` into a packed body.
 *   apply_bcj : request the x86 branch filter before compression (LZMA builds).
 * Returns true on success. On failure `err` is set and the function returns
 * false. A build without PACK2_USE_LZMA always produces a STORE result.
 */
bool pack_bytes(const std::vector<uint8_t>& input,
                bool apply_bcj,
                PackResult& out,
                std::string& err);

#endif /* PACK2_COMPRESS_H */
