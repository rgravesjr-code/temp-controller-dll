/* test_release2.inc - release 2 tests: multiplexed signals, J1939 RTS/CTS and ISO-TP sessions.
   Included by test_main.c (same CHECK / msgdef / sigdef helpers). */

#define ISNAN(x) ((x) != (x))

/* ----------------------------------------------------------------------- */
static void test_mux(void)
{
    double m[8], s[6 * 8], mx[6 * 2];
    msgdef(m, 0x321, 0, 8, 0, -1, 255, 0);
    /* row: 0 Mode (multiplexor) 1 A m1  2 B m2  3 C plain  4 Sub m2 (itself a multiplexor)  5 D under Sub == 7 */
    sigdef(s + 0,  0, 8, 0, 0, 1, 0, 0, 0);
    sigdef(s + 8,  8, 16, 0, 0, 1, 0, 0, 0);
    sigdef(s + 16, 8, 16, 0, 0, 1, 0, 0, 0);
    sigdef(s + 24, 24, 8, 0, 0, 1, 0, 0, 0);
    sigdef(s + 32, 32, 4, 0, 0, 1, 0, 0, 0);
    sigdef(s + 40, 40, 8, 0, 0, 1, 0, 0, 0);
    double rows[6] = { -1, 0, 0, -1, 0, 4 }, vals[6] = { 0, 1, 2, 0, 2, 7 };
    for (int i = 0; i < 6; i++) { mx[2 * i] = rows[i]; mx[2 * i + 1] = vals[i]; }
    CHECK(CanTp_Define(10, m, 8, s, 6) == CANTP_OK);
    CHECK(CanTp_DefineMux(10, mx, 5) == CANTP_ERR_ARG);                  /* row count must match */
    CHECK(CanTp_DefineMux(11, mx, 6) == CANTP_ERR_NOT_DEFINED);
    CHECK(CanTp_DefineMux(10, mx, 6) == CANTP_OK);

    double v[6] = { 1, 1000, 2000, 5, 7, 9 }, u[6];
    uint8_t out[24]; int32_t n, c;
    CHECK(CanTp_Pack(10, v, 6, 0, 0, out, 24, &n) == CANTP_OK);
    const uint8_t* d = out + 16;
    CHECK(d[0] == 1 && d[1] == 0xE8 && d[2] == 0x03 && d[3] == 5 && d[4] == 0 && d[5] == 0);   /* B, Sub, D absent */
    CHECK(CanTp_Unpack(10, out, 24, u, 6, &c) == CANTP_FOUND);
    CHECK(u[0] == 1 && u[1] == 1000 && ISNAN(u[2]) && u[3] == 5 && ISNAN(u[4]) && ISNAN(u[5]));

    v[0] = 2;
    CHECK(CanTp_Pack(10, v, 6, 0, 0, out, 24, &n) == CANTP_OK);
    CHECK(d[1] == 0xD0 && d[2] == 0x07 && d[4] == 7 && d[5] == 9);          /* B, Sub, D present (Sub == 7) */
    CHECK(CanTp_Unpack(10, out, 24, u, 6, &c) == CANTP_FOUND);
    CHECK(u[0] == 2 && ISNAN(u[1]) && u[2] == 2000 && u[4] == 7 && u[5] == 9);

    v[4] = 3;                                                             /* Sub != 7: D absent, Sub present */
    CHECK(CanTp_Pack(10, v, 6, 0, 0, out, 24, &n) == CANTP_OK && d[4] == 3 && d[5] == 0);
    CHECK(CanTp_Unpack(10, out, 24, u, 6, &c) == CANTP_FOUND && u[4] == 3 && ISNAN(u[5]));

    /* bad tables */
    double bad[6 * 2]; memcpy(bad, mx, sizeof bad);
    bad[2] = 1;  CHECK(CanTp_DefineMux(10, bad, 6) == CANTP_ERR_MUXDEF);   /* self */
    memcpy(bad, mx, sizeof bad); bad[0] = 1; bad[2] = 0;                  /* 0 -> 1 -> 0 cycle */
    CHECK(CanTp_DefineMux(10, bad, 6) == CANTP_ERR_MUXDEF);
    memcpy(bad, mx, sizeof bad); bad[2] = 6;  CHECK(CanTp_DefineMux(10, bad, 6) == CANTP_ERR_MUXDEF);
    memcpy(bad, mx, sizeof bad); bad[3] = -1; CHECK(CanTp_DefineMux(10, bad, 6) == CANTP_ERR_MUXDEF);
    /* a failed DefineMux keeps the previous table */
    CHECK(CanTp_Pack(10, v, 6, 0, 0, out, 24, &n) == CANTP_OK && ISNAN(u[5]));
    /* Define clears the mux table: everything present again */
    CHECK(CanTp_Define(10, m, 8, s, 6) == CANTP_OK);
    v[0] = 1;
    CHECK(CanTp_Pack(10, v, 6, 0, 0, out, 24, &n) == CANTP_OK);
    CHECK(CanTp_Unpack(10, out, 24, u, 6, &c) == CANTP_FOUND && u[2] == 2000 && u[5] == 9);   /* B overwrote A: last row wins */
    CanTp_Clear(10);
}

/* ----------------------------------------------------------------------- */
/* Session helpers: run one transfer between a sender slot and a receiver slot in memory. */
typedef struct {
    int txFrames, rxFrames, found, dropIndex;
    int32_t lastTx, lastRx;
    uint8_t log[16384]; int logLen;
    uint8_t rxResp[8192]; int32_t rxRespLen;      /* last responses, for inspection */
} Exchange;

static void log_add(Exchange* e, const uint8_t* rec) { if (e->logLen + 24 <= (int)sizeof e->log) { memcpy(e->log + e->logLen, rec, 24); e->logLen += 24; } }

static void run_exchange(Exchange* e, int txSlot, int rxSlot, const double* vals, int n, double* got, uint32_t stepMs)
{
    static uint8_t txout[8192], next[8192], rxout[8192];
    int32_t tw = 0, rw = 0, nw = 0;
    uint32_t now = 1000;
    int drop = e->dropIndex, txIndex = 0;
    e->txFrames = e->rxFrames = e->found = 0; e->logLen = 0; e->rxRespLen = 0;
    e->lastTx = CanTp_TxStart(txSlot, vals, n, now, 0, 0, txout, sizeof txout, &tw);
    for (int iter = 0; iter < 200 && tw > 0; iter++) {
        int32_t pending = 0;
        for (int o = 0; o < tw; o += 24) {
            int idx = txIndex++;
            e->txFrames++;
            if (idx == drop) continue;                                     /* simulate a lost frame */
            log_add(e, txout + o);
            int r = CanTp_RxStep(rxSlot, txout + o, 24, now, 0, got, n, rxout, sizeof rxout, &rw);
            e->lastRx = r;
            if (r == CANTP_FOUND) e->found++;
            if (rw > 0 && rw <= (int)sizeof e->rxResp) { memcpy(e->rxResp, rxout, (size_t)rw); e->rxRespLen = rw; }
            for (int q = 0; q < rw; q += 24) {
                e->rxFrames++;
                log_add(e, rxout + q);
                e->lastTx = CanTp_TxFeed(txSlot, rxout + q, 24, now, 0, 0, next + pending, (int32_t)sizeof next - pending, &nw);
                pending += nw;
            }
        }
        memcpy(txout, next, (size_t)pending); tw = pending;
        now += stepMs;
    }
}

/* ----------------------------------------------------------------------- */
static void test_rts_cts(void)
{
    /* 30-byte Proprietary B message (PDU2 PGN 0xFF20) from SA 0x80 to DA 0x21: 5 DT packets */
    double m[8], s[4 * 8], ses[6];
    msgdef(m, 0x18FF2080, 1, 30, 4, -1, 0x21, 255);
    sigdef(s + 0,  0, 16, 0, 0, 0.1, 0, 0, 0);
    sigdef(s + 8,  16, 16, 0, 1, 1, 0, 0, 0);
    sigdef(s + 16, 100, 8, 0, 0, 1, 0, 0, 0);
    sigdef(s + 24, 232, 8, 0, 0, 1, 0, 0, 0);                             /* last byte */
    CHECK(CanTp_Define(12, m, 8, s, 4) == CANTP_OK);                      /* sender */
    CHECK(CanTp_Define(13, m, 8, s, 4) == CANTP_OK);                      /* receiver (same table) */
    CHECK(CanTp_FrameCount(12) == 6 && CanTp_OutputSize(12) == 144);
    double v[4] = { 123.4, -5, 77, 200 }, u[4]; int32_t n;
    uint8_t buf[256];
    CHECK(CanTp_Pack(12, v, 4, 0, 0, buf, sizeof buf, &n) == CANTP_ERR_TRANSPORT);   /* handshake needed */
    CHECK(CanTp_TxState(12) == 0 && CanTp_RxState(13) == 0);

    static Exchange e;
    e.dropIndex = -1;
    run_exchange(&e, 12, 13, v, 4, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && CanTp_TxState(12) == 2);
    CHECK(e.txFrames == 6 && e.rxFrames == 2);                             /* RTS + 5 DT; CTS + EndOfMsgAck */
    CHECKF(u[0], 123.4); CHECK(u[1] == -5 && u[2] == 77 && u[3] == 200);
    /* frame contents from the log: RTS, CTS, DT1..5, EoMA */
    const uint8_t* f = e.log;
    CHECK(rd_u32(f + 8) == (0x1CEC2180u | CANTP_XNET_EXTENDED_ID_FLAG));
    CHECK(f[16] == 0x10 && f[17] == 30 && f[18] == 0 && f[19] == 5 && f[20] == 255 && f[21] == 0x20 && f[22] == 0xFF && f[23] == 0x00);
    f = e.log + 24;
    CHECK(rd_u32(f + 8) == (0x1CEC8021u | CANTP_XNET_EXTENDED_ID_FLAG));  /* CTS from 0x21 to 0x80 */
    CHECK(f[16] == 0x11 && f[17] == 5 && f[18] == 1 && f[19] == 0xFF && f[20] == 0xFF && f[21] == 0x20);
    f = e.log + 48;
    CHECK(rd_u32(f + 8) == (0x1CEB2180u | CANTP_XNET_EXTENDED_ID_FLAG) && f[16] == 1);
    f = e.log + 24 * 6;
    CHECK(rd_u32(f + 8) == (0x1CEB2180u | CANTP_XNET_EXTENDED_ID_FLAG) && f[16] == 5 && f[18] == 200 && f[19] == 0xFF);
    f = e.log + 24 * 7;
    CHECK(rd_u32(f + 8) == (0x1CEC8021u | CANTP_XNET_EXTENDED_ID_FLAG) && f[16] == 0x13 && f[17] == 30 && f[19] == 5);
    /* stateless reassembly from the log (both directions interleaved) */
    int32_t c; double w[4] = { 0, 0, 0, 0 };
    CHECK(CanTp_Unpack(13, e.log, e.logLen, w, 4, &c) == CANTP_FOUND && c == e.logLen - 24 && w[3] == 200);   /* stops at DT5, before the EoMA */
    CHECK(CanTp_Unpack(12, e.log, e.logLen, w, 4, &c) == CANTP_FOUND);   /* the sender's slot decodes it too */

    /* receiver block size 2: CTS(2,1) DT1 DT2 CTS(2,3) DT3 DT4 CTS(1,5) DT5 EoMA */
    ses[0] = -1; ses[1] = -1; ses[2] = 2; ses[3] = 0; ses[4] = 0; ses[5] = 0;
    CHECK(CanTp_SessionConfig(13, ses, 6) == CANTP_OK);
    run_exchange(&e, 12, 13, v, 4, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && e.txFrames == 6 && e.rxFrames == 4 && u[2] == 77);
    CHECK(e.log[24 * 4 + 16] == 0x11 && e.log[24 * 4 + 17] == 2 && e.log[24 * 4 + 18] == 3);   /* second CTS asks 2 from 3 */
    ses[2] = 0; CHECK(CanTp_SessionConfig(13, ses, 6) == CANTP_OK);

    /* sender limits packets per CTS to 3 (RTS byte 4): receiver windows follow it */
    ses[5] = 3; CHECK(CanTp_SessionConfig(12, ses, 6) == CANTP_OK);
    run_exchange(&e, 12, 13, v, 4, u, 10);
    CHECK(e.found == 1 && e.txFrames == 6 && e.rxFrames == 3 && e.log[20] == 3 && e.log[24 + 17] == 3);
    ses[5] = 0; CHECK(CanTp_SessionConfig(12, ses, 6) == CANTP_OK);

    /* lost DT2: receiver re-requests from 2, sender resends 2..5 */
    e.dropIndex = 2;
    run_exchange(&e, 12, 13, v, 4, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && e.txFrames == 10 && e.rxFrames == 3);   /* RTS + 5 + 4 resent; CTS, CTS(2), EoMA */
    CHECKF(u[0], 123.4); CHECK(u[3] == 200);
    e.dropIndex = -1;

    /* hold (CTS with 0 packets), then go */
    uint8_t rts[24], cts[24], dt[8192]; int32_t w1, w2;
    CHECK(CanTp_TxStart(12, v, 4, 5000, 0, 0, rts, 24, &w1) == CANTP_OK && w1 == 24 && CanTp_TxState(12) == 1);
    CHECK(CanTp_TxStart(12, v, 4, 5000, 0, 0, rts, 24, &w1) == CANTP_ERR_BUSY);
    uint8_t hold[8] = { 0x11, 0, 1, 0xFF, 0xFF, 0x20, 0xFF, 0x00 };
    CanTp_MakeRecord(0x1CEC8021, 1, 0, hold, 8, 0, cts, 24);
    CHECK(CanTp_TxFeed(12, cts, 24, 5100, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 0);
    CHECK(CanTp_TxFeed(12, NULL, 0, 6200, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 0);      /* 1100 ms since the hold */
    hold[1] = 5;
    CanTp_MakeRecord(0x1CEC8021, 1, 0, hold, 8, 0, cts, 24);
    CHECK(CanTp_TxFeed(12, cts, 24, 6300, 0, 500000, dt, sizeof dt, &w2) == CANTP_OK && w2 == 120);
    CHECK(rd_u64(dt + 24 * 4) == 4 * 500000ULL);                          /* spacing applied to the DT timestamps */
    CHECK(CanTp_TxFeed(12, cts, 24, 6300, 0, 0, dt, 50, &w2) == CANTP_ERR_BUFFER && w2 == 120);
    /* sender timeout waiting for the EndOfMsgAck: abort frame, state -10 */
    CHECK(CanTp_TxFeed(12, NULL, 0, 7000, 0, 0, dt, sizeof dt, &w2) == CANTP_OK);
    CHECK(CanTp_TxFeed(12, NULL, 0, 7600, 0, 0, dt, sizeof dt, &w2) == CANTP_ERR_TIMEOUT && w2 == 24);
    CHECK(rd_u32(dt + 8) == (0x1CEC2180u | CANTP_XNET_EXTENDED_ID_FLAG) && dt[16] == 0xFF && dt[17] == 3);
    CHECK(CanTp_TxState(12) == CANTP_ERR_TIMEOUT);
    CHECK(CanTp_TxFeed(12, NULL, 0, 7700, 0, 0, dt, sizeof dt, &w2) == CANTP_ERR_TIMEOUT && w2 == 0);
    CHECK(CanTp_TxReset(12) == CANTP_OK && CanTp_TxState(12) == 0);

    /* peer abort, and a CTS pointing past the end */
    CHECK(CanTp_TxStart(12, v, 4, 8000, 0, 0, rts, 24, &w1) == CANTP_OK);
    uint8_t ab[8] = { 0xFF, 1, 0xFF, 0xFF, 0xFF, 0x20, 0xFF, 0x00 };
    CanTp_MakeRecord(0x1CEC8021, 1, 0, ab, 8, 0, cts, 24);
    CHECK(CanTp_TxFeed(12, cts, 24, 8010, 0, 0, dt, sizeof dt, &w2) == CANTP_ERR_ABORTED && w2 == 0);
    CanTp_TxReset(12);
    CHECK(CanTp_TxStart(12, v, 4, 8000, 0, 0, rts, 24, &w1) == CANTP_OK);
    hold[1] = 5; hold[2] = 9;
    CanTp_MakeRecord(0x1CEC8021, 1, 0, hold, 8, 0, cts, 24);
    CHECK(CanTp_TxFeed(12, cts, 24, 8010, 0, 0, dt, sizeof dt, &w2) == CANTP_ERR_ABORTED && w2 == 24 && dt[16] == 0xFF);
    CanTp_TxReset(12);
    /* frames not for us are ignored (wrong PS, wrong PGN in the CTS) */
    CHECK(CanTp_TxStart(12, v, 4, 9000, 0, 0, rts, 24, &w1) == CANTP_OK);
    hold[2] = 1;
    CanTp_MakeRecord(0x1CEC8121, 1, 0, hold, 8, 0, cts, 24);             /* PS 0x81 */
    CHECK(CanTp_TxFeed(12, cts, 24, 9010, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 0);
    hold[5] = 0x21;
    CanTp_MakeRecord(0x1CEC8021, 1, 0, hold, 8, 0, cts, 24);             /* other PGN */
    CHECK(CanTp_TxFeed(12, cts, 24, 9020, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 0 && CanTp_TxState(12) == 1);
    CanTp_TxReset(12);

    /* receiver side alone: RTS -> CTS; timeout waiting for DT -> abort; bad sequence twice -> abort */
    CHECK(CanTp_TxStart(12, v, 4, 10000, 0, 0, rts, 24, &w1) == CANTP_OK);
    uint8_t resp[64]; int32_t rw;
    CHECK(CanTp_RxStep(13, rts, 24, 10000, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 24 && resp[16] == 0x11 && CanTp_RxState(13) == 1);
    CHECK(CanTp_RxStep(13, NULL, 0, 10700, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 0);
    CHECK(CanTp_RxStep(13, NULL, 0, 10800, 0, u, 4, resp, sizeof resp, &rw) == CANTP_ERR_TIMEOUT && rw == 24);
    CHECK(rd_u32(resp + 8) == (0x1CEC8021u | CANTP_XNET_EXTENDED_ID_FLAG) && resp[16] == 0xFF && resp[17] == 3 && CanTp_RxState(13) == 0);
    CanTp_TxReset(12);
    CHECK(CanTp_TxStart(12, v, 4, 11000, 0, 0, rts, 24, &w1) == CANTP_OK);
    CHECK(CanTp_RxStep(13, rts, 24, 11000, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 24);
    CHECK(CanTp_TxFeed(12, resp, 24, 11010, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 120);
    CHECK(CanTp_RxStep(13, dt + 24 * 2, 24, 11020, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 0);   /* DT3 alone: wait for the window */
    CHECK(CanTp_RxStep(13, dt + 24 * 4, 24, 11030, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 24 && resp[16] == 0x11 && resp[17] == 5 && resp[18] == 1);   /* DT5 ends the window: ask from 1 */
    CHECK(CanTp_RxStep(13, dt + 24 * 4, 24, 11040, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 24 && resp[18] == 1);   /* second re-request */
    CHECK(CanTp_RxStep(13, dt + 24 * 4, 24, 11050, 0, u, 4, resp, sizeof resp, &rw) == CANTP_ERR_ABORTED && rw == 24 && resp[16] == 0xFF && resp[17] == 5);
    CHECK(CanTp_RxState(13) == 0);
    /* out-of-order arrival within a window is fine */
    CanTp_TxReset(12);
    CHECK(CanTp_TxStart(12, v, 4, 12000, 0, 0, rts, 24, &w1) == CANTP_OK);
    CHECK(CanTp_RxStep(13, rts, 24, 12000, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 24);
    CHECK(CanTp_TxFeed(12, resp, 24, 12010, 0, 0, dt, sizeof dt, &w2) == CANTP_OK && w2 == 120);
    for (int k = 4; k >= 1; k--) CHECK(CanTp_RxStep(13, dt + 24 * (k - 1), 24, 12020, 0, u, 4, resp, sizeof resp, &rw) == CANTP_OK && rw == 0);
    CHECK(CanTp_RxStep(13, dt + 24 * 4, 24, 12030, 0, u, 4, resp, sizeof resp, &rw) == CANTP_FOUND && rw == 24 && resp[16] == 0x13 && u[3] == 200);
    CanTp_TxReset(12);

    /* SA placeholder on the receiver: any sender, answers go back to that sender */
    m[4] = -1; m[0] = 0x18FF20FE;
    CHECK(CanTp_Define(14, m, 8, s, 4) == CANTP_OK);
    run_exchange(&e, 12, 14, v, 4, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && rd_u32(e.log + 24 + 8) == (0x1CEC8021u | CANTP_XNET_EXTENDED_ID_FLAG));

    /* a short RTS/CTS message (<= 8 bytes) is one PDU frame: TxStart completes at once, Pack works too */
    msgdef(m, 0x18EA0080, 1, 3, 4, -1, 0x21, 255);                        /* PDU1 PGN 0xEA00 (request) to DA 0x21 */
    sigdef(s, 0, 24, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(15, m, 8, s, 1) == CANTP_OK && CanTp_FrameCount(15) == 1);
    double one = 0xFEE3;
    CHECK(CanTp_TxStart(15, &one, 1, 0, 7, 0, buf, sizeof buf, &n) == CANTP_DONE && n == 24);
    CHECK(rd_u32(buf + 8) == (0x18EA2180u | CANTP_XNET_EXTENDED_ID_FLAG) && buf[16] == 0xE3 && buf[17] == 0xFE && buf[18] == 0);
    CHECK(CanTp_Pack(15, &one, 1, 0, 0, buf, sizeof buf, &n) == CANTP_OK && n == 24);
    CHECK(CanTp_RxStep(15, buf, 24, 0, 0, u, 1, resp, sizeof resp, &rw) == CANTP_FOUND && u[0] == 0xFEE3 && rw == 0);
    CanTp_MakeRecord(0x18EA2280, 1, 0, buf + 16, 3, 0, buf, 24);            /* addressed to 0x22: not ours */
    CHECK(CanTp_RxStep(15, buf, 24, 0, 0, u, 1, resp, sizeof resp, &rw) == CANTP_OK);
    for (int i = 12; i <= 15; i++) CanTp_Clear(i);
}

/* ----------------------------------------------------------------------- */
static void test_isotp(void)
{
    /* 20-byte diagnostic-style message on 0x7E0, flow control expected on 0x7E8 (default): FF + 2 CF */
    double m[8], s[3 * 8], ses[6];
    msgdef(m, 0x7E0, 0, 20, 5, -1, 255, 0xCC);
    sigdef(s + 0,  0, 32, 0, 0, 1, 0, 0, 0);
    sigdef(s + 8,  32, 8, 0, 0, 1, 0, 0, 0);
    sigdef(s + 16, 152, 8, 0, 0, 1, 0, 0, 0);                              /* last byte */
    CHECK(CanTp_Define(16, m, 8, s, 3) == CANTP_OK && CanTp_Define(17, m, 8, s, 3) == CANTP_OK);
    CHECK(CanTp_FrameCount(16) == 3 && CanTp_OutputSize(16) == 72);
    double v[3] = { 0x11223344, 0x55, 0x99 }, u[3]; int32_t n, c;
    uint8_t buf[256], resp[64]; int32_t rw;
    CHECK(CanTp_Pack(16, v, 3, 0, 0, buf, sizeof buf, &n) == CANTP_ERR_TRANSPORT);

    static Exchange e;
    e.dropIndex = -1;
    run_exchange(&e, 16, 17, v, 3, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && e.txFrames == 3 && e.rxFrames == 1);
    CHECK(u[0] == 0x11223344 && u[1] == 0x55 && u[2] == 0x99);
    const uint8_t* f = e.log;
    CHECK(rd_u32(f + 8) == 0x7E0 && f[15] == 8 && f[16] == 0x10 && f[17] == 20 && f[18] == 0x44 && f[19] == 0x33 && f[22] == 0x55);
    f = e.log + 24;
    CHECK(rd_u32(f + 8) == 0x7E8 && f[16] == 0x30 && f[17] == 0 && f[18] == 0 && f[19] == 0xCC);   /* FC: CTS, BS 0, STmin 0, pad */
    f = e.log + 48;
    CHECK(rd_u32(f + 8) == 0x7E0 && f[16] == 0x21);
    f = e.log + 72;
    CHECK(f[16] == 0x22 && f[23] == 0x99);                                 /* byte 19 = 6 + 7 + 6 -> CF2 data[6] */
    CHECK(CanTp_Unpack(17, e.log, e.logLen, u, 3, &c) == CANTP_FOUND && u[0] == 0x11223344);

    /* single frame: 5-byte message */
    double m1[8], s1[8]; msgdef(m1, 0x7E0, 0, 5, 5, -1, 255, 0xCC); sigdef(s1, 0, 40, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(18, m1, 8, s1, 1) == CANTP_OK && CanTp_FrameCount(18) == 1);
    double one = 0xAABBCCDDEEULL, o1;
    CHECK(CanTp_Pack(18, &one, 1, 0, 0, buf, sizeof buf, &n) == CANTP_OK && n == 24 && buf[16] == 0x05 && buf[17] == 0xEE && buf[21] == 0xAA && buf[22] == 0xCC);
    CHECK(CanTp_TxStart(18, &one, 1, 0, 0, 0, buf, sizeof buf, &n) == CANTP_DONE && n == 24);
    CHECK(CanTp_RxStep(18, buf, 24, 0, 0, &o1, 1, resp, sizeof resp, &rw) == CANTP_FOUND && o1 == (double)0xAABBCCDDEEULL && rw == 0);

    /* block size 2 + STmin 20 ms on the receiver: 41 bytes = FF + 5 CF -> FC, 2 CF, FC, 2 CF, FC, 1 CF */
    double m2[8], s2[2 * 8]; msgdef(m2, 0x7E0, 0, 41, 5, -1, 255, 0xCC);
    sigdef(s2, 0, 8, 0, 0, 1, 0, 0, 0); sigdef(s2 + 8, 320, 8, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(19, m2, 8, s2, 2) == CANTP_OK && CanTp_Define(20, m2, 8, s2, 2) == CANTP_OK);
    ses[0] = -1; ses[1] = -1; ses[2] = 2; ses[3] = 20; ses[4] = 0; ses[5] = 0;
    CHECK(CanTp_SessionConfig(20, ses, 6) == CANTP_OK);
    double v2[2] = { 7, 8 }, u2[2];
    run_exchange(&e, 19, 20, v2, 2, u2, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && e.txFrames == 6 && e.rxFrames == 3 && u2[0] == 7 && u2[1] == 8);
    CHECK(e.log[24 + 17] == 2 && e.log[24 + 18] == 20);                    /* FC announces BS 2, STmin 20 */
    CHECK(rd_u64(e.log + 24 * 3) - rd_u64(e.log + 24 * 2) == 200000ULL);   /* CF timestamps 20 ms apart */

    /* sequence number wrap: 6 + 7*20 = 146 bytes, CFs 1..15, 0, 1..4 */
    double m3[8], s3[2 * 8]; msgdef(m3, 0x7E0, 0, 146, 5, -1, 255, 0xCC);
    sigdef(s3, 0, 8, 0, 0, 1, 0, 0, 0); sigdef(s3 + 8, 1160, 8, 0, 0, 1, 0, 0, 0);
    CHECK(CanTp_Define(21, m3, 8, s3, 2) == CANTP_OK && CanTp_Define(22, m3, 8, s3, 2) == CANTP_OK);
    double v3[2] = { 1, 2 };
    run_exchange(&e, 21, 22, v3, 2, u2, 10);
    CHECK(e.found == 1 && e.txFrames == 21 && u2[0] == 1 && u2[1] == 2);
    CHECK(e.log[24 * 16 + 16] == 0x2F && e.log[24 * 17 + 16] == 0x20 && e.log[24 * 18 + 16] == 0x21);
    CHECK(CanTp_Unpack(22, e.log, e.logLen, u2, 2, &c) == CANTP_FOUND && u2[1] == 2);

    /* flow control: wait, then overflow */
    uint8_t ff[24], fc[24], cf[8192]; int32_t w1, w2;
    CHECK(CanTp_TxStart(16, v, 3, 100, 0, 0, ff, 24, &w1) == CANTP_OK && CanTp_TxState(16) == 1);
    uint8_t fcw[8] = { 0x31, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    CanTp_MakeRecord(0x7E8, 0, 0, fcw, 8, 0, fc, 24);
    CHECK(CanTp_TxFeed(16, fc, 24, 900, 0, 0, cf, sizeof cf, &w2) == CANTP_OK && w2 == 0);
    CHECK(CanTp_TxFeed(16, NULL, 0, 1800, 0, 0, cf, sizeof cf, &w2) == CANTP_OK);            /* timer restarted by the wait */
    fcw[0] = 0x32;
    CanTp_MakeRecord(0x7E8, 0, 0, fcw, 8, 0, fc, 24);
    CHECK(CanTp_TxFeed(16, fc, 24, 1810, 0, 0, cf, sizeof cf, &w2) == CANTP_ERR_ABORTED && w2 == 0 && CanTp_TxState(16) == CANTP_ERR_ABORTED);
    CanTp_TxReset(16);
    /* sender timeout without any flow control */
    CHECK(CanTp_TxStart(16, v, 3, 2000, 0, 0, ff, 24, &w1) == CANTP_OK);
    CHECK(CanTp_TxFeed(16, NULL, 0, 2900, 0, 0, cf, sizeof cf, &w2) == CANTP_OK);
    CHECK(CanTp_TxFeed(16, NULL, 0, 3100, 0, 0, cf, sizeof cf, &w2) == CANTP_ERR_TIMEOUT && w2 == 0);
    CanTp_TxReset(16);
    /* FC on another id is ignored; custom peer id via SessionConfig */
    ses[0] = 0x7F0; ses[1] = 0; ses[2] = 0; ses[3] = 0; ses[4] = 0; ses[5] = 0;
    CHECK(CanTp_SessionConfig(16, ses, 6) == CANTP_OK);
    CHECK(CanTp_TxStart(16, v, 3, 4000, 0, 0, ff, 24, &w1) == CANTP_OK);
    fcw[0] = 0x30;
    CanTp_MakeRecord(0x7E8, 0, 0, fcw, 8, 0, fc, 24);
    CHECK(CanTp_TxFeed(16, fc, 24, 4010, 0, 0, cf, sizeof cf, &w2) == CANTP_OK && w2 == 0);
    CanTp_MakeRecord(0x7F0, 0, 0, fcw, 8, 0, fc, 24);
    CHECK(CanTp_TxFeed(16, fc, 24, 4020, 0, 0, cf, sizeof cf, &w2) == CANTP_DONE && w2 == 48);
    CHECK(CanTp_SessionConfig(16, ses, 5) == CANTP_ERR_ARG);
    ses[0] = 0x800; CHECK(CanTp_SessionConfig(16, ses, 6) == CANTP_ERR_ARG);   /* 11-bit peer id out of range */
    /* receiver: timeout waiting for CF, wrong CF sequence */
    CHECK(CanTp_RxStep(17, ff, 24, 5000, 0, u, 3, resp, sizeof resp, &rw) == CANTP_OK && rw == 24 && CanTp_RxState(17) == 1);
    CHECK(CanTp_RxStep(17, NULL, 0, 5900, 0, u, 3, resp, sizeof resp, &rw) == CANTP_OK);
    CHECK(CanTp_RxStep(17, NULL, 0, 6100, 0, u, 3, resp, sizeof resp, &rw) == CANTP_ERR_TIMEOUT && rw == 0 && CanTp_RxState(17) == 0);
    CHECK(CanTp_RxStep(17, ff, 24, 7000, 0, u, 3, resp, sizeof resp, &rw) == CANTP_OK);
    CHECK(CanTp_RxStep(17, cf + 24, 24, 7010, 0, u, 3, resp, sizeof resp, &rw) == CANTP_ERR_ABORTED);   /* CF2 before CF1 */
    CHECK(CanTp_RxState(17) == 0);
    CHECK(CanTp_RxStep(17, cf, 24, 7020, 0, u, 3, resp, sizeof resp, &rw) == CANTP_OK && rw == 0);     /* stray CF ignored */

    /* 29-bit normal fixed addressing: 0x18DA2180 (to 0x21 from 0x80), FC on 0x18DA8021 by default */
    double m4[8]; msgdef(m4, 0x18DA2180, 1, 20, 5, -1, 255, 0xCC);
    CHECK(CanTp_Define(23, m4, 8, s, 3) == CANTP_OK && CanTp_Define(24, m4, 8, s, 3) == CANTP_OK);
    run_exchange(&e, 23, 24, v, 3, u, 10);
    CHECK(e.found == 1 && e.lastTx == CANTP_DONE && u[2] == 0x99);
    CHECK(rd_u32(e.log + 8) == (0x18DA2180u | CANTP_XNET_EXTENDED_ID_FLAG));
    CHECK(rd_u32(e.log + 24 + 8) == (0x18DA8021u | CANTP_XNET_EXTENDED_ID_FLAG) && e.log[24 + 16] == 0x30);

    /* every transport goes through the session calls: BAM completes at TxStart, RxStep decodes it */
    double mb[8]; msgdef(mb, 0x18FF0080, 1, 20, 1, -1, 255, 255);
    CHECK(CanTp_Define(25, mb, 8, s, 3) == CANTP_OK);
    CHECK(CanTp_TxStart(25, v, 3, 0, 0, 0, cf, sizeof cf, &w1) == CANTP_DONE && w1 == 96 && CanTp_TxState(25) == 2);
    int foundBam = 0;
    for (int o = 0; o < 96; o += 24) if (CanTp_RxStep(25, cf + o, 24, 0, 0, u, 3, resp, sizeof resp, &rw) == CANTP_FOUND) foundBam++;
    CHECK(foundBam == 1 && u[0] == 0x11223344);
    for (int i = 16; i <= 25; i++) CanTp_Clear(i);
}
