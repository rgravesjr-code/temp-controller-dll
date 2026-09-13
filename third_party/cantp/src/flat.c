/* flat.c - CanTp_DefineFlat: message definition from a flattened LabVIEW
 * "J1939Msg(V4).ctl" cluster (the record an Eaton .ecd database stores per
 * message), plus CanTp_GetDef / CanTp_Defaults. See cantp.h and tools/ecdflat.py. */
#include "cantp_internal.h"

/* ---- big-endian cursor over the flattened bytes ------------------------ */
typedef struct { const uint8_t* p; int32_t len, o; int bad; } FlatRd;

static int rd_need(FlatRd* r, int32_t n)
{
    if (r->bad || n < 0 || r->o > r->len - n) { r->bad = 1; return 0; }
    return 1;
}
static uint32_t rd_u32(FlatRd* r)
{
    if (!rd_need(r, 4)) return 0;
    const uint8_t* p = r->p + r->o; r->o += 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int32_t  rd_i32(FlatRd* r) { return (int32_t)rd_u32(r); }
static uint16_t rd_u16(FlatRd* r)
{
    if (!rd_need(r, 2)) return 0;
    const uint8_t* p = r->p + r->o; r->o += 2;
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint8_t rd_u8(FlatRd* r)
{
    if (!rd_need(r, 1)) return 0;
    return r->p[r->o++];
}
static double rd_f64(FlatRd* r)
{
    if (!rd_need(r, 8)) return 0.0;
    uint64_t b = 0;
    for (int i = 0; i < 8; i++) b = (b << 8) | r->p[r->o + i];
    r->o += 8;
    double d; memcpy(&d, &b, 8);
    return d;
}
static void rd_skip_str(FlatRd* r)                    /* I32 length + bytes, contents unused */
{
    int32_t n = rd_i32(r);
    if (n < 0) { r->bad = 1; return; }
    if (rd_need(r, n)) r->o += n;
}

/* ---- cluster -> tables ------------------------------------------------- */
/* J1939Msg(V4) enum values */
#define ECD_TYPE_SIGNED    0
#define ECD_TYPE_UNSIGNED  1
#define ECD_TYPE_FLOAT     2
#define ECD_ORDER_INTEL    0
#define ECD_ORDER_MOTOROLA 1

/*
 * Parses one flattened J1939Msg(V4) cluster into a CanTp message row, signal
 * rows and per-channel defaults. Returns the channel count, CANTP_ERR_FLAT
 * when the bytes do not parse, CANTP_ERR_TOO_MANY above CANTP_MAX_SIGNALS.
 * When `transport` is -1 it is derived: 11-bit ids are one classic frame
 * (CAN FD above 8 bytes); 29-bit ids are J1939: a PDU1 PGN addressed to one
 * node (PS not 0xFF/0xFE) is one classic frame with the id verbatim up to 8
 * bytes and RTS/CTS above; everything else is transport 1 (single frame
 * under the PGN up to 8 bytes, BAM above).
 */
static int32_t flat_parse(const uint8_t* flat, int32_t flatLen, int32_t transport, int32_t sa,
                          double* msgDef, double* sigDefs, double* defaults, int32_t* consumed)
{
    FlatRd r = { flat, flatLen, 0, 0 };
    rd_skip_str(&r);                                   /* message name */
    uint32_t msgId = rd_u32(&r);
    (void)rd_u32(&r);                                  /* PGN, implied by the id */
    int ext = rd_u8(&r) != 0;
    int32_t numBytes = rd_i32(&r);
    rd_skip_str(&r);                                   /* description */
    double updateRate = rd_f64(&r);
    (void)rd_f64(&r);                                  /* tolerance */
    int32_t nCh = rd_i32(&r);
    if (r.bad) return CANTP_ERR_FLAT;
    if (nCh < 0 || nCh > 100000) return CANTP_ERR_FLAT;
    if (nCh > CANTP_MAX_SIGNALS) return CANTP_ERR_TOO_MANY;

    for (int32_t i = 0; i < nCh; i++) {
        rd_skip_str(&r);                               /* channel name */
        int32_t start = rd_i32(&r), nBits = rd_i32(&r);
        uint16_t dtype = rd_u16(&r), order = rd_u16(&r);
        double sf = rd_f64(&r), off = rd_f64(&r), mn = rd_f64(&r), mx = rd_f64(&r), dflt = rd_f64(&r);
        rd_skip_str(&r);                               /* unit */
        (void)rd_u16(&r);                              /* lookup table kind */
        int32_t nLut = rd_i32(&r);
        if (r.bad || nLut < 0) return CANTP_ERR_FLAT;
        for (int32_t k = 0; k < nLut && !r.bad; k++) { (void)rd_f64(&r); rd_skip_str(&r); rd_skip_str(&r); rd_skip_str(&r); }
        rd_skip_str(&r);                               /* channel description */
        if (r.bad) return CANTP_ERR_FLAT;
        double vtype;
        switch (dtype) {
        case ECD_TYPE_SIGNED:   vtype = 1; break;
        case ECD_TYPE_UNSIGNED: vtype = 0; break;
        case ECD_TYPE_FLOAT:    vtype = nBits == 64 ? 3 : 2; break;
        default: return CANTP_ERR_SIGDEF;
        }
        if (order != ECD_ORDER_INTEL && order != ECD_ORDER_MOTOROLA) return CANTP_ERR_SIGDEF;
        double* s = sigDefs + i * CANTP_SIGDEF_COLS;
        s[CANTP_SIG_START] = start; s[CANTP_SIG_LENGTH] = nBits; s[CANTP_SIG_ORDER] = order == ECD_ORDER_MOTOROLA;
        s[CANTP_SIG_TYPE] = vtype;  s[CANTP_SIG_FACTOR] = sf;    s[CANTP_SIG_OFFSET] = off;
        s[CANTP_SIG_MIN] = mn;      s[CANTP_SIG_MAX] = mx;
        defaults[i] = dflt;
    }
    (void)rd_u8(&r);                                   /* EatonIPY flag */
    if (r.bad) return CANTP_ERR_FLAT;
    *consumed = r.o;

    uint32_t id = msgId & 0x1FFFFFFFu;
    if (!ext && msgId >= 0x80000000u) ext = 1;         /* DBC-style extended marker */
    if (!ext && id > 0x7FFu) ext = 1;                  /* cannot be an 11-bit id */
    if (numBytes < 0) return CANTP_ERR_MSGDEF;

    uint32_t pgn = pgn_of_id(id);
    int pdu1 = ext && is_pdu1(pgn);
    uint8_t ps = (uint8_t)((id >> 8) & 0xFFu);
    int realDa = pdu1 && ps != 0xFFu && ps != J1939_SA_ANY;   /* PDU1 addressed to one node */
    if (transport < 0) {
        if (!ext)         transport = numBytes <= 8 ? CANTP_TP_CLASSIC : CANTP_TP_CANFD;
        else if (realDa)  transport = numBytes <= 8 ? CANTP_TP_CLASSIC : CANTP_TP_J1939_RTS;   /* id verbatim / handshake */
        else              transport = CANTP_TP_J1939_BAM;                                      /* single frame under the PGN, or BAM */
    }
    int da = (pdu1 && transport != CANTP_TP_J1939_BAM) ? ps : J1939_GLOBAL_DA;
    int pad = transport == CANTP_TP_ISOTP ? 0xCC : (ext ? 0xFF : 0x00);

    msgDef[CANTP_MSG_ID] = id;  msgDef[CANTP_MSG_EXTENDED] = ext;  msgDef[CANTP_MSG_LENGTH] = numBytes;
    msgDef[CANTP_MSG_TRANSPORT] = transport;  msgDef[CANTP_MSG_SA] = ext ? sa : -1;  msgDef[CANTP_MSG_DA] = da;
    msgDef[CANTP_MSG_PAD] = pad;  msgDef[CANTP_MSG_CYCLE_MS] = updateRate > 0 ? updateRate : 0;
    return nCh;
}

CANTP_API int32_t CanTp_DefineFlat(int32_t slot, const uint8_t* flat, int32_t flatLen,
                                   int32_t transport, int32_t sa)
{
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return CANTP_ERR_SLOT;
    if (!flat || flatLen < 4 + 4 + 4 + 1 + 4 + 4 + 8 + 8 + 4 + 1) return CANTP_ERR_ARG;
    if (transport < -1 || transport > CANTP_TP_ISOTP || sa < -1 || sa > 253) return CANTP_ERR_ARG;
    double msgDef[CANTP_MSGDEF_COLS];
    double sigDefs[CANTP_MAX_SIGNALS * CANTP_SIGDEF_COLS];          /* 8 KB on the stack, per call (re-entrant) */
    double defaults[CANTP_MAX_SIGNALS];
    int32_t consumed = 0;
    int32_t n = flat_parse(flat, flatLen, transport, sa, msgDef, sigDefs, defaults, &consumed);
    if (n < 0) return n;
    int32_t rc = CanTp_Define(slot, msgDef, CANTP_MSGDEF_COLS, sigDefs, n);
    if (rc != CANTP_OK) return rc;
    CanTpMsg* m = cantp_slot(slot);
    for (int32_t i = 0; i < n; i++) m->sig[i].dflt = defaults[i];
    m->flatBytes = consumed;
    return CANTP_OK;
}

CANTP_API int32_t CanTp_FlatSize(const uint8_t* flat, int32_t flatLen)
{
    if (!flat || flatLen < 0) return CANTP_ERR_ARG;
    double msgDef[CANTP_MSGDEF_COLS];
    double sigDefs[CANTP_MAX_SIGNALS * CANTP_SIGDEF_COLS];          /* 8 KB on the stack, per call (re-entrant) */
    double defaults[CANTP_MAX_SIGNALS];
    int32_t consumed = 0;
    int32_t n = flat_parse(flat, flatLen, -1, -1, msgDef, sigDefs, defaults, &consumed);
    return n < 0 ? n : consumed;
}

CANTP_API int32_t CanTp_GetDef(int32_t slot, double* msgDef, int32_t msgDefLen,
                               double* sigDefs, int32_t nSigMax)
{
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return CANTP_ERR_SLOT;
    const CanTpMsg* m = cantp_slot(slot);
    if (!m->used) return CANTP_ERR_NOT_DEFINED;
    if (msgDef) {
        if (msgDefLen < CANTP_MSGDEF_COLS) return CANTP_ERR_ARG;
        msgDef[CANTP_MSG_ID] = m->id;  msgDef[CANTP_MSG_EXTENDED] = m->ext;  msgDef[CANTP_MSG_LENGTH] = m->len;
        msgDef[CANTP_MSG_TRANSPORT] = m->transport;
        msgDef[CANTP_MSG_SA] = (m->ext && !m->saAny) ? m->sa : -1;
        msgDef[CANTP_MSG_DA] = m->da;  msgDef[CANTP_MSG_PAD] = m->pad;  msgDef[CANTP_MSG_CYCLE_MS] = m->cycleMs;
    }
    if (sigDefs) {
        if (nSigMax < 0) return CANTP_ERR_ARG;
        for (int i = 0; i < m->nSig && i < nSigMax; i++) {
            double* s = sigDefs + i * CANTP_SIGDEF_COLS;
            const CanTpSig* g = &m->sig[i];
            s[CANTP_SIG_START] = g->start;  s[CANTP_SIG_LENGTH] = g->len;  s[CANTP_SIG_ORDER] = g->motorola;
            s[CANTP_SIG_TYPE] = g->type;    s[CANTP_SIG_FACTOR] = g->factor;  s[CANTP_SIG_OFFSET] = g->offset;
            s[CANTP_SIG_MIN] = g->min;      s[CANTP_SIG_MAX] = g->max;
        }
    }
    return m->nSig;
}

CANTP_API int32_t CanTp_Defaults(int32_t slot, double* values, int32_t nValues)
{
    if (slot < 0 || slot >= CANTP_MAX_SLOTS) return CANTP_ERR_SLOT;
    const CanTpMsg* m = cantp_slot(slot);
    if (!m->used) return CANTP_ERR_NOT_DEFINED;
    if (!values || nValues < 0) return CANTP_ERR_ARG;
    for (int i = 0; i < m->nSig && i < nValues; i++) values[i] = m->sig[i].dflt;
    return m->nSig;
}
