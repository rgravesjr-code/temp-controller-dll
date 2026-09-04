/* xnet.c - NI-XNET raw frame record read/write helpers + exported record API. */
#include "cantp_internal.h"

void xnet_put_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void xnet_put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void xnet_put_u64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint32_t xnet_get_u32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
uint64_t xnet_get_u64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* NI-XNET logfile spec: FrameSize = 24; if PayloadLength > 8, += (PayloadLength-1) & ~7 */
int xnet_record_size(int payloadLen)
{
    if (payloadLen < 0 || payloadLen > 64) return -1;
    return CANTP_RECORD_MIN + (payloadLen > 8 ? ((payloadLen - 1) & ~7) : 0);
}

int xnet_write_record(uint8_t* out, int outLen, uint64_t ts, uint32_t id, int ext,
                      int type, const uint8_t* payload, int payloadLen)
{
    int size = xnet_record_size(payloadLen);
    if (size < 0) return CANTP_ERR_ARG;
    if (!out || outLen < size) return CANTP_ERR_BUFFER;
    memset(out, 0, (size_t)size);
    xnet_put_u64(out, ts);
    xnet_put_u32(out + 8, (id & 0x1FFFFFFFu) | (ext ? CANTP_XNET_EXTENDED_ID_FLAG : 0u));
    out[12] = (uint8_t)type;
    out[13] = 0;
    out[14] = 0;
    out[15] = (uint8_t)payloadLen;
    if (payloadLen > 0) memcpy(out + 16, payload, (size_t)payloadLen);
    return size;
}

CANTP_API int32_t CanTp_RecordSizeFor(int32_t payloadLen)
{
    int s = xnet_record_size(payloadLen);
    return s < 0 ? CANTP_ERR_ARG : s;
}

CANTP_API int32_t CanTp_RecordSize(const uint8_t* rec, int32_t avail)
{
    if (!rec || avail < CANTP_RECORD_MIN) return CANTP_ERR_RECORD;
    int s = xnet_record_size(rec[15]);
    if (s < 0 || s > avail) return CANTP_ERR_RECORD;
    return s;
}

CANTP_API int32_t CanTp_MakeRecord(uint32_t id, int32_t extended, int32_t frameType,
                                   const uint8_t* payload, int32_t payloadLen,
                                   uint64_t timestamp100ns, uint8_t* out, int32_t outLen)
{
    if ((payloadLen > 0 && !payload) || payloadLen < 0 || payloadLen > 64) return CANTP_ERR_ARG;
    if (frameType != XNET_TYPE_CAN_DATA && frameType != XNET_TYPE_CANFD_DATA && frameType != XNET_TYPE_CANFDBRS)
        return CANTP_ERR_ARG;
    if (frameType == XNET_TYPE_CAN_DATA && payloadLen > 8) return CANTP_ERR_ARG;
    if (id > (extended ? 0x1FFFFFFFu : 0x7FFu)) return CANTP_ERR_ARG;
    return xnet_write_record(out, outLen, timestamp100ns, id, extended != 0, frameType, payload, payloadLen);
}

CANTP_API int32_t CanTp_NclHeader(uint8_t* out, int32_t outLen)
{
    static const uint8_t hdr[CANTP_NCL_HEADER_SIZE] =
        { 0x4E, 0x49, 0x00, 0x03, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00 };
    if (!out || outLen < CANTP_NCL_HEADER_SIZE) return CANTP_ERR_ARG;
    memcpy(out, hdr, sizeof hdr);
    return CANTP_OK;
}
