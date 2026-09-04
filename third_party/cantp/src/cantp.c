/* cantp.c - slots, define, pack, unpack, live receive. See cantp.h. */
#include "cantp_internal.h"

static CanTpMsg g_slots[CANTP_MAX_SLOTS];

CANTP_API uint32_t CanTp_Version(void)
{
    return ((uint32_t)CANTP_VERSION_MAJOR << 16) | ((uint32_t)CANTP_VERSION_MINOR << 8) | CANTP_VERSION_PATCH;
}

static CanTpMsg* slot_get(int32_t slot, int mustExist)
{
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return NULL;
    CanTpMsg* m = &g_slots[slot];
    if (mustExist && !m->used) return NULL;
    return m;
}
#define SLOT_OR_RETURN(m, slot)                                              \
    CanTpMsg* m = slot_get(slot, 1);                                         \
    if (!m) return (slot < 0 || slot >= CANTP_MAX_SLOTS) ? CANTP_ERR_SLOT : CANTP_ERR_NOT_DEFINED

/* ------------------------------------------------------------------------ */
/* J1939 helpers                                                             */
/* ------------------------------------------------------------------------ */
static int is_pdu1(uint32_t pgn) { return ((pgn >> 8) & 0xFFu) < 0xF0u; }

static uint32_t pgn_of_id(uint32_t id)
{
    uint32_t pgn = (id >> 8) & 0x3FFFFu;
    if (is_pdu1(pgn)) pgn &= 0x3FF00u;            /* PS is a destination, not part of the PGN */
    return pgn;
}
static uint32_t j1939_id(uint32_t pgn, uint8_t sa, uint8_t da, uint8_t priority)
{
    pgn &= 0x3FFFFu;
    if (is_pdu1(pgn)) pgn = (pgn & 0x3FF00u) | da;
    return ((uint32_t)(priority & 7u) << 26) | (pgn << 8) | sa;
}
static int bam_frames_for(int len) { return len <= 8 ? 1 : 1 + (len + 6) / 7; }

/* ------------------------------------------------------------------------ */
/* Define / queries                                                          */
/* ------------------------------------------------------------------------ */
CANTP_API int32_t CanTp_Define(int32_t slot, const double* msgDef, int32_t msgDefLen,
                               const double* sigDefs, int32_t nSig)
{
    CanTpMsg* m = slot_get(slot, 0);
    if (!m) return CANTP_ERR_SLOT;
    if (!msgDef || msgDefLen < CANTP_MSGDEF_COLS) return CANTP_ERR_ARG;
    if (nSig < 0 || (nSig > 0 && !sigDefs)) return CANTP_ERR_ARG;
    if (nSig > CANTP_MAX_SIGNALS) return CANTP_ERR_TOO_MANY;

    CanTpMsg t;
    memset(&t, 0, sizeof t);
    double idd = msgDef[CANTP_MSG_ID];
    if (idd < 0 || idd > 4294967295.0) return CANTP_ERR_MSGDEF;
    uint32_t id = (uint32_t)idd & 0x1FFFFFFFu;         /* DBC bit 31 (extended marker) ignored */
    t.ext = msgDef[CANTP_MSG_EXTENDED] != 0.0 || idd >= 2147483648.0;   /* DBC bit 31 = extended */
    if (!t.ext && id > 0x7FFu) return CANTP_ERR_MSGDEF;
    t.len = (int)msgDef[CANTP_MSG_LENGTH];
    t.transport = (int)msgDef[CANTP_MSG_TRANSPORT];
    int sa = (int)msgDef[CANTP_MSG_SA];
    int da = (int)msgDef[CANTP_MSG_DA];
    int pad = (int)msgDef[CANTP_MSG_PAD];
    t.cycleMs = msgDef[CANTP_MSG_CYCLE_MS];
    if (sa < -1 || sa > 253 || da < 0 || da > 255 || pad < 0 || pad > 255) return CANTP_ERR_MSGDEF;
    t.da = (uint8_t)da; t.pad = (uint8_t)pad;

    switch (t.transport) {
    case CANTP_TP_CLASSIC:   if (t.len < 0 || t.len > 8)  return CANTP_ERR_MSGDEF; break;
    case CANTP_TP_CANFD:
    case CANTP_TP_CANFD_BRS: if (t.len < 0 || t.len > 64) return CANTP_ERR_MSGDEF; break;
    case CANTP_TP_J1939_BAM:
        if (!t.ext) return CANTP_ERR_MSGDEF;
        if (t.len < 0 || t.len > CANTP_MAX_PAYLOAD) return CANTP_ERR_MSGDEF;
        if (t.da != J1939_GLOBAL_DA) return CANTP_ERR_TRANSPORT;    /* RTS/CTS: release 2 */
        break;
    case CANTP_TP_J1939_RTS:
    case CANTP_TP_ISOTP:     return CANTP_ERR_TRANSPORT;
    default:                 return CANTP_ERR_MSGDEF;
    }

    if (t.ext) {
        if (sa >= 0) { id = (id & ~0xFFu) | (uint32_t)sa; t.sa = (uint8_t)sa; }
        else { t.sa = (uint8_t)(id & 0xFFu); t.saAny = (t.sa == J1939_SA_ANY); }
    } else if (sa >= 0) {
        return CANTP_ERR_MSGDEF;                          /* SA override is a J1939 concept */
    }
    t.id = id;

    int bitsAvail = t.len * 8;
    for (int i = 0; i < nSig; i++) {
        const double* r = sigDefs + i * CANTP_SIGDEF_COLS;
        CanTpSig* s = &t.sig[i];
        s->start = (int)r[CANTP_SIG_START];
        s->len = (int)r[CANTP_SIG_LENGTH];
        s->motorola = r[CANTP_SIG_ORDER] != 0.0;
        s->type = (int)r[CANTP_SIG_TYPE];
        s->factor = r[CANTP_SIG_FACTOR];
        s->offset = r[CANTP_SIG_OFFSET];
        s->min = r[CANTP_SIG_MIN];
        s->max = r[CANTP_SIG_MAX];
        if (s->len < 1 || s->len > 64 || s->start < 0) return CANTP_ERR_SIGDEF;
        if (s->factor == 0.0) return CANTP_ERR_SIGDEF;
        if (s->type < 0 || s->type > 3) return CANTP_ERR_SIGDEF;
        if ((s->type == 2 && s->len != 32) || (s->type == 3 && s->len != 64)) return CANTP_ERR_SIGDEF;
        if (bits_last_pos(s->start, s->len, s->motorola) >= bitsAvail) return CANTP_ERR_SIGDEF;
        if (s->start >= bitsAvail) return CANTP_ERR_SIGDEF;
    }
    t.nSig = nSig;
    t.used = 1;
    *m = t;
    return CANTP_OK;
}

CANTP_API int32_t CanTp_Clear(int32_t slot)
{
    CanTpMsg* m = slot_get(slot, 0);
    if (!m) return CANTP_ERR_SLOT;
    memset(m, 0, sizeof *m);
    return CANTP_OK;
}

CANTP_API int32_t CanTp_SignalCount(int32_t slot)   { SLOT_OR_RETURN(m, slot); return m->nSig; }
CANTP_API int32_t CanTp_PayloadLength(int32_t slot) { SLOT_OR_RETURN(m, slot); return m->len; }

static int frame_count(const CanTpMsg* m)
{
    return m->transport == CANTP_TP_J1939_BAM ? bam_frames_for(m->len) : 1;
}
static int output_size(const CanTpMsg* m)
{
    switch (m->transport) {
    case CANTP_TP_CLASSIC:   return xnet_record_size(m->len);
    case CANTP_TP_CANFD:
    case CANTP_TP_CANFD_BRS: return xnet_record_size(canfd_pad_len(m->len));
    default:                 return bam_frames_for(m->len) * CANTP_RECORD_MIN;
    }
}
CANTP_API int32_t CanTp_FrameCount(int32_t slot) { SLOT_OR_RETURN(m, slot); return frame_count(m); }
CANTP_API int32_t CanTp_OutputSize(int32_t slot) { SLOT_OR_RETURN(m, slot); return output_size(m); }

/* ------------------------------------------------------------------------ */
/* Pack                                                                      */
/* ------------------------------------------------------------------------ */
static int32_t pack_values(const CanTpMsg* m, const double* values, int32_t nValues, uint8_t* payload)
{
    if (nValues < m->nSig) return CANTP_ERR_ARG;
    memset(payload, m->pad, (size_t)m->len);   /* unused bits = pad; bits_place overwrites signal bits */
    for (int i = 0; i < m->nSig; i++) {
        uint64_t raw;
        int rc = bits_encode(&m->sig[i], values[i], &raw);
        if (rc != CANTP_OK) return rc;
        bits_place(payload, m->sig[i].start, m->sig[i].len, m->sig[i].motorola, raw);
    }
    return CANTP_OK;
}

static int32_t emit_frames(const CanTpMsg* m, const uint8_t* payload, uint64_t ts, uint64_t spacing,
                           uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    int need = output_size(m);
    if (!out || outLen < need) { *bytesWritten = need; return CANTP_ERR_BUFFER; }

    if (m->transport == CANTP_TP_CLASSIC) {
        xnet_write_record(out, outLen, ts, m->id, m->ext, XNET_TYPE_CAN_DATA, payload, m->len);
        *bytesWritten = need;
        return CANTP_OK;
    }
    if (m->transport == CANTP_TP_CANFD || m->transport == CANTP_TP_CANFD_BRS) {
        uint8_t buf[64];
        int plen = canfd_pad_len(m->len);
        memset(buf, m->pad, sizeof buf);
        memcpy(buf, payload, (size_t)m->len);
        xnet_write_record(out, outLen, ts, m->id, m->ext,
                          m->transport == CANTP_TP_CANFD ? XNET_TYPE_CANFD_DATA : XNET_TYPE_CANFDBRS, buf, plen);
        *bytesWritten = need;
        return CANTP_OK;
    }

    /* J1939 BAM */
    uint32_t pgn = pgn_of_id(m->id);
    uint8_t* p = out;
    if (m->len <= 8) {                                   /* single frame under the PGN, DA applied for PDU1 */
        uint8_t buf[8]; memset(buf, m->pad, 8); memcpy(buf, payload, (size_t)m->len);
        uint32_t id = j1939_id(pgn, m->sa, m->da, (uint8_t)((m->id >> 26) & 7u));
        xnet_write_record(p, outLen, ts, id, 1, XNET_TYPE_CAN_DATA, buf, m->len);
        *bytesWritten = need;
        return CANTP_OK;
    }
    int nPackets = bam_frames_for(m->len) - 1;
    uint8_t d[8];
    d[0] = J1939_BAM_CTRL;
    xnet_put_u16(d + 1, (uint16_t)m->len);
    d[3] = (uint8_t)nPackets;
    d[4] = 0xFF;
    d[5] = (uint8_t)pgn; d[6] = (uint8_t)(pgn >> 8); d[7] = (uint8_t)(pgn >> 16);
    xnet_write_record(p, outLen, ts, j1939_id(J1939_TP_CM_PGN, m->sa, m->da, J1939_TP_PRIORITY), 1,
                      XNET_TYPE_CAN_DATA, d, 8);
    p += CANTP_RECORD_MIN;
    for (int i = 0; i < nPackets; i++) {
        ts += spacing;
        memset(d, m->pad, 8);
        d[0] = (uint8_t)(i + 1);
        int off = i * 7, n = m->len - off; if (n > 7) n = 7;
        memcpy(d + 1, payload + off, (size_t)n);
        xnet_write_record(p, CANTP_RECORD_MIN, ts, j1939_id(J1939_TP_DT_PGN, m->sa, m->da, J1939_TP_PRIORITY), 1,
                          XNET_TYPE_CAN_DATA, d, 8);
        p += CANTP_RECORD_MIN;
    }
    *bytesWritten = need;
    return CANTP_OK;
}

CANTP_API int32_t CanTp_Pack(int32_t slot, const double* values, int32_t nValues,
                             uint64_t timestamp100ns, uint64_t spacing100ns,
                             uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!values && nValues > 0) || nValues < 0) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    uint8_t payload[CANTP_MAX_PAYLOAD];
    int32_t rc = pack_values(m, values, nValues, payload);
    if (rc != CANTP_OK) return rc;
    return emit_frames(m, payload, timestamp100ns, spacing100ns, out, outLen, bytesWritten);
}

CANTP_API int32_t CanTp_PackSgl(int32_t slot, const float* values, int32_t nValues,
                                uint64_t timestamp100ns, uint64_t spacing100ns,
                                uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!values && nValues > 0) || nValues < 0 || nValues > CANTP_MAX_SIGNALS) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    for (int32_t i = 0; i < nValues; i++) tmp[i] = values[i];
    return CanTp_Pack(slot, tmp, nValues, timestamp100ns, spacing100ns, out, outLen, bytesWritten);
}

/* ------------------------------------------------------------------------ */
/* Unpack / receive                                                          */
/* ------------------------------------------------------------------------ */
static void decode_values(const CanTpMsg* m, const uint8_t* payload, double* values, int32_t nValues)
{
    for (int i = 0; i < m->nSig && i < nValues; i++)
        values[i] = bits_decode(&m->sig[i], bits_extract(payload, m->sig[i].start, m->sig[i].len, m->sig[i].motorola));
}

/* Does this record's id carry the slot's message (single-frame case)? */
static int id_matches(const CanTpMsg* m, uint32_t rawId)
{
    int ext = (rawId & CANTP_XNET_EXTENDED_ID_FLAG) != 0;
    uint32_t id = rawId & 0x1FFFFFFFu;
    if (ext != m->ext) return 0;
    if (!m->ext) return id == m->id;
    if (m->transport == CANTP_TP_J1939_BAM || m->saAny) {
        /* J1939: compare PGN (and priority-less), any SA when placeholder */
        if (pgn_of_id(id) != pgn_of_id(m->id)) return 0;
        return m->saAny || (id & 0xFFu) == m->sa;
    }
    return id == m->id;
}

/* Feed one record into the slot's decoder. Returns CANTP_FOUND when complete. */
static int32_t feed_record(CanTpMsg* m, const uint8_t* rec, int recLen, double* values, int32_t nValues)
{
    int size = CanTp_RecordSize(rec, recLen);
    if (size < 0) return CANTP_ERR_RECORD;
    uint32_t rawId = xnet_get_u32(rec + 8);
    int plen = rec[15];
    const uint8_t* data = rec + 16;
    int type = rec[12];
    if (type != XNET_TYPE_CAN_DATA && type != XNET_TYPE_CANFD_DATA && type != XNET_TYPE_CANFDBRS) return CANTP_OK;

    /* single-frame messages (classic, FD, short BAM) */
    if (m->transport != CANTP_TP_J1939_BAM || m->len <= 8) {
        if (!id_matches(m, rawId)) return CANTP_OK;
        if (plen < m->len) return CANTP_OK;                  /* too short to hold the signals */
        decode_values(m, data, values, nValues);
        return CANTP_FOUND;
    }

    /* J1939 transport: TP.CM / TP.DT from the expected sender */
    if (!(rawId & CANTP_XNET_EXTENDED_ID_FLAG)) return CANTP_OK;
    uint32_t id = rawId & 0x1FFFFFFFu;
    uint32_t pgn = pgn_of_id(id);
    uint8_t sa = (uint8_t)(id & 0xFFu);
    uint8_t da = (uint8_t)((id >> 8) & 0xFFu);
    if (!m->saAny && sa != m->sa) return CANTP_OK;
    if (plen < 8) return CANTP_OK;

    if (pgn == J1939_TP_CM_PGN) {
        if (data[0] != J1939_BAM_CTRL || da != J1939_GLOBAL_DA) return CANTP_OK;   /* RTS etc.: release 2 */
        uint32_t msgPgn = (uint32_t)data[5] | ((uint32_t)data[6] << 8) | ((uint32_t)data[7] << 16);
        if (msgPgn != pgn_of_id(m->id)) return CANTP_OK;
        int total = data[1] | (data[2] << 8);
        int packets = data[3];
        if (total < 9 || total > CANTP_MAX_PAYLOAD || packets != (total + 6) / 7) { m->rx.total = 0; return CANTP_OK; }
        m->rx.total = total; m->rx.packets = packets; m->rx.nextSeq = 1; m->rx.sa = sa;
        return CANTP_OK;
    }
    if (pgn == J1939_TP_DT_PGN && m->rx.total > 0 && sa == m->rx.sa) {
        int seq = data[0];
        if (seq != m->rx.nextSeq) { m->rx.total = 0; return CANTP_OK; }   /* lost packet: abandon */
        int off = (seq - 1) * 7, n = m->rx.total - off; if (n > 7) n = 7;
        memcpy(m->rx.buf + off, data + 1, (size_t)n);
        if (seq == m->rx.packets) {
            m->rx.total = 0;
            if (m->len > CANTP_MAX_PAYLOAD) return CANTP_OK;
            /* decode from what was received; a shorter-than-defined message is still decoded
               (missing bytes read as pad) */
            uint8_t payload[CANTP_MAX_PAYLOAD];
            memset(payload, m->pad, sizeof payload);
            memcpy(payload, m->rx.buf, (size_t)(off + n));
            decode_values(m, payload, values, nValues);
            return CANTP_FOUND;
        }
        m->rx.nextSeq++;
    }
    return CANTP_OK;
}

CANTP_API int32_t CanTp_RxFeed(int32_t slot, const uint8_t* frame, int32_t frameLen,
                               double* values, int32_t nValues)
{
    if (!frame || (!values && nValues > 0) || nValues < 0) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    return feed_record(m, frame, frameLen, values, nValues);
}

CANTP_API int32_t CanTp_RxReset(int32_t slot)
{
    SLOT_OR_RETURN(m, slot);
    memset(&m->rx, 0, sizeof m->rx);
    return CANTP_OK;
}

CANTP_API int32_t CanTp_Unpack(int32_t slot, const uint8_t* frames, int32_t framesLen,
                               double* values, int32_t nValues, int32_t* bytesConsumed)
{
    if (bytesConsumed) *bytesConsumed = 0;
    if (!frames || framesLen < 0 || (!values && nValues > 0) || nValues < 0 || !bytesConsumed) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    CanTpRx saved = m->rx;                       /* stateless: do not disturb the live decoder */
    memset(&m->rx, 0, sizeof m->rx);
    int32_t off = 0, rc = CANTP_OK;
    while (off + CANTP_RECORD_MIN <= framesLen) {
        int size = CanTp_RecordSize(frames + off, framesLen - off);
        if (size < 0) { rc = CANTP_ERR_RECORD; break; }
        rc = feed_record(m, frames + off, size, values, nValues);
        off += size;
        if (rc != CANTP_OK) break;
    }
    m->rx = saved;
    *bytesConsumed = off;
    return rc;
}

CANTP_API int32_t CanTp_UnpackSgl(int32_t slot, const uint8_t* frames, int32_t framesLen,
                                  float* values, int32_t nValues, int32_t* bytesConsumed)
{
    if (nValues < 0 || nValues > CANTP_MAX_SIGNALS) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    int32_t rc = CanTp_Unpack(slot, frames, framesLen, tmp, nValues, bytesConsumed);
    if (rc == CANTP_FOUND && values) for (int32_t i = 0; i < nValues; i++) values[i] = (float)tmp[i];
    return rc;
}
