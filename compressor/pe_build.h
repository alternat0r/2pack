/*
 * pe_build.h  -  Minimal PE reading + output assembly for the packer.
 */
#ifndef PACK2_PE_BUILD_H
#define PACK2_PE_BUILD_H

#include <cstdint>
#include <string>
#include <vector>

#include "../common/payload.h"

struct PeInfo {
    bool     is64 = false;      /* PE32+ (x64) vs PE32 (x86)          */
    uint16_t machine = 0;       /* IMAGE_FILE_MACHINE_*               */
    uint16_t subsystem = 0;     /* IMAGE_SUBSYSTEM_*                  */
};

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err);
bool write_file(const std::string& path, const std::vector<uint8_t>& data, std::string& err);

/* Parse just enough of `image` to fill PeInfo. */
bool inspect_pe(const std::vector<uint8_t>& image, PeInfo& info, std::string& err);

/*
 * Produce the packed output by appending one section (random name) to the stub
 * template. The section contains `hdr` immediately followed by `body`.
 *   orig_subsystem : copied into the output so the packed exe keeps the
 *                    original's console/GUI behavior.
 * Returns false + `err` if the stub has no room for another section header.
 */
bool build_packed(const std::vector<uint8_t>& stub_template,
                  const Pack2Header& hdr,
                  const std::vector<uint8_t>& body,
                  uint16_t orig_subsystem,
                  std::vector<uint8_t>& out,
                  std::string& err);

#endif /* PACK2_PE_BUILD_H */
