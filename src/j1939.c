/* j1939.c - J1939-21 BAM transport, NI-XNET logfile header, temp-controller encoder. */
#include "xnet_raw.h"

#define J1939_TP_MAX      1785
#define J1939_TP_CM_PGN   0xEC00u
#define J1939_TP_DT_PGN   0xEB00u
#define J1939_TP_PRIORITY 7u
#define J1939_GLOBAL_DA   0xFFu
#define J1939_BAM_CTRL    0x20u

/* 29-bit identifier for a PGN sent from `sa`. PDU1 PGNs get DA = global. */
static uint32_t j1939_id(uint32_t pgn, uint8_t sa, uint8_t priority)
{
    pgn &= 0x3FFFFu;                          /* EDP, DP, PF, PS */
    uint32_t pf = (pgn >> 8) & 0xFFu;
    if (pf < 0xF0u) pgn = (pgn & 0x3FF00u) | J1939_GLOBAL_DA;   /* PDU1: PS = destination */
    return ((uint32_t)(priority & 7u) << 26) | (pgn << 8) | sa;
}

TC_API int32_t TcNclHeader(uint8_t* out, int32_t outLen)
{
    static const uint8_t hdr[TC_NCL_HEADER_SIZE] =
        { 0x4E, 0x49, 0x00, 0x03, 0x01, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00 };
    if (!out || outLen < TC_NCL_HEADER_SIZE) return TC_ERR_ARG;
    memcpy(out, hdr, sizeof hdr);
    return TC_OK;
}

TC_API int32_t TcJ1939BamFrameCount(int32_t payloadLen)
{
    if (payloadLen < 0 || payloadLen > J1939_TP_MAX) return 0;
    if (payloadLen <= 8) return 1;
    return 1 + (payloadLen + 6) / 7;
}

TC_API int32_t TcJ1939Bam(uint32_t pgn, uint8_t sa, uint8_t priority,
                          const uint8_t* payload, int32_t payloadLen,
                          uint64_t timestamp100ns, uint64_t spacing100ns,
                          uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!payload || payloadLen < 0 || !bytesWritten) return TC_ERR_ARG;
    if (payloadLen > J1939_TP_MAX) return TC_ERR_PAYLOAD;

    int32_t nFrames = TcJ1939BamFrameCount(payloadLen);
    int32_t need = nFrames * TC_RAW_FRAME_SIZE;
    if (!out || outLen < need) { *bytesWritten = need; return TC_ERR_BUFFER; }

    uint8_t data[8];
    uint8_t* p = out;
    uint64_t ts = timestamp100ns;

    if (payloadLen <= 8) {
        xnet_raw_frame(p, ts, j1939_id(pgn, sa, priority), 1, payload, payloadLen);
        *bytesWritten = need;
        return TC_OK;
    }

    int32_t nPackets = nFrames - 1;
    /* TP.CM BAM */
    data[0] = J1939_BAM_CTRL;
    put_u16le(data + 1, (uint16_t)payloadLen);
    data[3] = (uint8_t)nPackets;
    data[4] = 0xFF;
    data[5] = (uint8_t)(pgn);
    data[6] = (uint8_t)(pgn >> 8);
    data[7] = (uint8_t)(pgn >> 16);
    xnet_raw_frame(p, ts, j1939_id(J1939_TP_CM_PGN, sa, J1939_TP_PRIORITY), 1, data, 8);
    p += TC_RAW_FRAME_SIZE;

    /* TP.DT packets */
    for (int32_t i = 0; i < nPackets; i++) {
        ts += spacing100ns;
        memset(data, 0xFF, sizeof data);
        data[0] = (uint8_t)(i + 1);
        int32_t off = i * 7;
        int32_t n = payloadLen - off; if (n > 7) n = 7;
        memcpy(data + 1, payload + off, (size_t)n);
        xnet_raw_frame(p, ts, j1939_id(J1939_TP_DT_PGN, sa, J1939_TP_PRIORITY), 1, data, 8);
        p += TC_RAW_FRAME_SIZE;
    }
    *bytesWritten = need;
    return TC_OK;
}

TC_API int32_t TcEncodeFrames(const float* signals, int32_t n,
                              uint32_t pgn, uint8_t sa, uint8_t priority,
                              uint64_t timestamp100ns, uint64_t spacing100ns,
                              uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (!signals || n < 0 || n * 4 > J1939_TP_MAX) return TC_ERR_ARG;
    uint8_t payload[J1939_TP_MAX];
    for (int32_t i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &signals[i], 4);        /* IEEE-754 single, little-endian on the wire */
        put_u32le(payload + 4 * i, bits);
    }
    return TcJ1939Bam(pgn, sa, priority, payload, n * 4, timestamp100ns, spacing100ns,
                      out, outLen, bytesWritten);
}
