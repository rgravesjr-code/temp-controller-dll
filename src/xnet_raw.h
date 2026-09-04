/* xnet_raw.h - internal: write one NI-XNET raw frame record (little-endian). */
#ifndef XNET_RAW_H
#define XNET_RAW_H
#include <stdint.h>
#include <string.h>
#include "tempctl.h"

static inline void put_u16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_u32le(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void put_u64le(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Writes TC_RAW_FRAME_SIZE bytes. id is the bare 11/29-bit identifier. */
static inline void xnet_raw_frame(uint8_t* p, uint64_t ts100ns, uint32_t id, int extended,
                                  const uint8_t* data, int dlc)
{
    memset(p, 0, TC_RAW_FRAME_SIZE);
    put_u64le(p, ts100ns);
    put_u32le(p + 8, (id & 0x1FFFFFFFu) | (extended ? TC_XNET_EXTENDED_ID_FLAG : 0u));
    p[12] = 0x00;                 /* Type: CAN data frame */
    p[13] = 0x00;                 /* Flags */
    p[14] = 0x00;                 /* Info */
    p[15] = (uint8_t)dlc;         /* PayloadLength */
    if (dlc > 0) memcpy(p + 16, data, (size_t)dlc);
}
#endif
