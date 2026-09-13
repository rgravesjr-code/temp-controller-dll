/* session.c - multi-frame reception (BAM, RTS/CTS, ISO-TP) and the transmit sessions (RTS/CTS, ISO-TP).
 * Frames the peer must receive are appended to `out`; with out == NULL (CanTp_Unpack, CanTp_RxFeed)
 * reassembly still works, the responses are simply not produced. See cantp.h. */
#include "cantp_internal.h"

#define J1939_ABORT_TIMEOUT   3u   /* J1939-21 connection abort reasons */
#define J1939_ABORT_RESOURCES 2u
#define J1939_ABORT_RETRANS   5u

static uint32_t elapsed(uint32_t now, uint32_t then) { return (uint32_t)(now - then); }

void session_defaults(CanTpMsg* m)
{
    memset(&m->ses, 0, sizeof m->ses);
    m->ses.peerExt = m->ext;
    if (m->ext) m->ses.peerId = (m->id & 0x1FFF0000u) | ((m->id & 0xFFu) << 8) | ((m->id >> 8) & 0xFFu);
    else        m->ses.peerId = m->id + 8 <= 0x7FFu ? m->id + 8 : m->id - 8;
    m->ses.maxPerCts = 255;
}
static uint32_t timeout_of(const CanTpMsg* m, uint32_t def) { return m->ses.timeoutMs ? m->ses.timeoutMs : def; }

/* Append one classic 8-byte record to out (no-op without an out buffer). */
static int32_t put_record(uint8_t* out, int32_t outLen, int32_t* written, uint64_t ts, uint32_t id, int ext, const uint8_t* d)
{
    if (!out) return CANTP_OK;
    if (*written + CANTP_RECORD_MIN > outLen) return CANTP_ERR_BUFFER;
    xnet_write_record(out + *written, CANTP_RECORD_MIN, ts, id, ext, XNET_TYPE_CAN_DATA, d, 8);
    *written += CANTP_RECORD_MIN;
    return CANTP_OK;
}

/* ---- J1939 TP.CM builders ------------------------------------------------ */
static void cm_fill(uint8_t* d, uint8_t ctrl, uint32_t pgn)
{
    memset(d, 0xFF, 8);
    d[0] = ctrl;
    d[5] = (uint8_t)pgn; d[6] = (uint8_t)(pgn >> 8); d[7] = (uint8_t)(pgn >> 16);
}
static int32_t put_cts(const CanTpMsg* m, uint8_t from, uint8_t to, int num, int next, uint64_t ts,
                       uint8_t* out, int32_t outLen, int32_t* written)
{
    uint8_t d[8]; cm_fill(d, J1939_CTS_CTRL, pgn_of_id(m->id));
    d[1] = (uint8_t)num; d[2] = (uint8_t)next;
    return put_record(out, outLen, written, ts, j1939_id(J1939_TP_CM_PGN, from, to, J1939_TP_PRIORITY), 1, d);
}
static int32_t put_abort(const CanTpMsg* m, uint8_t from, uint8_t to, uint8_t reason, uint64_t ts,
                         uint8_t* out, int32_t outLen, int32_t* written)
{
    uint8_t d[8]; cm_fill(d, J1939_ABORT_CTRL, pgn_of_id(m->id));
    d[1] = reason;
    return put_record(out, outLen, written, ts, j1939_id(J1939_TP_CM_PGN, from, to, J1939_TP_PRIORITY), 1, d);
}
static int32_t put_dt(const CanTpMsg* m, const uint8_t* payload, int total, int seq, uint64_t ts,
                      uint8_t* out, int32_t outLen, int32_t* written)
{
    uint8_t d[8]; memset(d, m->pad, 8);
    d[0] = (uint8_t)seq;
    int off = (seq - 1) * 7, n = total - off; if (n > 7) n = 7;
    memcpy(d + 1, payload + off, (size_t)n);
    return put_record(out, outLen, written, ts, j1939_id(J1939_TP_DT_PGN, m->sa, m->da, J1939_TP_PRIORITY), 1, d);
}

/* ---- ISO-TP builders ------------------------------------------------------ */
static int32_t put_fc(const CanTpMsg* m, int status, uint64_t ts, uint8_t* out, int32_t outLen, int32_t* written)
{
    uint8_t d[8]; memset(d, m->pad, 8);
    d[0] = (uint8_t)(ISOTP_PCI_FC << 4 | status);
    d[1] = (uint8_t)m->ses.blockSize;
    d[2] = (uint8_t)m->ses.stMin;
    return put_record(out, outLen, written, ts, m->ses.peerId, m->ses.peerExt, d);
}
static int32_t put_cf(const CanTpMsg* m, const uint8_t* payload, int total, int packet, int seq, uint64_t ts,
                      uint8_t* out, int32_t outLen, int32_t* written)
{
    uint8_t d[8]; memset(d, m->pad, 8);
    d[0] = (uint8_t)(ISOTP_PCI_CF << 4 | (seq & 0xF));
    int off = 6 + (packet - 1) * 7, n = total - off; if (n > 7) n = 7;
    memcpy(d + 1, payload + off, (size_t)n);
    return put_record(out, outLen, written, ts, m->id, m->ext, d);
}

/* ---- decode helper ---------------------------------------------------------- */
static int32_t complete(const CanTpMsg* m, const uint8_t* buf, int got, double* values, int32_t nValues)
{
    uint8_t payload[CANTP_MAX_PAYLOAD];
    memset(payload, m->pad, sizeof payload);
    if (got > CANTP_MAX_PAYLOAD) got = CANTP_MAX_PAYLOAD;
    memcpy(payload, buf, (size_t)got);
    decode_values(m, payload, values, nValues);
    return CANTP_FOUND;
}

/* ======================================================================== */
/* Receive                                                                   */
/* ======================================================================== */
static int rx_window(const CanTpMsg* m)
{
    int w = m->ses.blockSize > 0 ? m->ses.blockSize : 255;
    if (m->rx.rtsMaxPackets && m->rx.rtsMaxPackets < w) w = m->rx.rtsMaxPackets;
    if (m->rx.packets - m->rx.nextSeq + 1 < w) w = m->rx.packets - m->rx.nextSeq + 1;
    return w;
}

static int32_t rx_j1939(CanTpMsg* m, uint32_t id, const uint8_t* d, int plen, uint32_t nowMs, uint64_t ts,
                        double* values, int32_t nValues, uint8_t* out, int32_t outLen, int32_t* written)
{
    uint32_t pgn = pgn_of_id(id);
    uint8_t sa = (uint8_t)(id & 0xFFu);
    uint8_t ps = (uint8_t)((id >> 8) & 0xFFu);
    if (!m->saAny && sa != m->sa) return CANTP_OK;                    /* not our sender */
    if (plen < 8) return CANTP_OK;

    if (pgn == J1939_TP_CM_PGN) {
        uint32_t msgPgn = (uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16);
        if (msgPgn != pgn_of_id(m->id)) return CANTP_OK;
        int total = d[1] | (d[2] << 8), packets = d[3];
        switch (d[0]) {
        case J1939_BAM_CTRL:
            if (ps != J1939_GLOBAL_DA) return CANTP_OK;
            if (total < 9 || total > CANTP_MAX_PAYLOAD || packets != (total + 6) / 7) { m->rx.kind = RX_NONE; return CANTP_OK; }
            memset(&m->rx, 0, sizeof m->rx);
            m->rx.kind = RX_BAM; m->rx.total = total; m->rx.packets = packets; m->rx.nextSeq = 1; m->rx.sa = sa; m->rx.lastMs = nowMs;
            return CANTP_OK;
        case J1939_RTS_CTRL: {
            if (m->transport != CANTP_TP_J1939_RTS || ps != m->da) return CANTP_OK;
            if (total < 9 || total > CANTP_MAX_PAYLOAD || packets != (total + 6) / 7) {
                m->rx.kind = RX_NONE;
                return put_abort(m, m->da, sa, J1939_ABORT_RESOURCES, ts, out, outLen, written) < 0 ? CANTP_ERR_BUFFER : CANTP_OK;
            }
            memset(&m->rx, 0, sizeof m->rx);
            m->rx.kind = RX_RTS; m->rx.total = total; m->rx.packets = packets; m->rx.nextSeq = 1; m->rx.sa = sa;
            m->rx.rtsMaxPackets = d[4]; m->rx.lastMs = nowMs;
            m->rx.windowLeft = rx_window(m);
            m->rx.windowEnd = m->rx.windowLeft;
            return put_cts(m, m->da, sa, m->rx.windowLeft, 1, ts, out, outLen, written);
        }
        case J1939_ABORT_CTRL:
            if (m->rx.kind != RX_NONE && sa == m->rx.sa) { m->rx.kind = RX_NONE; m->rx.total = 0; return CANTP_ERR_ABORTED; }
            return CANTP_OK;
        default:                                                      /* CTS / EndOfMsgAck belong to a sender */
            return CANTP_OK;
        }
    }
    if (pgn == J1939_TP_DT_PGN && (m->rx.kind == RX_BAM || m->rx.kind == RX_RTS) && sa == m->rx.sa) {
        if (m->rx.kind == RX_RTS && ps != m->da) return CANTP_OK;
        int seq = d[0];
        m->rx.lastMs = nowMs;
        if (m->rx.kind == RX_BAM) {                                       /* BAM: strictly sequential, no recovery */
            if (seq != m->rx.nextSeq) { m->rx.kind = RX_NONE; m->rx.total = 0; return CANTP_OK; }
            int off = (seq - 1) * 7, n = m->rx.total - off; if (n > 7) n = 7;
            memcpy(m->rx.buf + off, d + 1, (size_t)n);
            if (seq == m->rx.packets) {
                m->rx.kind = RX_NONE; m->rx.total = 0;
                return complete(m, m->rx.buf, off + n, values, nValues);
            }
            m->rx.nextSeq++;
            return CANTP_OK;
        }
        /* RTS/CTS: store whatever arrives, judge the window when its last packet (or the final one) is in */
        if (seq < 1 || seq > m->rx.packets) return CANTP_OK;
        {
            int off = (seq - 1) * 7, n = m->rx.total - off; if (n > 7) n = 7;
            memcpy(m->rx.buf + off, d + 1, (size_t)n);
            m->rx.seen[seq] = 1;
        }
        if (seq < m->rx.windowEnd && seq != m->rx.packets) return CANTP_OK;
        int missing = 0, last = m->rx.windowEnd < m->rx.packets ? m->rx.windowEnd : m->rx.packets;
        for (int k = 1; k <= last; k++) if (!m->rx.seen[k]) { missing = k; break; }
        if (missing) {                                                    /* J1939-21: re-request from the first gap */
            if (m->rx.retries++ >= 2) {
                m->rx.kind = RX_NONE; m->rx.total = 0;
                int32_t rc = put_abort(m, m->da, sa, J1939_ABORT_RETRANS, ts, out, outLen, written);
                return rc < 0 ? rc : CANTP_ERR_ABORTED;
            }
            m->rx.nextSeq = missing;
            m->rx.windowLeft = rx_window(m);
            m->rx.windowEnd = missing + m->rx.windowLeft - 1;
            return put_cts(m, m->da, sa, m->rx.windowLeft, missing, ts, out, outLen, written);
        }
        if (last >= m->rx.packets) {                                      /* everything received */
            int got = m->rx.total;
            m->rx.kind = RX_NONE; m->rx.total = 0;
            uint8_t e[8]; cm_fill(e, J1939_EOMA_CTRL, pgn_of_id(m->id));
            e[1] = (uint8_t)got; e[2] = (uint8_t)(got >> 8); e[3] = (uint8_t)m->rx.packets;
            int32_t rc = put_record(out, outLen, written, ts, j1939_id(J1939_TP_CM_PGN, m->da, sa, J1939_TP_PRIORITY), 1, e);
            if (rc < 0) return rc;
            return complete(m, m->rx.buf, got, values, nValues);
        }
        m->rx.nextSeq = last + 1;                                         /* next window */
        m->rx.windowLeft = rx_window(m);
        m->rx.windowEnd = m->rx.nextSeq + m->rx.windowLeft - 1;
        return put_cts(m, m->da, sa, m->rx.windowLeft, m->rx.nextSeq, ts, out, outLen, written);
    }
    return CANTP_OK;
}

static int32_t rx_isotp(CanTpMsg* m, uint32_t rawId, const uint8_t* d, int plen, uint32_t nowMs, uint64_t ts,
                        double* values, int32_t nValues, uint8_t* out, int32_t outLen, int32_t* written)
{
    if (!id_matches(m, rawId) || plen < 1) return CANTP_OK;
    int pci = d[0] >> 4;
    switch (pci) {
    case ISOTP_PCI_SF: {
        int len = d[0] & 0xF;
        if (len < 1 || len > 7 || len > plen - 1) return CANTP_OK;
        return complete(m, d + 1, len, values, nValues);
    }
    case ISOTP_PCI_FF: {
        if (plen < 8) return CANTP_OK;
        int total = ((d[0] & 0xF) << 8) | d[1];
        if (total < 8 || total > CANTP_MAX_PAYLOAD) return CANTP_OK;  /* 0 = 32-bit escape: > 4095, not supported */
        memset(&m->rx, 0, sizeof m->rx);
        m->rx.kind = RX_ISOTP; m->rx.total = total; m->rx.packets = (total - 6 + 6) / 7;
        memcpy(m->rx.buf, d + 2, 6); m->rx.received = 6; m->rx.cfSeq = 1; m->rx.lastMs = nowMs;
        m->rx.windowLeft = m->ses.blockSize;
        return put_fc(m, 0, ts, out, outLen, written);
    }
    case ISOTP_PCI_CF: {
        if (m->rx.kind != RX_ISOTP) return CANTP_OK;
        if ((d[0] & 0xF) != m->rx.cfSeq) { m->rx.kind = RX_NONE; m->rx.total = 0; return CANTP_ERR_ABORTED; }
        int n = m->rx.total - m->rx.received; if (n > 7) n = 7;
        if (n > plen - 1) n = plen - 1;
        memcpy(m->rx.buf + m->rx.received, d + 1, (size_t)n);
        m->rx.received += n;
        m->rx.cfSeq = (m->rx.cfSeq + 1) & 0xF;
        m->rx.lastMs = nowMs;
        if (m->rx.received >= m->rx.total) {
            int got = m->rx.received;
            m->rx.kind = RX_NONE; m->rx.total = 0;
            return complete(m, m->rx.buf, got, values, nValues);
        }
        if (m->ses.blockSize > 0 && --m->rx.windowLeft <= 0) {
            m->rx.windowLeft = m->ses.blockSize;
            return put_fc(m, 0, ts, out, outLen, written);
        }
        return CANTP_OK;
    }
    default:                                                          /* flow control belongs to a sender */
        return CANTP_OK;
    }
}

int32_t session_rx_record(CanTpMsg* m, const uint8_t* rec, int recLen, uint32_t nowMs, uint64_t ts,
                          double* values, int32_t nValues, uint8_t* out, int32_t outLen, int32_t* written)
{
    (void)recLen;
    uint32_t rawId = xnet_get_u32(rec + 8);
    int plen = rec[15];
    const uint8_t* d = rec + 16;
    if (m->transport == CANTP_TP_ISOTP) return rx_isotp(m, rawId, d, plen, nowMs, ts, values, nValues, out, outLen, written);
    if (!(rawId & CANTP_XNET_EXTENDED_ID_FLAG)) return CANTP_OK;
    return rx_j1939(m, rawId & 0x1FFFFFFFu, d, plen, nowMs, ts, values, nValues, out, outLen, written);
}

int32_t session_rx_timer(CanTpMsg* m, uint32_t nowMs, uint64_t ts, uint8_t* out, int32_t outLen, int32_t* written)
{
    if (m->rx.kind == RX_NONE) return CANTP_OK;
    uint32_t limit = m->rx.kind == RX_ISOTP ? timeout_of(m, ISOTP_N_MS) : timeout_of(m, J1939_T1_MS);
    if (elapsed(nowMs, m->rx.lastMs) <= limit) return CANTP_OK;
    int kind = m->rx.kind; uint8_t sa = m->rx.sa;
    m->rx.kind = RX_NONE; m->rx.total = 0;
    if (kind == RX_RTS) {
        int32_t rc = put_abort(m, m->da, sa, J1939_ABORT_TIMEOUT, ts, out, outLen, written);
        if (rc < 0) return rc;
    }
    return CANTP_ERR_TIMEOUT;
}

/* ======================================================================== */
/* Transmit                                                                  */
/* ======================================================================== */
int32_t session_tx_start(CanTpMsg* m, uint32_t nowMs, uint64_t ts, uint64_t spacing,
                         uint8_t* out, int32_t outLen, int32_t* written)
{
    CanTpTx* t = &m->tx;
    t->state = TX_IDLE; t->packets = packets_for(m); t->nextSeq = 1; t->sent = 0; t->cfSeq = 1; t->lastMs = nowMs;
    if (t->packets == 0 || m->transport == CANTP_TP_J1939_BAM) {     /* no handshake: the whole sequence at once */
        int32_t rc = emit_frames(m, t->payload, ts, spacing, out, outLen, written);
        if (rc != CANTP_OK) return rc;
        t->state = TX_DONE; t->sent = t->packets;
        return CANTP_DONE;
    }
    if (!out || outLen < CANTP_RECORD_MIN) { *written = CANTP_RECORD_MIN; return CANTP_ERR_BUFFER; }
    uint8_t d[8];
    if (m->transport == CANTP_TP_J1939_RTS) {
        cm_fill(d, J1939_RTS_CTRL, pgn_of_id(m->id));
        d[1] = (uint8_t)m->len; d[2] = (uint8_t)(m->len >> 8); d[3] = (uint8_t)t->packets; d[4] = (uint8_t)m->ses.maxPerCts;
        put_record(out, outLen, written, ts, j1939_id(J1939_TP_CM_PGN, m->sa, m->da, J1939_TP_PRIORITY), 1, d);
    } else {                                                          /* ISO-TP first frame */
        memset(d, m->pad, 8);
        d[0] = (uint8_t)(ISOTP_PCI_FF << 4 | (m->len >> 8)); d[1] = (uint8_t)m->len;
        memcpy(d + 2, t->payload, 6);
        put_record(out, outLen, written, ts, m->id, m->ext, d);
    }
    t->state = TX_WAIT;
    return CANTP_OK;
}

static int32_t tx_fail(CanTpMsg* m, int32_t code, uint8_t reason, uint64_t ts, uint8_t* out, int32_t outLen, int32_t* written)
{
    m->tx.state = code;
    if (m->transport == CANTP_TP_J1939_RTS) put_abort(m, m->sa, m->da, reason, ts, out, outLen, written);
    return code;
}

int32_t session_tx_feed(CanTpMsg* m, const uint8_t* rec, int recLen, uint32_t nowMs, uint64_t ts, uint64_t spacing,
                        uint8_t* out, int32_t outLen, int32_t* written)
{
    (void)recLen;
    CanTpTx* t = &m->tx;
    if (t->state != TX_WAIT) return t->state;                         /* idle 0, done 2, or the last error */

    if (rec && rec[12] == XNET_TYPE_CAN_DATA) {
        uint32_t rawId = xnet_get_u32(rec + 8);
        int ext = (rawId & CANTP_XNET_EXTENDED_ID_FLAG) != 0;
        uint32_t id = rawId & 0x1FFFFFFFu;
        int plen = rec[15];
        const uint8_t* d = rec + 16;
        if (m->transport == CANTP_TP_J1939_RTS) {
            if (ext && plen >= 8 && pgn_of_id(id) == J1939_TP_CM_PGN && ((id >> 8) & 0xFFu) == m->sa && (id & 0xFFu) == m->da) {
                uint32_t msgPgn = (uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16);
                if (msgPgn == pgn_of_id(m->id)) {
                    switch (d[0]) {
                    case J1939_CTS_CTRL: {
                        int num = d[1], next = d[2];
                        t->lastMs = nowMs;
                        if (num == 0) return CANTP_OK;                /* hold: receiver busy */
                        if (next < 1 || next > t->packets) return tx_fail(m, CANTP_ERR_ABORTED, J1939_ABORT_RESOURCES, ts, out, outLen, written);
                        int n = t->packets - next + 1; if (n > num) n = num;
                        if (n > m->ses.maxPerCts) n = m->ses.maxPerCts;
                        if (!out || outLen - *written < n * CANTP_RECORD_MIN) { *written = n * CANTP_RECORD_MIN; return CANTP_ERR_BUFFER; }
                        for (int i = 0; i < n; i++) {
                            put_dt(m, t->payload, m->len, next + i, ts, out, outLen, written);
                            ts += spacing;
                        }
                        t->nextSeq = next + n; t->sent += n;
                        return CANTP_OK;                              /* wait for the next CTS or the EndOfMsgAck */
                    }
                    case J1939_EOMA_CTRL:
                        t->state = TX_DONE;
                        return CANTP_DONE;
                    case J1939_ABORT_CTRL:
                        t->state = CANTP_ERR_ABORTED;
                        return CANTP_ERR_ABORTED;
                    default: break;
                    }
                }
            }
        } else if (m->transport == CANTP_TP_ISOTP) {
            if (ext == m->ses.peerExt && id == m->ses.peerId && plen >= 3 && (d[0] >> 4) == ISOTP_PCI_FC) {
                int fs = d[0] & 0xF, bs = d[1], st = d[2];
                t->lastMs = nowMs;
                if (fs == 1) return CANTP_OK;                         /* wait */
                if (fs != 0) return tx_fail(m, CANTP_ERR_ABORTED, 0, ts, out, outLen, written);
                uint64_t gap = spacing;
                if (st <= 0x7F && (uint64_t)st * 10000u > gap) gap = (uint64_t)st * 10000u;   /* STmin in ms */
                int remaining = t->packets - t->nextSeq + 1;
                int n = (bs == 0 || bs > remaining) ? remaining : bs;
                if (!out || outLen - *written < n * CANTP_RECORD_MIN) { *written = n * CANTP_RECORD_MIN; return CANTP_ERR_BUFFER; }
                for (int i = 0; i < n; i++) {
                    put_cf(m, t->payload, m->len, t->nextSeq + i, t->cfSeq, ts, out, outLen, written);
                    t->cfSeq = (t->cfSeq + 1) & 0xF;
                    ts += gap;
                }
                t->nextSeq += n; t->sent += n;
                if (t->nextSeq > t->packets) { t->state = TX_DONE; return CANTP_DONE; }
                return CANTP_OK;
            }
        }
    }
    /* timers */
    uint32_t limit = m->transport == CANTP_TP_ISOTP ? timeout_of(m, ISOTP_N_MS) : timeout_of(m, J1939_T3_MS);
    if (elapsed(nowMs, t->lastMs) > limit) return tx_fail(m, CANTP_ERR_TIMEOUT, J1939_ABORT_TIMEOUT, ts, out, outLen, written);
    return CANTP_OK;
}
