/* cantp.c - slots, define, pack, unpack, live receive, session entry points. See cantp.h. */
#include "cantp_internal.h"

static CanTpMsg g_slots[CANTP_MAX_SLOTS];

CANTP_API uint32_t CanTp_Version(void)
{
    return ((uint32_t)CANTP_VERSION_MAJOR << 16) | ((uint32_t)CANTP_VERSION_MINOR << 8) | CANTP_VERSION_PATCH;
}

CanTpMsg* cantp_slot(int32_t slot) { return &g_slots[slot]; }

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

static double quiet_nan(void) { uint64_t b = 0x7FF8000000000000ULL; double d; memcpy(&d, &b, 8); return d; }

/* ------------------------------------------------------------------------ */
/* J1939 helpers                                                             */
/* ------------------------------------------------------------------------ */
int is_pdu1(uint32_t pgn) { return ((pgn >> 8) & 0xFFu) < 0xF0u; }

uint32_t pgn_of_id(uint32_t id)
{
    uint32_t pgn = (id >> 8) & 0x3FFFFu;
    if (is_pdu1(pgn)) pgn &= 0x3FF00u;            /* PS is a destination, not part of the PGN */
    return pgn;
}
uint32_t j1939_id(uint32_t pgn, uint8_t sa, uint8_t da, uint8_t priority)
{
    pgn &= 0x3FFFFu;
    if (is_pdu1(pgn)) pgn = (pgn & 0x3FF00u) | da;
    return ((uint32_t)(priority & 7u) << 26) | (pgn << 8) | sa;
}
static int bam_frames_for(int len) { return len <= 8 ? 1 : 1 + (len + 6) / 7; }

/* DT (J1939) or CF (ISO-TP) packets carrying the message, 0 for single-frame messages. */
int packets_for(const CanTpMsg* m)
{
    switch (m->transport) {
    case CANTP_TP_J1939_BAM:
    case CANTP_TP_J1939_RTS: return m->len <= 8 ? 0 : (m->len + 6) / 7;
    case CANTP_TP_ISOTP:     return m->len <= 7 ? 0 : (m->len - 6 + 6) / 7;
    default:                 return 0;
    }
}

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
        if (t.da != J1939_GLOBAL_DA) return CANTP_ERR_MSGDEF;      /* destination-specific: use RTS/CTS */
        break;
    case CANTP_TP_J1939_RTS:
        if (!t.ext) return CANTP_ERR_MSGDEF;
        if (t.len < 0 || t.len > CANTP_MAX_PAYLOAD) return CANTP_ERR_MSGDEF;
        if (t.da == J1939_GLOBAL_DA) return CANTP_ERR_MSGDEF;      /* RTS/CTS needs a real destination */
        break;
    case CANTP_TP_ISOTP:
        if (t.len < 0 || t.len > CANTP_MAX_PAYLOAD) return CANTP_ERR_MSGDEF;
        break;
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
        s->muxRow = -1;
        s->muxValue = 0;
        if (s->len < 1 || s->len > 64 || s->start < 0) return CANTP_ERR_SIGDEF;
        if (s->factor == 0.0) return CANTP_ERR_SIGDEF;
        if (s->type < 0 || s->type > 3) return CANTP_ERR_SIGDEF;
        if ((s->type == 2 && s->len != 32) || (s->type == 3 && s->len != 64)) return CANTP_ERR_SIGDEF;
        if (bits_last_pos(s->start, s->len, s->motorola) >= bitsAvail) return CANTP_ERR_SIGDEF;
        if (s->start >= bitsAvail) return CANTP_ERR_SIGDEF;
    }
    t.nSig = nSig;
    t.used = 1;
    session_defaults(&t);
    *m = t;
    return CANTP_OK;
}

CANTP_API int32_t CanTp_DefineMux(int32_t slot, const double* muxDefs, int32_t nSig)
{
    SLOT_OR_RETURN(m, slot);
    if (!muxDefs || nSig != m->nSig) return CANTP_ERR_ARG;
    int rows[CANTP_MAX_SIGNALS];
    uint64_t vals[CANTP_MAX_SIGNALS];
    for (int i = 0; i < nSig; i++) {
        double r = muxDefs[i * CANTP_MUXDEF_COLS], v = muxDefs[i * CANTP_MUXDEF_COLS + 1];
        if (r < -1 || r >= nSig || r != (double)(int)r) return CANTP_ERR_MUXDEF;
        rows[i] = (int)r;
        if (rows[i] == i) return CANTP_ERR_MUXDEF;
        if (rows[i] >= 0 && (v < 0 || v > 1.8446744073709552e19 || v != (double)(uint64_t)v)) return CANTP_ERR_MUXDEF;
        vals[i] = rows[i] >= 0 ? (uint64_t)v : 0;
        if (rows[i] >= 0 && m->sig[rows[i]].type > 1) return CANTP_ERR_MUXDEF;   /* multiplexor must be an integer */
    }
    for (int i = 0; i < nSig; i++) {                     /* reject cycles */
        int r = rows[i], hops = 0;
        while (r >= 0) { if (++hops > nSig) return CANTP_ERR_MUXDEF; r = rows[r]; }
    }
    for (int i = 0; i < nSig; i++) { m->sig[i].muxRow = rows[i]; m->sig[i].muxValue = vals[i]; }
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
    return packets_for(m) > 0 ? 1 + packets_for(m) : 1;
}
static int output_size(const CanTpMsg* m)
{
    switch (m->transport) {
    case CANTP_TP_CLASSIC:   return xnet_record_size(m->len);
    case CANTP_TP_CANFD:
    case CANTP_TP_CANFD_BRS: return xnet_record_size(canfd_pad_len(m->len));
    default:                 return frame_count(m) * CANTP_RECORD_MIN;
    }
}
CANTP_API int32_t CanTp_FrameCount(int32_t slot) { SLOT_OR_RETURN(m, slot); return frame_count(m); }
CANTP_API int32_t CanTp_OutputSize(int32_t slot) { SLOT_OR_RETURN(m, slot); return output_size(m); }

/* ------------------------------------------------------------------------ */
/* Multiplexing                                                              */
/* ------------------------------------------------------------------------ */
/* Is signal i present given every signal's raw value? (multiplexors may themselves be multiplexed) */
static int mux_selected(const CanTpMsg* m, int i, const uint64_t* raws)
{
    int hops = 0;
    while (m->sig[i].muxRow >= 0) {
        int r = m->sig[i].muxRow;
        if (raws[r] != m->sig[i].muxValue) return 0;
        i = r;
        if (++hops > m->nSig) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Pack                                                                      */
/* ------------------------------------------------------------------------ */
int32_t pack_values(const CanTpMsg* m, const double* values, int32_t nValues, uint8_t* payload)
{
    if (nValues < m->nSig) return CANTP_ERR_ARG;
    uint64_t raws[CANTP_MAX_SIGNALS];
    for (int i = 0; i < m->nSig; i++) {
        int rc = bits_encode(&m->sig[i], values[i], &raws[i]);
        if (rc != CANTP_OK) return rc;
    }
    memset(payload, m->pad, (size_t)m->len);   /* unused bits = pad; bits_place overwrites signal bits */
    for (int i = 0; i < m->nSig; i++) {
        if (!mux_selected(m, i, raws)) continue;
        bits_place(payload, m->sig[i].start, m->sig[i].len, m->sig[i].motorola, raws[i]);
    }
    return CANTP_OK;
}

int32_t emit_frames(const CanTpMsg* m, const uint8_t* payload, uint64_t ts, uint64_t spacing,
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
    if (m->transport == CANTP_TP_ISOTP) {                /* single frame only; longer ones go through the session */
        if (m->len > 7) return CANTP_ERR_TRANSPORT;
        uint8_t buf[8]; memset(buf, m->pad, 8);
        buf[0] = (uint8_t)(ISOTP_PCI_SF << 4 | m->len);
        memcpy(buf + 1, payload, (size_t)m->len);
        xnet_write_record(out, outLen, ts, m->id, m->ext, XNET_TYPE_CAN_DATA, buf, 8);
        *bytesWritten = CANTP_RECORD_MIN;
        return CANTP_OK;
    }

    /* J1939 */
    uint32_t pgn = pgn_of_id(m->id);
    uint8_t* p = out;
    if (m->len <= 8) {                                   /* single frame under the PGN, DA applied for PDU1 */
        uint8_t buf[8]; memset(buf, m->pad, 8); memcpy(buf, payload, (size_t)m->len);
        uint32_t id = j1939_id(pgn, m->sa, m->da, (uint8_t)((m->id >> 26) & 7u));
        xnet_write_record(p, outLen, ts, id, 1, XNET_TYPE_CAN_DATA, buf, m->len);
        *bytesWritten = CANTP_RECORD_MIN;
        return CANTP_OK;
    }
    if (m->transport != CANTP_TP_J1939_BAM) return CANTP_ERR_TRANSPORT;   /* RTS/CTS goes through the session */
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
    if (packets_for(m) > 0 && (m->transport == CANTP_TP_J1939_RTS || m->transport == CANTP_TP_ISOTP))
        return CANTP_ERR_TRANSPORT;                      /* handshake needed: CanTp_TxStart */
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
void decode_values(const CanTpMsg* m, const uint8_t* payload, double* values, int32_t nValues)
{
    uint64_t raws[CANTP_MAX_SIGNALS];
    for (int i = 0; i < m->nSig; i++)
        raws[i] = bits_extract(payload, m->sig[i].start, m->sig[i].len, m->sig[i].motorola);
    for (int i = 0; i < m->nSig && i < nValues; i++)
        values[i] = mux_selected(m, i, raws) ? bits_decode(&m->sig[i], raws[i]) : quiet_nan();
}

/* Does this record's id carry the slot's message (single-frame case)? */
int id_matches(const CanTpMsg* m, uint32_t rawId)
{
    int ext = (rawId & CANTP_XNET_EXTENDED_ID_FLAG) != 0;
    uint32_t id = rawId & 0x1FFFFFFFu;
    if (ext != m->ext) return 0;
    if (!m->ext) return id == m->id;
    if (m->transport == CANTP_TP_J1939_BAM || m->transport == CANTP_TP_J1939_RTS || m->saAny) {
        /* J1939: compare PGN (priority-less), any SA when placeholder; PDU1: the PS byte must be our DA */
        if (pgn_of_id(id) != pgn_of_id(m->id)) return 0;
        if (m->transport == CANTP_TP_J1939_RTS && is_pdu1(pgn_of_id(id)) && ((id >> 8) & 0xFFu) != m->da) return 0;
        return m->saAny || (id & 0xFFu) == m->sa;
    }
    return id == m->id;
}

/* Feed one record into the slot's decoder (no responses). Returns CANTP_FOUND when complete. */
static int32_t feed_record(CanTpMsg* m, const uint8_t* rec, int recLen, uint32_t nowMs,
                           double* values, int32_t nValues, uint8_t* out, int32_t outLen, int32_t* written)
{
    int size = CanTp_RecordSize(rec, recLen);
    if (size < 0) return CANTP_ERR_RECORD;
    int type = rec[12];
    if (type != XNET_TYPE_CAN_DATA && type != XNET_TYPE_CANFD_DATA && type != XNET_TYPE_CANFDBRS) return CANTP_OK;

    int singleFrame = m->transport == CANTP_TP_CLASSIC || m->transport == CANTP_TP_CANFD || m->transport == CANTP_TP_CANFD_BRS
                   || ((m->transport == CANTP_TP_J1939_BAM || m->transport == CANTP_TP_J1939_RTS) && m->len <= 8);
    if (singleFrame) {
        uint32_t rawId = xnet_get_u32(rec + 8);
        int plen = rec[15];
        if (!id_matches(m, rawId)) return CANTP_OK;
        if (plen < m->len) return CANTP_OK;                  /* too short to hold the signals */
        decode_values(m, rec + 16, values, nValues);
        return CANTP_FOUND;
    }
    return session_rx_record(m, rec, size, nowMs, xnet_get_u64(rec), values, nValues, out, outLen, written);
}

int32_t cantp_feed_record(CanTpMsg* m, const uint8_t* rec, int recLen, double* values, int32_t nValues)
{
    int32_t w = 0;
    return feed_record(m, rec, recLen, m->rx.lastMs, values, nValues, NULL, 0, &w);
}

CANTP_API int32_t CanTp_RxFeed(int32_t slot, const uint8_t* frame, int32_t frameLen,
                               double* values, int32_t nValues)
{
    if (!frame || (!values && nValues > 0) || nValues < 0) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    int32_t w = 0;
    return feed_record(m, frame, frameLen, m->rx.lastMs, values, nValues, NULL, 0, &w);
}

CANTP_API int32_t CanTp_RxReset(int32_t slot)
{
    SLOT_OR_RETURN(m, slot);
    memset(&m->rx, 0, sizeof m->rx);
    return CANTP_OK;
}

CANTP_API int32_t CanTp_RxStep(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                               uint64_t timestamp100ns, double* values, int32_t nValues,
                               uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!values && nValues > 0) || nValues < 0 || (!out && outLen > 0)) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    if (!frame) return session_rx_timer(m, nowMs, timestamp100ns, out, outLen, bytesWritten);
    return feed_record(m, frame, frameLen, nowMs, values, nValues, out, outLen, bytesWritten);
}

CANTP_API int32_t CanTp_RxStepSgl(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                                  uint64_t timestamp100ns, float* values, int32_t nValues,
                                  uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (nValues < 0 || nValues > CANTP_MAX_SIGNALS) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    int32_t rc = CanTp_RxStep(slot, frame, frameLen, nowMs, timestamp100ns, tmp, nValues, out, outLen, bytesWritten);
    if (rc == CANTP_FOUND && values) for (int32_t i = 0; i < nValues; i++) values[i] = (float)tmp[i];
    return rc;
}

CANTP_API int32_t CanTp_RxState(int32_t slot) { SLOT_OR_RETURN(m, slot); return m->rx.kind != RX_NONE; }

CANTP_API int32_t CanTp_Unpack(int32_t slot, const uint8_t* frames, int32_t framesLen,
                               double* values, int32_t nValues, int32_t* bytesConsumed)
{
    if (bytesConsumed) *bytesConsumed = 0;
    if (!frames || framesLen < 0 || (!values && nValues > 0) || nValues < 0 || !bytesConsumed) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    CanTpRx saved = m->rx;                       /* stateless: do not disturb the live decoder */
    memset(&m->rx, 0, sizeof m->rx);
    int32_t off = 0, rc = CANTP_OK, w = 0;
    while (off + CANTP_RECORD_MIN <= framesLen) {
        int size = CanTp_RecordSize(frames + off, framesLen - off);
        if (size < 0) { rc = CANTP_ERR_RECORD; break; }
        rc = feed_record(m, frames + off, size, m->rx.lastMs, values, nValues, NULL, 0, &w);
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

/* ------------------------------------------------------------------------ */
/* Session entry points                                                      */
/* ------------------------------------------------------------------------ */
CANTP_API int32_t CanTp_SessionConfig(int32_t slot, const double* cfg, int32_t cfgLen)
{
    SLOT_OR_RETURN(m, slot);
    if (!cfg || cfgLen < CANTP_SESSION_COLS) return CANTP_ERR_ARG;
    CanTpSession s = m->ses;
    if (cfg[0] >= 0) {
        if (cfg[0] > 0x1FFFFFFF) return CANTP_ERR_ARG;
        s.peerId = (uint32_t)cfg[0];
        s.peerExt = cfg[1] < 0 ? m->ext : (cfg[1] != 0.0);
        if (!s.peerExt && s.peerId > 0x7FFu) return CANTP_ERR_ARG;
    }
    if (cfg[2] < 0 || cfg[2] > 255 || cfg[3] < 0 || cfg[3] > 127 || cfg[4] < 0 || cfg[5] < 0 || cfg[5] > 255) return CANTP_ERR_ARG;
    s.blockSize = (int)cfg[2];
    s.stMin = (int)cfg[3];
    s.timeoutMs = (uint32_t)cfg[4];
    s.maxPerCts = (int)cfg[5] == 0 ? 255 : (int)cfg[5];
    m->ses = s;
    return CANTP_OK;
}

CANTP_API int32_t CanTp_TxStart(int32_t slot, const double* values, int32_t nValues, uint32_t nowMs,
                                uint64_t timestamp100ns, uint64_t spacing100ns,
                                uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!values && nValues > 0) || nValues < 0 || (!out && outLen > 0)) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    if (m->tx.state == TX_WAIT) return CANTP_ERR_BUSY;
    int32_t rc = pack_values(m, values, nValues, m->tx.payload);
    if (rc != CANTP_OK) return rc;
    return session_tx_start(m, nowMs, timestamp100ns, spacing100ns, out, outLen, bytesWritten);
}

CANTP_API int32_t CanTp_TxStartSgl(int32_t slot, const float* values, int32_t nValues, uint32_t nowMs,
                                   uint64_t timestamp100ns, uint64_t spacing100ns,
                                   uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!values && nValues > 0) || nValues < 0 || nValues > CANTP_MAX_SIGNALS) return CANTP_ERR_ARG;
    double tmp[CANTP_MAX_SIGNALS];
    for (int32_t i = 0; i < nValues; i++) tmp[i] = values[i];
    return CanTp_TxStart(slot, tmp, nValues, nowMs, timestamp100ns, spacing100ns, out, outLen, bytesWritten);
}

CANTP_API int32_t CanTp_TxFeed(int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                               uint64_t timestamp100ns, uint64_t spacing100ns,
                               uint8_t* out, int32_t outLen, int32_t* bytesWritten)
{
    if (bytesWritten) *bytesWritten = 0;
    if (!bytesWritten || (!out && outLen > 0)) return CANTP_ERR_ARG;
    SLOT_OR_RETURN(m, slot);
    if (frame && CanTp_RecordSize(frame, frameLen) < 0) return CANTP_ERR_RECORD;
    return session_tx_feed(m, frame, frameLen, nowMs, timestamp100ns, spacing100ns, out, outLen, bytesWritten);
}

CANTP_API int32_t CanTp_TxState(int32_t slot) { SLOT_OR_RETURN(m, slot); return m->tx.state; }
CANTP_API int32_t CanTp_TxReset(int32_t slot) { SLOT_OR_RETURN(m, slot); memset(&m->tx, 0, sizeof m->tx); return CANTP_OK; }
