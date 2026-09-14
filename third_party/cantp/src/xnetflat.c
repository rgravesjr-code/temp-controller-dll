/* xnetflat.c - v1.3.0: the flattened LabVIEW "XNET Frame CAN" cluster array.
 *
 * LabVIEW "Flatten To String" of a 1-D array of the NI-XNET "XNET Frame CAN"
 * cluster (big-endian, sizes prepended - the defaults) is
 *
 *   I32 count, then per frame (27 + payload bytes):
 *     I64 timestamp seconds since 1904-01-01 00:00:00 UTC
 *     U64 timestamp fraction of a second (units of 2^-64)
 *     I32 payload length, U8 payload[length]
 *     U32 identifier (bare 11- or 29-bit)
 *     U8  type (0 CAN Data, 1 Remote, 2 Bus Error, 8 CAN 2.0 Data, 16 CAN FD,
 *              24 CAN FD+BRS, 192 J1939 Data, 224 Delay, 225 Log Trigger,
 *              226 Start Trigger)
 *     U8  extended? (0/1)
 *     U8  echo? (0/1)
 *
 * The element order is the cluster's type order (from the .ctl type
 * descriptor), which is not the front-panel order. Verified byte-exact
 * against two samples flattened in LabVIEW (tests/fixtures/xnet_*.bin).
 */
#include "cantp_internal.h"

#define LV_EPOCH_OFFSET_S 9561628800ull        /* seconds from 1601-01-01 to 1904-01-01 */
#define XF_HEAD           27                   /* bytes of one frame without its payload */
#define XF_TMP_RECORDS    (CANTP_RECORD_MIN * 256 + 64)   /* records of the longest BAM (1 CM + 255 DT) */

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_be64(uint8_t* p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

/* ---- timestamps: 100 ns since 1601 (record) <-> LabVIEW seconds + 2^-64 fraction since 1904 ---- */
CANTP_API int32_t CanTp_TimeToLabView(uint64_t timestamp100ns, int64_t* seconds, uint64_t* fraction)
{
    if (!seconds || !fraction) return CANTP_ERR_ARG;
    if (timestamp100ns == 0) { *seconds = 0; *fraction = 0; return CANTP_OK; }   /* 0 = unset, both ways */
    uint64_t s = timestamp100ns / 10000000ull, rem = timestamp100ns % 10000000ull;
    *seconds = (int64_t)s - (int64_t)LV_EPOCH_OFFSET_S;
    /* rem * 2^64 / 1e7 exactly: 2^64 = 1844674407370 * 1e7 + 9551616 */
    *fraction = rem * 1844674407370ull + (rem * 9551616ull) / 10000000ull;
    return CANTP_OK;
}

CANTP_API uint64_t CanTp_TimeFromLabView(int64_t seconds, uint64_t fraction)
{
    if (seconds == 0 && fraction == 0) return 0;
    int64_t s = seconds + (int64_t)LV_EPOCH_OFFSET_S;
    if (s < 0) return 0;
    uint64_t hi = fraction >> 32, lo = fraction & 0xFFFFFFFFull;
    uint64_t t = hi * 10000000ull + ((lo * 10000000ull) >> 32);   /* fraction * 1e7 / 2^32 */
    uint64_t sub = (t + (1ull << 31)) >> 32;                        /* / 2^32, rounded */
    if (sub >= 10000000ull) { sub -= 10000000ull; s++; }
    return (uint64_t)s * 10000000ull + sub;
}

/* ---- one frame ---------------------------------------------------------- */
typedef struct {
    uint64_t       ts;
    uint32_t       id;
    int            ext, echo, type;
    const uint8_t* payload;
    int32_t        plen;
} XnetFrame;

/* Size of the frame at p, or CANTP_ERR_RECORD. */
static int32_t xf_frame_size(const uint8_t* p, int32_t avail)
{
    if (!p || avail < XF_HEAD) return CANTP_ERR_RECORD;
    int32_t plen = (int32_t)be32(p + 16);
    if (plen < 0 || plen > CANTP_MAX_PAYLOAD || avail - XF_HEAD < plen) return CANTP_ERR_RECORD;
    return XF_HEAD + plen;
}

static int32_t xf_parse(const uint8_t* p, int32_t avail, XnetFrame* f)
{
    int32_t size = xf_frame_size(p, avail);
    if (size < 0) return size;
    f->ts      = CanTp_TimeFromLabView((int64_t)be64(p), be64(p + 8));
    f->plen    = size - XF_HEAD;
    f->payload = p + 20;
    f->id      = be32(p + 20 + f->plen);
    f->type    = p[24 + f->plen];
    f->ext     = p[25 + f->plen] != 0;
    f->echo    = p[26 + f->plen] != 0;
    return size;
}

/* Writes the frame when it fits; always returns its size. */
static int32_t xf_write(uint8_t* out, int32_t outLen, uint64_t ts, uint32_t id, int ext, int type, int echo,
                        const uint8_t* payload, int32_t plen)
{
    int32_t size = XF_HEAD + plen;
    if (!out || outLen < size) return size;
    int64_t sec; uint64_t frac;
    CanTp_TimeToLabView(ts, &sec, &frac);
    put_be64(out, (uint64_t)sec);
    put_be64(out + 8, frac);
    put_be32(out + 16, (uint32_t)plen);
    if (plen > 0) memcpy(out + 20, payload, (size_t)plen);
    put_be32(out + 20 + plen, id & 0x1FFFFFFFu);
    out[24 + plen] = (uint8_t)type;
    out[25 + plen] = (uint8_t)(ext ? 1 : 0);
    out[26 + plen] = (uint8_t)(echo ? 1 : 0);
    return size;
}

/* ---- array walkers -------------------------------------------------------- */
/* Validates the whole array; returns the frame count or CANTP_ERR_RECORD. */
CANTP_API int32_t CanTp_XnetFrameCount(const uint8_t* xnet, int32_t xnetLen)
{
    if (!xnet || xnetLen < 4) return CANTP_ERR_RECORD;
    int32_t count = (int32_t)be32(xnet), off = 4;
    if (count < 0) return CANTP_ERR_RECORD;
    for (int32_t i = 0; i < count; i++) {
        int32_t size = xf_frame_size(xnet + off, xnetLen - off);
        if (size < 0) return CANTP_ERR_RECORD;
        off += size;
    }
    return count;
}

CANTP_API int32_t CanTp_RecordsToXnet(const uint8_t* frames, int32_t framesLen,
                                      uint8_t* xnet, int32_t xnetLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!frames && framesLen > 0) || framesLen < 0 || (!xnet && xnetLen > 0) || xnetLen < 0) return CANTP_ERR_ARG;
    int32_t off = 0, need = 4, n = 0;
    while (off < framesLen) {                             /* pass 1: validate, size */
        int size = CanTp_RecordSize(frames + off, framesLen - off);
        if (size < 0) return CANTP_ERR_RECORD;
        need += XF_HEAD + frames[off + 15];
        n++;
        off += size;
    }
    *bytesWritten = need;
    if (!xnet || xnetLen < need) return CANTP_ERR_BUFFER;
    put_be32(xnet, (uint32_t)n);
    int32_t w = 4;
    for (off = 0; off < framesLen; ) {                    /* pass 2: convert */
        const uint8_t* r = frames + off;
        int size = CanTp_RecordSize(r, framesLen - off);
        uint32_t raw = xnet_get_u32(r + 8);
        w += xf_write(xnet + w, xnetLen - w, xnet_get_u64(r), raw & 0x1FFFFFFFu,
                      (raw & CANTP_XNET_EXTENDED_ID_FLAG) != 0, r[12], 0, r + 16, r[15]);
        off += size;
    }
    return n;
}

CANTP_API int32_t CanTp_XnetToRecords(const uint8_t* xnet, int32_t xnetLen,
                                      uint8_t* frames, int32_t framesLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || !xnet || xnetLen < 0 || (!frames && framesLen > 0) || framesLen < 0) return CANTP_ERR_ARG;
    int32_t count = CanTp_XnetFrameCount(xnet, xnetLen);
    if (count < 0) return count;
    int32_t off = 4, need = 0;
    for (int32_t i = 0; i < count; i++) {                 /* pass 1: size; > 64 bytes has no record form */
        int32_t size = xf_frame_size(xnet + off, xnetLen - off);
        int32_t plen = size - XF_HEAD;
        if (plen > 64) return CANTP_ERR_RECORD;
        need += xnet_record_size(plen);
        off += size;
    }
    *bytesWritten = need;
    if (!frames || framesLen < need) return CANTP_ERR_BUFFER;
    int32_t w = 0;
    off = 4;
    for (int32_t i = 0; i < count; i++) {                 /* pass 2: convert, type byte verbatim */
        XnetFrame f;
        off += xf_parse(xnet + off, xnetLen - off, &f);
        w += xnet_write_record(frames + w, framesLen - w, f.ts, f.id, f.ext, f.type, f.payload, f.plen);
    }
    return count;
}

/* ---- one-call read/write on the cluster array ------------------------------ */
static int32_t xnet_write_j1939(CanTpMsg* m, const double* values, int32_t nValues, uint64_t ts,
                                uint8_t* xnet, int32_t xnetLen, int32_t* bytesUsed, int32_t* nFrames)
{
    if (m->transport != CANTP_TP_J1939_BAM && m->transport != CANTP_TP_J1939_RTS) return CANTP_ERR_TRANSPORT;
    uint8_t payload[CANTP_MAX_PAYLOAD];
    int32_t rc = pack_values(m, values, nValues, payload);
    if (rc != CANTP_OK) return rc;
    uint32_t id = j1939_id(pgn_of_id(m->id), m->sa, m->da, (uint8_t)((m->id >> 26) & 7u));
    int32_t need = 4 + XF_HEAD + m->len;
    *bytesUsed = need;
    *nFrames = 1;
    if (!xnet || xnetLen < need) return CANTP_ERR_BUFFER;
    put_be32(xnet, 1);
    xf_write(xnet + 4, xnetLen - 4, ts, id, 1, CANTP_XNET_TYPE_J1939_DATA, 0, payload, m->len);
    return CANTP_OK;
}

/* Stateless read (the live RxFeed state is untouched), like CanTp_Unpack:
 * J1939 Data frames are decoded whole, CAN data frames go through the
 * record decoder, everything else (remote, error, delay, triggers) is skipped. */
static int32_t xnet_read(CanTpMsg* m, const uint8_t* xnet, int32_t xnetLen, double* values, int32_t nValues,
                         int32_t* bytesUsed, int32_t* nFrames)
{
    int32_t count = CanTp_XnetFrameCount(xnet, xnetLen);
    if (count < 0) return count;
    CanTpRx saved = m->rx;
    memset(&m->rx, 0, sizeof m->rx);
    int32_t off = 4, cnt = 0, rc = CANTP_OK;
    while (cnt < count) {
        XnetFrame f;
        off += xf_parse(xnet + off, xnetLen - off, &f);
        cnt++;
        if (f.type == CANTP_XNET_TYPE_J1939_DATA) {
            uint32_t raw = f.id | (f.ext ? CANTP_XNET_EXTENDED_ID_FLAG : 0u);
            if (id_matches(m, raw) && f.plen >= m->len) {
                decode_values(m, f.payload, values, nValues);
                rc = CANTP_FOUND;
                break;
            }
            continue;
        }
        if (f.plen > 64) continue;
        uint8_t rec[CANTP_RECORD_MIN + 56];
        int size = xnet_write_record(rec, sizeof rec, f.ts, f.id, f.ext, f.type, f.payload, f.plen);
        rc = cantp_feed_record(m, rec, size, values, nValues);
        if (rc != CANTP_OK) break;
    }
    m->rx = saved;
    *bytesUsed = off; *nFrames = cnt;
    return rc;
}

CANTP_API int32_t CanTp_TransferXnet(int32_t slot, int32_t mode, double* values, int32_t nValues,
                                     uint8_t* xnet, int32_t xnetLen, int32_t xnetMode,
                                     uint64_t timestamp100ns, uint64_t spacing100ns,
                                     int32_t* bytesUsed, int32_t* nFrames)
{
    if (bytesUsed) *bytesUsed = 0;
    if (nFrames) *nFrames = 0;
    if (!bytesUsed || !nFrames || (!values && nValues > 0) || nValues < 0) return CANTP_ERR_ARG;
    if ((!xnet && xnetLen > 0) || xnetLen < 0) return CANTP_ERR_ARG;
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return CANTP_ERR_SLOT;
    CanTpMsg* m = cantp_slot(slot);
    if (!m->used) return CANTP_ERR_NOT_DEFINED;

    if (mode == CANTP_MODE_WRITE) {
        if (xnetMode == CANTP_XNET_J1939)
            return xnet_write_j1939(m, values, nValues, timestamp100ns, xnet, xnetLen, bytesUsed, nFrames);
        if (xnetMode != CANTP_XNET_FRAMES) return CANTP_ERR_ARG;
        uint8_t rec[XF_TMP_RECORDS];
        int32_t used = 0;
        int32_t rc = CanTp_Pack(slot, values, nValues, timestamp100ns, spacing100ns, rec, (int32_t)sizeof rec, &used);
        if (rc != CANTP_OK) return rc;
        int32_t n = CanTp_RecordsToXnet(rec, used, xnet, xnetLen, bytesUsed);
        if (n < 0) {
            if (n == CANTP_ERR_BUFFER) *nFrames = CanTp_FrameLengths(rec, used, NULL, 0);
            return n;
        }
        *nFrames = n;
        return CANTP_OK;
    }
    if (mode == CANTP_MODE_READ)
        return xnet_read(m, xnet, xnetLen, values, nValues, bytesUsed, nFrames);
    return CANTP_ERR_ARG;
}

CANTP_API int32_t CanTp_TransferXnetSgl(int32_t slot, int32_t mode, float* values, int32_t nValues,
                                        uint8_t* xnet, int32_t xnetLen, int32_t xnetMode,
                                        uint64_t timestamp100ns, uint64_t spacing100ns,
                                        int32_t* bytesUsed, int32_t* nFrames)
{
    if (nValues < 0 || nValues > CANTP_MAX_SIGNALS || (!values && nValues > 0)) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    if (mode == CANTP_MODE_WRITE) for (int32_t i = 0; i < nValues; i++) tmp[i] = values[i];
    int32_t rc = CanTp_TransferXnet(slot, mode, tmp, nValues, xnet, xnetLen, xnetMode,
                                    timestamp100ns, spacing100ns, bytesUsed, nFrames);
    if (mode == CANTP_MODE_READ && rc == CANTP_FOUND) for (int32_t i = 0; i < nValues; i++) values[i] = (float)tmp[i];
    return rc;
}
