/* test_main.c - unit tests, compiled together with the sources in ../src (no DLL needed). */
#include "../src/tempctl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKF(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-4)

/* ----------------------------------------------------------------------- */
static float cfg[TC_SIGNAL_COUNT_EXT];
static float out[TC_SIGNAL_COUNT_EXT];

static void base_cfg(void)
{
    memset(cfg, 0, sizeof cfg);
    cfg[TC_HI_LIMIT] = 100; cfg[TC_LO_LIMIT] = 0;
    cfg[TC_HI_DEADBAND] = 60; cfg[TC_LO_DEADBAND] = 40;
    cfg[TC_SETPOINT] = 50; cfg[TC_ACTUAL_TEMP] = 50;
    cfg[TC_ERROR_TIMEOUT] = 1000; cfg[TC_DEADBAND_TIMEOUT] = 500;
}
static int32_t st(int zone, int action, uint32_t ms, float temp)
{
    cfg[TC_ACTUAL_TEMP] = temp;
    return TcStep(zone, action, ms, cfg, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT_EXT);
}
#define HEAT out[TC_HEATING_ACTIVE]
#define COOL out[TC_COOLING_ACTIVE]
#define ERR  out[TC_ERROR_STATUS]

static void test_controller_basic(void)
{
    base_cfg();
    CHECK(st(0, TC_ACTION_INIT, 0, 50) == TC_OK);
    CHECK(HEAT == 0 && COOL == 0 && ERR == 0);
    CHECKF(out[TC_HI_LIMIT], 100);                       /* config echoed */

    /* below LoDeadband: countdown, then heat */
    CHECK(st(0, TC_ACTION_STEP, 100, 30) == TC_OK);
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 500);
    CHECK(st(0, TC_ACTION_STEP, 400, 30) == TC_OK);
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 200);
    CHECK(st(0, TC_ACTION_STEP, 600, 30) == TC_OK);
    CHECK(HEAT == 1 && COOL == 0); CHECKF(out[TC_DB_REMAIN_MS], 0);

    /* heating persists until >= setpoint, even while back inside the deadband */
    st(0, TC_ACTION_STEP, 700, 45);  CHECK(HEAT == 1);
    st(0, TC_ACTION_STEP, 800, 49.9f); CHECK(HEAT == 1);
    st(0, TC_ACTION_STEP, 900, 50.0f); CHECK(HEAT == 0 && COOL == 0);

    /* above HiDeadband: countdown, then cool */
    st(0, TC_ACTION_STEP, 1000, 70); CHECK(COOL == 0);
    st(0, TC_ACTION_STEP, 1499, 70); CHECK(COOL == 0);
    st(0, TC_ACTION_STEP, 1500, 70); CHECK(COOL == 1 && HEAT == 0);
    st(0, TC_ACTION_STEP, 1600, 55); CHECK(COOL == 1);
    st(0, TC_ACTION_STEP, 1700, 49); CHECK(COOL == 0 && HEAT == 0);
}

static void test_deadband_countdown_resets(void)
{
    base_cfg();
    st(1, TC_ACTION_INIT, 0, 50);
    st(1, TC_ACTION_STEP, 300, 30);   /* 300 ms below */
    st(1, TC_ACTION_STEP, 400, 50);   /* back inside: countdown cleared */
    CHECKF(out[TC_DB_REMAIN_MS], 0);
    st(1, TC_ACTION_STEP, 500, 30);   /* full 500 ms needed again */
    st(1, TC_ACTION_STEP, 900, 30);   CHECK(HEAT == 0);
    st(1, TC_ACTION_STEP, 1000, 30);  CHECK(HEAT == 1);
}

static void test_fault_latch_and_reset(void)
{
    base_cfg();
    st(2, TC_ACTION_INIT, 0, 50);
    st(2, TC_ACTION_STEP, 100, 120);  CHECK(ERR == 0); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    st(2, TC_ACTION_STEP, 900, 120);  CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 1100, 120); CHECK(ERR == 1 && HEAT == 0 && COOL == 0);
    st(2, TC_ACTION_STEP, 1200, 50);  CHECK(ERR == 1);          /* latched */
    st(2, TC_ACTION_STEP, 2000, 30);  CHECK(ERR == 1 && HEAT == 0);
    CHECK(st(2, TC_ACTION_RESET, 2100, 50) == TC_OK);
    CHECK(ERR == 0 && HEAT == 0 && COOL == 0);
    st(2, TC_ACTION_STEP, 2200, 50);  CHECK(ERR == 0);

    /* low limit -> 2; countdown resets if it clears in time */
    st(2, TC_ACTION_STEP, 2300, -5);
    st(2, TC_ACTION_STEP, 3200, 10);  CHECK(ERR == 0);            /* 900 ms then cleared */
    st(2, TC_ACTION_STEP, 3300, -5);
    st(2, TC_ACTION_STEP, 4200, -5);  CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 4300, -5);  CHECK(ERR == 2);
    /* reset with the fault condition still present: counts again from full,
       starting at the first STEP that sees it */
    st(2, TC_ACTION_RESET, 4400, -5); CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 4500, -5);  CHECK(ERR == 0); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    st(2, TC_ACTION_STEP, 5400, -5);  CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 5500, -5);  CHECK(ERR == 2);
}

static void test_fault_while_heating(void)
{
    base_cfg();
    st(3, TC_ACTION_INIT, 0, 50);
    st(3, TC_ACTION_STEP, 100, 30);
    st(3, TC_ACTION_STEP, 600, 30);  CHECK(HEAT == 1);
    /* heater failing: temp keeps falling through LoLimit while heating */
    st(3, TC_ACTION_STEP, 700, -5);  CHECK(HEAT == 1 && ERR == 0);  /* still heating during countdown */
    st(3, TC_ACTION_STEP, 1600, -5); CHECK(HEAT == 1 && ERR == 0);
    st(3, TC_ACTION_STEP, 1700, -5); CHECK(ERR == 2 && HEAT == 0);
    /* overshoot past the far limit simply ends the heat cycle at setpoint */
    st(3, TC_ACTION_RESET, 1800, 50);
    st(3, TC_ACTION_STEP, 1900, 30);
    st(3, TC_ACTION_STEP, 2400, 30); CHECK(HEAT == 1);
    st(3, TC_ACTION_STEP, 2500, 120); CHECK(HEAT == 0 && ERR == 0);
}

static void test_bad_reading(void)
{
    base_cfg();
    st(4, TC_ACTION_INIT, 0, 50);
    st(4, TC_ACTION_STEP, 100, 30);
    st(4, TC_ACTION_STEP, 600, 30);  CHECK(HEAT == 1);
    st(4, TC_ACTION_STEP, 700, NAN); CHECK(HEAT == 0 && COOL == 0 && ERR == 0); /* relays off at once */
    st(4, TC_ACTION_STEP, 800, 45);  CHECK(ERR == 0 && HEAT == 0);              /* recovered, idle */
    st(4, TC_ACTION_STEP, 900, INFINITY);
    st(4, TC_ACTION_STEP, 1800, INFINITY); CHECK(ERR == 0);
    st(4, TC_ACTION_STEP, 1900, INFINITY); CHECK(ERR == 3);
    st(4, TC_ACTION_STEP, 2000, 50); CHECK(ERR == 3);
}

static void test_init_state_and_live_setpoint(void)
{
    base_cfg();
    cfg[TC_HEATING_ACTIVE] = 1;
    CHECK(st(5, TC_ACTION_INIT, 0, 30) == TC_OK);
    CHECK(HEAT == 1 && COOL == 0);
    cfg[TC_HEATING_ACTIVE] = 0;              /* input relay fields are ignored on STEP */
    st(5, TC_ACTION_STEP, 100, 45); CHECK(HEAT == 1);
    cfg[TC_SETPOINT] = 45;                   /* live setpoint change */
    st(5, TC_ACTION_STEP, 200, 45); CHECK(HEAT == 0);

    cfg[TC_HEATING_ACTIVE] = 1; cfg[TC_COOLING_ACTIVE] = 1;   /* heat wins if both set */
    st(6, TC_ACTION_INIT, 0, 50); CHECK(HEAT == 1 && COOL == 0);
    cfg[TC_HEATING_ACTIVE] = 0;
    st(6, TC_ACTION_INIT, 0, 50); CHECK(HEAT == 0 && COOL == 1);
    cfg[TC_COOLING_ACTIVE] = 0;
}

static void test_zero_timeouts_and_wrap(void)
{
    base_cfg();
    cfg[TC_ERROR_TIMEOUT] = 0; cfg[TC_DEADBAND_TIMEOUT] = 0;
    st(7, TC_ACTION_INIT, 0, 50);
    st(7, TC_ACTION_STEP, 10, 30);  CHECK(HEAT == 1);             /* zero timeout: immediate */
    st(7, TC_ACTION_STEP, 20, 120); CHECK(ERR == 1 && HEAT == 0);

    base_cfg();
    st(8, TC_ACTION_INIT, 0xFFFFFF00u, 50);
    st(8, TC_ACTION_STEP, 0xFFFFFF80u, 30);
    st(8, TC_ACTION_STEP, 0x00000100u, 30);                   /* dt = 384 across wrap */
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 116);
    st(8, TC_ACTION_STEP, 0x00000180u, 30);
    CHECK(HEAT == 1);
}

static void test_errors_and_aliasing(void)
{
    base_cfg();
    CHECK(TcStep(9, TC_ACTION_STEP, 0, cfg, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_NOT_INIT);
    CHECK(TcStep(-1, TC_ACTION_INIT, 0, cfg, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ZONE);
    CHECK(TcStep(TC_MAX_ZONES, TC_ACTION_INIT, 0, cfg, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ZONE);
    CHECK(TcStep(0, 99, 0, cfg, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ACTION);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, cfg, 10, out, TC_SIGNAL_COUNT) == TC_ERR_ARG);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, cfg, TC_SIGNAL_COUNT, out, 10) == TC_ERR_ARG);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, NULL, TC_SIGNAL_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ARG);

    /* config warning, still initialises */
    cfg[TC_SETPOINT] = 70;                                    /* above HiDeadband */
    CHECK(st(9, TC_ACTION_INIT, 0, 50) == TC_WARN_CONFIG);
    CHECK(st(9, TC_ACTION_STEP, 100, 50) == TC_WARN_CONFIG);  /* reported every tick */
    cfg[TC_SETPOINT] = 50;
    CHECK(st(9, TC_ACTION_STEP, 200, 50) == TC_OK);

    /* in-place: in == out, 11-element array */
    base_cfg();
    float io[TC_SIGNAL_COUNT]; memcpy(io, cfg, sizeof io);
    CHECK(TcStep(10, TC_ACTION_INIT, 0, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    io[TC_ACTUAL_TEMP] = 30;
    CHECK(TcStep(10, TC_ACTION_STEP, 100, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    CHECK(TcStep(10, TC_ACTION_STEP, 600, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    CHECK(io[TC_HEATING_ACTIVE] == 1 && io[TC_ACTUAL_TEMP] == 30 && io[TC_HI_LIMIT] == 100);
}

/* ----------------------------------------------------------------------- */
static uint32_t rd_u32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd_u64(const uint8_t* p) { return rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32); }

static void test_bam(void)
{
    float sig[TC_SIGNAL_COUNT] = { 100, 0, 60, 40, 50, 23.5f, 1000, 500, 0, 1, 0 };
    uint8_t buf[8 * TC_RAW_FRAME_SIZE];
    int32_t n = 0;

    CHECK(TcJ1939BamFrameCount(44) == 8);
    CHECK(TcJ1939BamFrameCount(8) == 1);
    CHECK(TcJ1939BamFrameCount(9) == 3);
    CHECK(TcJ1939BamFrameCount(1785) == 256);
    CHECK(TcJ1939BamFrameCount(1786) == 0);

    /* too small -> required size reported */
    CHECK(TcEncodeFrames(sig, 11, TC_DEFAULT_PGN, TC_DEFAULT_SA, TC_DEFAULT_PRIORITY, 0, 0, buf, 10, &n) == TC_ERR_BUFFER);
    CHECK(n == 192);

    uint64_t t0 = 133700000000000000ULL, sp = 500000ULL;   /* 50 ms */
    CHECK(TcEncodeFrames(sig, 11, TC_DEFAULT_PGN, TC_DEFAULT_SA, TC_DEFAULT_PRIORITY, t0, sp, buf, sizeof buf, &n) == TC_OK);
    CHECK(n == 192);

    /* TP.CM BAM */
    const uint8_t* f = buf;
    CHECK(rd_u64(f) == t0);
    CHECK(rd_u32(f + 8) == (0x1CECFF80u | TC_XNET_EXTENDED_ID_FLAG));
    CHECK(f[12] == 0 && f[13] == 0 && f[14] == 0 && f[15] == 8);
    CHECK(f[16] == 0x20 && f[17] == 44 && f[18] == 0 && f[19] == 7 && f[20] == 0xFF);
    CHECK(f[21] == 0x00 && f[22] == 0xFF && f[23] == 0x00);

    /* TP.DT: reassemble */
    uint8_t asm_[49]; memset(asm_, 0, sizeof asm_);
    for (int i = 0; i < 7; i++) {
        f = buf + (i + 1) * TC_RAW_FRAME_SIZE;
        CHECK(rd_u64(f) == t0 + (uint64_t)(i + 1) * sp);
        CHECK(rd_u32(f + 8) == (0x1CEBFF80u | TC_XNET_EXTENDED_ID_FLAG));
        CHECK(f[15] == 8);
        CHECK(f[16] == i + 1);
        memcpy(asm_ + i * 7, f + 17, 7);
    }
    uint8_t expect[44];
    memcpy(expect, sig, 44);              /* host is little-endian x64 */
    CHECK(memcmp(asm_, expect, 44) == 0);
    CHECK(asm_[44] == 0xFF && asm_[48] == 0xFF);     /* padding */

    /* single-frame path: PDU2 pgn and PDU1 pgn (PS forced to 0xFF) */
    uint8_t small[8] = { 1, 2, 3 };
    CHECK(TcJ1939Bam(0xFF00, 0x80, 6, small, 3, 0, 0, buf, sizeof buf, &n) == TC_OK);
    CHECK(n == 24 && rd_u32(buf + 8) == (0x18FF0080u | TC_XNET_EXTENDED_ID_FLAG) && buf[15] == 3 && buf[18] == 3 && buf[19] == 0);
    CHECK(TcJ1939Bam(0xEA00, 0x80, 6, small, 3, 0, 0, buf, sizeof buf, &n) == TC_OK);
    CHECK(rd_u32(buf + 8) == (0x18EAFF80u | TC_XNET_EXTENDED_ID_FLAG));

    /* 9 bytes -> CM + 2 DT, last padded */
    uint8_t nine[9] = { 1,2,3,4,5,6,7,8,9 };
    CHECK(TcJ1939Bam(0xFF10, 0x11, 6, nine, 9, 0, 0, buf, sizeof buf, &n) == TC_OK);
    CHECK(n == 72 && buf[19] == 2 && buf[24 + 16] == 1 && buf[48 + 16] == 2 && buf[48 + 17] == 8 && buf[48 + 18] == 9 && buf[48 + 19] == 0xFF);

    static uint8_t big[1786];
    static uint8_t bigout[256 * TC_RAW_FRAME_SIZE];
    CHECK(TcJ1939Bam(0xFF00, 1, 6, big, 1785, 0, 0, bigout, sizeof bigout, &n) == TC_OK && n == 256 * 24);
    CHECK(bigout[19] == 255);
    CHECK(TcJ1939Bam(0xFF00, 1, 6, big, 1786, 0, 0, bigout, sizeof bigout, &n) == TC_ERR_PAYLOAD);

    uint8_t hdr[12];
    CHECK(TcNclHeader(hdr, 12) == TC_OK);
    CHECK(hdr[0] == 0x4E && hdr[1] == 0x49 && hdr[3] == 0x03 && hdr[8] == 1);
    CHECK(TcNclHeader(hdr, 11) == TC_ERR_ARG);
    CHECK(TcVersion() == 0x010000);
}

/* ----------------------------------------------------------------------- */
static void test_canpack(void)
{
    /* frame 0: std id 0x123 dlc 8; frame 1: ext id 0x18FF1234 dlc 8 */
    double frames[2 * TC_FRAMEDEF_COLS] = { 0x123, 0, 8, 0,   0x18FF1234, 1, 8, 0 };
    /*  frame start len order type factor offset min max */
    double sigs[][TC_SIGDEF_COLS] = {
        { 0,  0, 16, 0, 0, 0.1, -40, 0, 0 },    /* Intel u16, temp 25.3 -> 653 = 0x028D */
        { 0, 16,  8, 0, 1, 1,     0, 0, 0 },    /* Intel s8, -5 -> 0xFB */
        { 0, 31, 16, 1, 0, 1,     0, 0, 0 },    /* Motorola u16 at start 31: bytes 3,4 -> 0x1234 */
        { 0, 45,  3, 1, 0, 1,     0, 0, 7 },    /* Motorola 3-bit at start 45 (byte5 bits5..3), value 9 clamps to 7 */
        { 0,  4,  4, 0, 0, 1,     0, 0, 0 },    /* Intel 4-bit in byte 0 high nibble? no: bits 4..7 of byte0 collide w/ sig0 -> use frame 1 */
        { 1,  0, 32, 0, 2, 1,     0, 0, 0 },    /* float32 LE */
        { 1, 32, 12, 0, 0, 0.5,   0, 0, 0 },    /* Intel u12 starting byte 4: 100.0 -> 200 */
        { 1, 44,  1, 0, 0, 1,     0, 0, 0 },    /* bit flag right after it */
        { 1, 63, 16, 1, 1, 1,     0, 0, 0 },    /* Motorola s16 at start 63 spills into byte 8 -> error (tested first) */
    };
    float vals[] = { 25.3f, -5, 0x1234, 9, 0, 1.5f, 100, 1, -2 };
    uint8_t buf[2 * TC_RAW_FRAME_SIZE]; int32_t n;

    /* first: the overflowing Motorola signal must be rejected */
    CHECK(TcCanPack(&sigs[0][0], 9, frames, 2, vals, 9, 0, buf, sizeof buf, &n) == TC_ERR_SIGDEF);
    /* fix it: Motorola s8 in byte 7 */
    sigs[8][2] = 8;
    /* drop the colliding sig 4 by making it a harmless zero elsewhere */
    sigs[4][0] = 1; sigs[4][1] = 48; sigs[4][2] = 4; vals[4] = 0xA;

    CHECK(TcCanPack(&sigs[0][0], 9, frames, 2, vals, 9, 42, buf, sizeof buf, &n) == TC_OK);
    CHECK(n == 48);
    const uint8_t* f0 = buf, *f1 = buf + 24;
    CHECK(rd_u64(f0) == 42 && rd_u32(f0 + 8) == 0x123 && f0[15] == 8);
    CHECK(rd_u32(f1 + 8) == (0x18FF1234u | TC_XNET_EXTENDED_ID_FLAG));
    const uint8_t* d0 = f0 + 16, *d1 = f1 + 16;
    CHECK(d0[0] == 0x8D && d0[1] == 0x02);
    CHECK(d0[2] == 0xFB);
    CHECK(d0[3] == 0x12 && d0[4] == 0x34);
    CHECK(d0[5] == (7 << 3));
    float fl; memcpy(&fl, d1, 4); CHECK(fl == 1.5f);
    CHECK(d1[4] == 200 && (d1[5] & 0x0F) == 0);
    CHECK((d1[5] & 0x10) == 0x10);                  /* flag at bit 44 */
    CHECK((d1[6] & 0x0F) == 0xA);                   /* nibble at 48 */
    CHECK((d1[6] >> 4) == 0);
    CHECK(d1[7] == 0xFE);                           /* Motorola s8 -2 */

    /* buffer / arg errors */
    CHECK(TcCanPack(&sigs[0][0], 9, frames, 2, vals, 9, 0, buf, 10, &n) == TC_ERR_BUFFER && n == 48);
    frames[2] = 9;
    CHECK(TcCanPack(&sigs[0][0], 9, frames, 2, vals, 9, 0, buf, sizeof buf, &n) == TC_ERR_FRAMEDEF);
    frames[2] = 8;
    double bad[TC_SIGDEF_COLS] = { 0, 0, 16, 0, 0, 0.0, 0, 0, 0 };   /* factor 0 */
    CHECK(TcCanPack(bad, 1, frames, 2, vals, 1, 0, buf, sizeof buf, &n) == TC_ERR_SIGDEF);

    /* saturation: u8 with value 300 -> 255; s8 with -200 -> -128 */
    double sat[2][TC_SIGDEF_COLS] = { { 0, 0, 8, 0, 0, 1, 0, 0, 0 }, { 0, 8, 8, 0, 1, 1, 0, 0, 0 } };
    float sv[2] = { 300, -200 };
    CHECK(TcCanPack(&sat[0][0], 2, frames, 1, sv, 2, 0, buf, sizeof buf, &n) == TC_OK);
    CHECK(buf[16] == 255 && buf[17] == 0x80);
}

int main(void)
{
    test_controller_basic();
    test_deadband_countdown_resets();
    test_fault_latch_and_reset();
    test_fault_while_heating();
    test_bad_reading();
    test_init_state_and_live_setpoint();
    test_zero_timeouts_and_wrap();
    test_errors_and_aliasing();
    test_bam();
    test_canpack();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
