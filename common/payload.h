/*
 * payload.h  -  On-disk contract shared by the compressor and the stub.
 *
 * The packed output is a copy of stub.exe with one extra PE section appended.
 * That section's NAME IS RANDOMIZED per pack (so no two outputs share a
 * recognizable ".pqpk"-style marker). Because the name is random, the stub does
 * NOT look the section up by name: it walks its own section table at run time
 * and matches the first section whose first dword equals PACK2_MAGIC.
 *
 * Section layout:  [ Pack2Header ][ transformed original-exe bytes ]
 *
 * The body is the ORIGINAL executable in its entirety, optionally BCJ-filtered
 * (x86 branch conversion) and then LZMA- or store-compressed. The stub reverses
 * those transforms in memory, manually maps the recovered image and jumps to
 * its entry point.
 */
#ifndef PACK2_PAYLOAD_H
#define PACK2_PAYLOAD_H

#include <stdint.h>

#define PACK2_MAGIC   0x4B514150u  /* 'PAQK' little-endian */
#define PACK2_VERSION 1u

/* Compression method used for the payload body. */
enum Pack2Method {
    PACK2_METHOD_STORE = 0,   /* raw copy, no compression (always available) */
    PACK2_METHOD_LZMA  = 1    /* LZMA (LZMA SDK), see third_party/lzma       */
};

/* Bit flags describing which reversible transforms were applied. */
enum Pack2Flags {
    PACK2_FLAG_BCJ_X86 = 1u << 0   /* x86/x64 E8/E9 branch-call filter (Bra86) */
};

#pragma pack(push, 1)
typedef struct Pack2Header {
    uint32_t magic;            /* PACK2_MAGIC (also the discovery marker)  */
    uint32_t version;          /* PACK2_VERSION                            */
    uint32_t method;           /* enum Pack2Method                         */
    uint32_t flags;            /* enum Pack2Flags                          */
    uint64_t original_size;    /* size in bytes of the original file         */
    uint64_t packed_size;      /* size in bytes of the body that follows     */
    uint8_t  lzma_props[5];    /* LZMA properties blob (needed to decode)    */
    uint8_t  reserved[3];      /* pad to 8-byte boundary                     */
} Pack2Header;
#pragma pack(pop)

#endif /* PACK2_PAYLOAD_H */
