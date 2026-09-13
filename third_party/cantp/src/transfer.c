/* transfer.c - frame-length arrays and the one-call read/write entry point (v1.2.0). */
#include "cantp_internal.h"

CANTP_API int32_t CanTp_FrameLengths(const uint8_t* frames, int32_t framesLen,
                                     uint8_t* lens, int32_t lensLen)
{
    if (!frames || framesLen < 0 || (!lens && lensLen > 0) || lensLen < 0) return CANTP_ERR_ARG;
    int32_t off = 0, n = 0;
    while (off < framesLen) {
        int size = CanTp_RecordSize(frames + off, framesLen - off);
        if (size < 0) return CANTP_ERR_RECORD;
        if (n < lensLen) lens[n] = frames[off + 15];
        n++;
        off += size;
    }
    return n;
}

/* Walk `frames` using the caller's per-frame payload lengths when given (they
 * must agree with the record headers), else the headers alone. Stateless. */
static int32_t transfer_read(CanTpMsg* m, const uint8_t* frames, int32_t framesLen,
                             const uint8_t* lens, int32_t nLens, double* values, int32_t nValues,
                             int32_t* bytesUsed, int32_t* nFrames)
{
    CanTpRx saved = m->rx;
    memset(&m->rx, 0, sizeof m->rx);
    int32_t off = 0, cnt = 0, rc = CANTP_OK;
    while (off + CANTP_RECORD_MIN <= framesLen) {
        if (lens && cnt >= nLens) break;             /* caller's list exhausted: stop there */
        int size = CanTp_RecordSize(frames + off, framesLen - off);
        if (size < 0 || (lens && frames[off + 15] != lens[cnt])) { rc = CANTP_ERR_RECORD; break; }
        rc = cantp_feed_record(m, frames + off, size, values, nValues);
        off += size; cnt++;
        if (rc != CANTP_OK) break;
    }
    m->rx = saved;
    *bytesUsed = off; *nFrames = cnt;
    return rc;
}

CANTP_API int32_t CanTp_Transfer(int32_t slot, int32_t mode, double* values, int32_t nValues,
                                 uint8_t* frames, int32_t framesLen, uint8_t* frameLens, int32_t frameLensLen,
                                 uint64_t timestamp100ns, uint64_t spacing100ns,
                                 int32_t* bytesUsed, int32_t* nFrames)
{
    if (bytesUsed) *bytesUsed = 0;
    if (nFrames) *nFrames = 0;
    if (!bytesUsed || !nFrames || (!values && nValues > 0) || nValues < 0) return CANTP_ERR_ARG;
    if ((!frames && framesLen > 0) || framesLen < 0 || (!frameLens && frameLensLen > 0) || frameLensLen < 0) return CANTP_ERR_ARG;
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return CANTP_ERR_SLOT;
    CanTpMsg* m = cantp_slot(slot);
    if (!m->used) return CANTP_ERR_NOT_DEFINED;

    if (mode == CANTP_MODE_WRITE) {
        int32_t rc = CanTp_Pack(slot, values, nValues, timestamp100ns, spacing100ns, frames, framesLen, bytesUsed);
        if (rc != CANTP_OK) return rc;
        int32_t n = CanTp_FrameLengths(frames, *bytesUsed, frameLens, frameLensLen);
        if (n < 0) return n;
        *nFrames = n;
        return (frameLens && n > frameLensLen) ? CANTP_ERR_BUFFER : CANTP_OK;
    }
    if (mode == CANTP_MODE_READ)
        return transfer_read(m, frames, framesLen, frameLensLen > 0 ? frameLens : NULL, frameLensLen,
                             values, nValues, bytesUsed, nFrames);
    return CANTP_ERR_ARG;
}

CANTP_API int32_t CanTp_TransferSgl(int32_t slot, int32_t mode, float* values, int32_t nValues,
                                    uint8_t* frames, int32_t framesLen, uint8_t* frameLens, int32_t frameLensLen,
                                    uint64_t timestamp100ns, uint64_t spacing100ns,
                                    int32_t* bytesUsed, int32_t* nFrames)
{
    if (nValues < 0 || nValues > CANTP_MAX_SIGNALS || (!values && nValues > 0)) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    if (mode == CANTP_MODE_WRITE) for (int32_t i = 0; i < nValues; i++) tmp[i] = values[i];
    int32_t rc = CanTp_Transfer(slot, mode, tmp, nValues, frames, framesLen, frameLens, frameLensLen,
                                timestamp100ns, spacing100ns, bytesUsed, nFrames);
    if (mode == CANTP_MODE_READ && rc == CANTP_FOUND) for (int32_t i = 0; i < nValues; i++) values[i] = (float)tmp[i];
    return rc;
}
