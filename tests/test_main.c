/*
 * test_main.c - TempCtl v3.0.0 unit tests, compiled together with
 * ../src/tempctl.c (no DLL needed).
 *
 * One function per rule group of TEMPCTL-SPEC-v3.0.0 (R1..R9), plus the
 * change list rows C1..C22 and the required scenarios S1..S15 of the
 * v3.0.0 handoff, named in the comments. Tick period is 100 ms; a countdown
 * observed on tick k expires on tick k + timeout/100.
 */
#include "../src/tempctl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKF(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-6)
#define ISNAN(v) ((v) != (v))

/* ----------------------------------------------------------------------- */
/* harness                                                                 */
/* ----------------------------------------------------------------------- */
#define P 100u
static double   S[TC_SETUP_COUNT];
static double   DG[TC_DIAG_COUNT];
static int32_t  DH, DC, ST, WN, RC;
static uint32_t NOW;

/* Enabled, degC, setpoint 50, band 45..55, limits 0..100, ErrorTimeout 1000,
   DeadbandTimeout 500, AtSetPtTimeout 300, single sensor, filter 4, no feedback. */
static void base(void)
{
    memset(S, 0, sizeof S);
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 1;  S[TC_SETUP_TEMP_UNITS] = 1;
    S[TC_SETUP_SETPOINT] = 50;         S[TC_SETUP_DEADBAND_HI] = 5;  S[TC_SETUP_DEADBAND_LO] = 5;
    S[TC_SETUP_HI_LIMIT] = 100;        S[TC_SETUP_LO_LIMIT] = 0;
    S[TC_SETUP_ERROR_TIMEOUT] = 1000;  S[TC_SETUP_DEADBAND_TIMEOUT] = 500; S[TC_SETUP_AT_SETPT_TIMEOUT] = 300;
    S[TC_SETUP_TEMP2_ENABLE] = 0;      S[TC_SETUP_TEMP2_OFFSET] = 0;   S[TC_SETUP_TEMP2_TOLERANCE] = 3;
    S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 2000;
    S[TC_SETUP_FILTER_POINTS] = 4;
    S[TC_SETUP_FEEDBACK_ENABLE] = 0;   S[TC_SETUP_RELAY_FEEDBACK_TIMEOUT] = 400;
    DH = DC = 0;
    /* A re-Init on a running zone keeps its relays (R9.3); every test starts from a reset zone 0. */
    TcReset(0, NOW, &ST, &WN);
}
static void two_sensor(void) { S[TC_SETUP_TEMP2_ENABLE] = 1; }
static void feedback(void)   { S[TC_SETUP_FEEDBACK_ENABLE] = 1; }

static int32_t initz(int z) { RC = TcInit(z, NOW, S, TC_SETUP_COUNT, &ST, &WN); return RC; }
static int32_t init0(void)  { return initz(0); }
static int32_t rstz(int z)  { RC = TcReset(z, NOW, &ST, &WN); return RC; }
static int32_t rst0(void)   { return rstz(0); }

/* One tick on zone z with explicit feedback. */
static int32_t tkz(int z, double t1, double t2, int hfb, int cfb)
{
    RC = TcCheckTemp(z, NOW, t1, t2, hfb, cfb, &DH, &DC, &ST, &WN);
    return RC;
}
/* Advance time one period, honest relays (feedback echoes the previous command). */
static int32_t tk2(double t1, double t2) { NOW += P; return tkz(0, t1, t2, DH, DC); }
static int32_t tk(double t1)             { return tk2(t1, t1); }
static void tkn(int n, double t1)        { while (n--) tk(t1); }
static void tk2n(int n, double t1, double t2) { while (n--) tk2(t1, t2); }
/* Explicit feedback tick on zone 0. */
static int32_t tkf(double t1, int hfb, int cfb) { NOW += P; return tkz(0, t1, t1, hfb, cfb); }

static double dgz(int z, int i) { TcGetDiag(z, DG, TC_DIAG_COUNT); return DG[i]; }
static double dg(int i)         { return dgz(0, i); }

/* A single-sensor zone heated from `from` until the heater is on (tick 6). */
static void heat_on(double from)
{
    base(); init0();
    tkn(6, from);
    CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
}

/* ----------------------------------------------------------------------- */
/* C1 API surface, sizes, argument checks                                   */
/* ----------------------------------------------------------------------- */
static void test_api(void)
{
    double d[TC_DIAG_COUNT + 5];
    int32_t a = 7, b = 7, c = 7, e = 7;
    CHECK(TcVersion() == 0x030000);
    CHECK(TcVersion() == ((TC_VERSION_MAJOR << 16) | (TC_VERSION_MINOR << 8) | TC_VERSION_PATCH));
    CHECK(TcSetupCount() == 17 && TcSetupCount() == TC_SETUP_COUNT);
    CHECK(TcDiagCount() == 25 && TcDiagCount() == TC_DIAG_COUNT);

    base(); NOW = 1000;
    CHECK(TcInit(16, NOW, S, TC_SETUP_COUNT, &a, &b) == TC_ERR_ZONE);
    CHECK(TcInit(-1, NOW, S, TC_SETUP_COUNT, &a, &b) == TC_ERR_ZONE);
    CHECK(TcInit(0, NOW, 0, TC_SETUP_COUNT, &a, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, 16, &a, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, 18, &a, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, TC_SETUP_COUNT, 0, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, TC_SETUP_COUNT, &a, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7);                               /* nothing written on a negative return */

    CHECK(TcCheckTemp(16, NOW, 50, 50, 0, 0, &a, &b, &c, &e) == TC_ERR_ZONE);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, 0, &b, &c, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, &a, 0, &c, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, &a, &b, 0, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, &a, &b, &c, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7 && c == 7 && e == 7);

    CHECK(TcReset(99, NOW, &a, &b) == TC_ERR_ZONE);
    CHECK(TcReset(0, NOW, 0, &b) == TC_ERR_ARG);
    CHECK(TcReset(0, NOW, &a, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7);

    CHECK(TcGetDiag(16, d, TC_DIAG_COUNT) == TC_ERR_ZONE);
    CHECK(TcGetDiag(0, 0, TC_DIAG_COUNT) == TC_ERR_ARG);
    CHECK(TcGetDiag(0, d, TC_DIAG_COUNT - 1) == TC_ERR_ARG);
    CHECK(TcGetDiag(0, d, TC_DIAG_COUNT + 5) == TC_OK);   /* longer buffers are fine */
}

/* ----------------------------------------------------------------------- */
/* R2 enable / lifecycle  (C3, S12)                                        */
/* ----------------------------------------------------------------------- */
static void test_R2_enable(void)
{
    int i;
    /* an uninitialised zone is inert and returns TC_OK, status 0 */
    NOW = 5000; DH = DC = 5; ST = WN = 5;
    CHECK(tkz(9, 200, NAN, 1, 1) == TC_OK);
    CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE && DH == 0 && DC == 0);
    CHECK(rstz(9) == TC_OK && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    CHECK(dgz(9, TC_DIAG_ZONE_INITIALIZED) == 0);
    CHECK(ISNAN(dgz(9, TC_DIAG_CONTROL_TEMP)) && ISNAN(dgz(9, TC_DIAG_TEMP1_AVG)) && ISNAN(dgz(9, TC_DIAG_HI_BAND)));
    CHECK(dgz(9, TC_DIAG_ACTIVE_SENSOR) == 1 && dgz(9, TC_DIAG_STATUS_MIRROR) == 0);

    /* Enable = 0: no action under any stimulus */
    base(); two_sensor(); feedback(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0;
    CHECK(init0() == TC_OK && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    for (i = 0; i < 30; i++) {
        double t = (i % 3 == 0) ? 200.0 : (i % 3 == 1) ? NAN : 10.0;
        NOW += P; tkz(0, t, t, 1, 1);
        CHECK(RC == TC_OK && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE && DH == 0 && DC == 0);
    }
    CHECK(dg(TC_DIAG_ZONE_INITIALIZED) == 1);
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)) && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    CHECK(dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    /* enabling by a new Init starts control */
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 1; S[TC_SETUP_TEMP2_ENABLE] = 0; S[TC_SETUP_FEEDBACK_ENABLE] = 0;
    init0(); tk(10);
    CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);
    /* the boolean rule: 0.1 is off, 0.11 is on, NaN handled by the config check (R9 tests) */
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 0.1;  init0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED);
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 0.11; init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    /* R2.3 power-on: a zone nobody initialised is disabled */
    CHECK(dgz(15, TC_DIAG_ZONE_INITIALIZED) == 0);
    tkz(15, 30, 30, 0, 0); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && DH == 0);
}

/* ----------------------------------------------------------------------- */
/* R3 units are a label  (C4)                                              */
/* ----------------------------------------------------------------------- */
static void test_R3_units(void)
{
    static const double prof[] = { 30, 30, 30, 30, 30, 30, 35, 40, 45, 50, 50, 50, 50, 52, 52, 56, 56, 56, 56, 56, 56, 56, 50, 50, 50, 50 };
    int32_t log0[26][4], log1[26][4];
    int i;
    base(); S[TC_SETUP_TEMP_UNITS] = 0; NOW = 100; initz(0);
    for (i = 0; i < 26; i++) { NOW += P; tkz(0, prof[i], prof[i], DH, DC); log0[i][0] = DH; log0[i][1] = DC; log0[i][2] = ST; log0[i][3] = WN; }
    base(); S[TC_SETUP_TEMP_UNITS] = 1; NOW = 100; rstz(1); initz(1); DH = DC = 0;
    for (i = 0; i < 26; i++) { NOW += P; tkz(1, prof[i], prof[i], DH, DC); log1[i][0] = DH; log1[i][1] = DC; log1[i][2] = ST; log1[i][3] = WN; }
    CHECK(memcmp(log0, log1, sizeof log0) == 0);
    CHECK(log0[5][0] == 1 && log0[21][1] == 1);            /* the profile really exercised both relays */
    base(); S[TC_SETUP_TEMP_UNITS] = 2;   init0(); CHECK(ST == TC_ST_CONFIG_FAULT);
    base(); S[TC_SETUP_TEMP_UNITS] = 0.5; init0(); CHECK(ST == TC_ST_CONFIG_FAULT);
    base(); S[TC_SETUP_TEMP_UNITS] = NAN; init0(); CHECK(ST == TC_ST_CONFIG_FAULT);
}

/* ----------------------------------------------------------------------- */
/* R4 filter length, raw control, averages  (C9)                           */
/* ----------------------------------------------------------------------- */
static void test_R4_filter(void)
{
    static const double bad[] = { 0, -1, 65, 100, NAN, INFINITY, -INFINITY, 0.99 };
    int i;
    for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        base(); S[TC_SETUP_FILTER_POINTS] = bad[i]; init0();
        CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);          /* never a fault or warning */
        CHECK(dg(TC_DIAG_APPLIED_FILTER_POINTS) == TC_DEFAULT_FILTER);
    }
    base(); S[TC_SETUP_FILTER_POINTS] = 64;  init0(); CHECK(dg(TC_DIAG_APPLIED_FILTER_POINTS) == 64);
    base(); S[TC_SETUP_FILTER_POINTS] = 2.9; init0(); CHECK(dg(TC_DIAG_APPLIED_FILTER_POINTS) == 2);
    base(); S[TC_SETUP_FILTER_POINTS] = 1;   init0(); CHECK(dg(TC_DIAG_APPLIED_FILTER_POINTS) == 1);
    base(); S[TC_SETUP_FILTER_POINTS] = 64.9; init0(); CHECK(dg(TC_DIAG_APPLIED_FILTER_POINTS) == 64);

    /* R4.1 / C9: with FilterPoints = 64 control reacts on the raw value at once */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 64; init0();
    tk(30);
    CHECK(ST == TC_ST_HEAT_PENDING);
    tkn(5, 30);
    CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    CHECKF(dg(TC_DIAG_TEMP1_AVG), 30);                                  /* 6 samples, average of raw */
    CHECKF(dg(TC_DIAG_CONTROL_TEMP), 30);
    tk(52); CHECKF(dg(TC_DIAG_CONTROL_TEMP), 52);                        /* raw, not the average (33.1) */
    CHECKF(dg(TC_DIAG_TEMP1_AVG), (30.0 * 6 + 52) / 7);

    /* moving-average arithmetic with FilterPoints = 2 */
    base(); S[TC_SETUP_FILTER_POINTS] = 2; init0();
    tk(40); CHECKF(dg(TC_DIAG_TEMP1_AVG), 40);
    tk(50); CHECKF(dg(TC_DIAG_TEMP1_AVG), 45);
    tk(60); CHECKF(dg(TC_DIAG_TEMP1_AVG), 55);
    tk(200); CHECKF(dg(TC_DIAG_TEMP1_AVG), 55);                          /* R4.3: out-of-range sample not averaged */
    tk(NAN); CHECKF(dg(TC_DIAG_TEMP1_AVG), 55);
    /* R4.5: Init and Reset clear the averages */
    init0(); CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)));
    tk(50); CHECKF(dg(TC_DIAG_TEMP1_AVG), 50);
    rst0(); CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)));
    /* sensor 2 average uses the corrected value */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 5; S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2(50, 40); CHECKF(dg(TC_DIAG_TEMP2_AVG), 45); CHECKF(dg(TC_DIAG_TEMP2_CORRECTED), 45); CHECKF(dg(TC_DIAG_TEMP2_RAW), 40);
    /* Temp2 disabled: NaN in every sensor-2 field */
    base(); init0(); tk2(50, 40);
    CHECK(ISNAN(dg(TC_DIAG_TEMP2_RAW)) && ISNAN(dg(TC_DIAG_TEMP2_CORRECTED)) && ISNAN(dg(TC_DIAG_TEMP2_AVG)));
}

/* ----------------------------------------------------------------------- */
/* R4.4 NaN / Inf handling and chatter immunity  (C10, S4)                 */
/* ----------------------------------------------------------------------- */
static void test_R4_nan_and_chatter(void)
{
    int i;
    /* idle in band: a single NaN changes nothing but the warning */
    base(); init0(); tkn(3, 50);
    CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    tk(NAN);
    CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0 && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100 && ISNAN(dg(TC_DIAG_CONTROL_TEMP)) && ISNAN(dg(TC_DIAG_TEMP1_RAW)));
    CHECKF(dg(TC_DIAG_TEMP1_AVG), 50);                                   /* average not corrupted */
    tk(50);
    CHECK(WN == TC_WN_NONE && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50);   /* draining at half rate */

    /* heating: single NaN, single spike high, single spike into the band */
    heat_on(30);
    tk(NAN);  CHECK(DH == 1 && ST == TC_ST_HEATER_ON && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    tk(30);   CHECK(DH == 1 && WN == TC_WN_NONE);
    tk(200);  CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    tk(30);   CHECK(DH == 1);
    tk(52);   CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);   /* observed, not acted on */
    tk(30);   CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(-INFINITY); CHECK(DH == 1);
    tk(INFINITY);  CHECK(DH == 1);

    /* pending: single spike back into the band does not engage anything */
    base(); init0(); tk(50);
    tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);
    tk(50);        CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0);
    tk(30);        CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);

    /* cooling: single NaN holds the cooler */
    base(); init0(); tkn(6, 70); CHECK(DC == 1);
    tk(NAN); CHECK(DC == 1 && DH == 0 && ST == TC_ST_COOLER_ON);

    /* a NaN stream fails sensor 1 high after ErrorTimeout (single-sensor mode -> fault) */
    base(); init0(); tk(50);
    for (i = 1; i <= 9; i++) { tk(NAN); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_TEMP1_OUT_OF_RANGE); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100.0 * i); }
    tk(NAN);
    CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && DH == 0 && DC == 0 && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    base(); init0(); tk(50); tkn(10, INFINITY);  CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    base(); init0(); tk(50); tkn(10, -INFINITY); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);   /* Inf of either sign = high */
    base(); init0(); tk(50); tkn(10, 150);       CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    base(); init0(); tk(50); tkn(10, -5);        CHECK(ST == TC_ST_TEMP1_FAIL_LOW);
    /* NaN on temp2 while it is disabled is ignored entirely */
    base(); init0(); tk2n(20, 50, NAN); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
}

/* ----------------------------------------------------------------------- */
/* R5.4 leaky accumulator, R5.6 hourly events  (C11, S5)                   */
/* ----------------------------------------------------------------------- */
static void test_R5_accumulator(void)
{
    int i;
    /* R5.4 (Amendment A): out of range charges elapsed, in range drains half of it (DRAIN 0.5).
       1-tick glitch: charged 100 ms on the tick it is seen, drained 50 + 50 on the next two, one event */
    base(); init0(); tk(50);
    tk(200); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100 && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    tk(50);  CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50 && WN == TC_WN_NONE);
    tk(50);  CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    tkn(10, 50); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    /* sparse glitches never fail; each one is an event */
    for (i = 0; i < 20; i++) { tk(NAN); tkn(4, 50); }
    CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 21 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    /* a sustained excursion is one event, however long; it drains at half rate */
    tkn(5, 200); tk(50);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 22 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 450);
    tkn(8, 50); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50);
    tk(50);     CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);

    /* Amendment A table (E = ErrorTimeout = 1000): 33 % duty (1 out, 2 in) nets zero -> never fails */
    base(); init0(); tk(50);
    for (i = 0; i < 40; i++) {
        tk(200); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100);
        tk(50);  CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50);
        tk(50);  CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    }
    CHECK(ST == TC_ST_TEMP_AT_SETPT);
    /* 25 % duty (1 out, 3 in): never fails */
    base(); init0(); tk(50);
    for (i = 0; i < 30; i++) { tk(200); tkn(3, 50); }
    CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    /* 50 % duty (1 out, 1 in) gains 50 ms per 200 ms: fails on tick 37 = 3.7 s (~4 x E) */
    base(); init0(); tk(50);
    for (i = 1; i <= 18; i++) {
        tk(200); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50.0 * (i - 1) + 100);
        tk(50);  CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50.0 * i);
    }
    tk(200); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000);
    /* 75 % duty (3 out, 1 in) gains 250 ms per 400 ms: fails on tick 15 = 1.5 s (~1.6 x E) */
    base(); init0(); tk(50);
    for (i = 0; i < 3; i++) { tkn(3, 200); tk(50); CHECK(ST == TC_ST_TEMP_AT_SETPT); }
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 750);
    tk(200); tk(200); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 950);
    tk(200); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1050);
    /* 100 % duty fails at exactly 1 x E */
    base(); init0(); tk(50);
    tkn(9, 200); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 900);
    tk(200);     CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000);
    /* an odd loop period keeps the half-ms accumulator exact: charge 101, drain 50.5 */
    base(); init0(); tk(50);
    NOW += 101; tkz(0, 200, 200, DH, DC); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 101);
    NOW += 101; tkz(0, 50, 50, DH, DC);   CHECKF(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS), 50.5);
    NOW += 101; tkz(0, 50, 50, DH, DC);   CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    /* ErrorTimeout below one loop period fails a sensor from one sample (the host must keep it >= 2 periods) */
    base(); S[TC_SETUP_ERROR_TIMEOUT] = 100; init0(); tk(50);
    tk(200); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    /* the direction at the moment of failure decides high/low */
    base(); init0(); tk(50);
    tkn(9, -5); tk(200); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    base(); init0(); tk(50);
    tkn(9, 200); tk(-5); CHECK(ST == TC_ST_TEMP1_FAIL_LOW);

    /* R5.6 ring buffer: events expire after 60 minutes, partial expiry works */
    base(); init0(); tk(50);
    tk(200); tk(50);                                                     /* event A at t0 */
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    NOW += 30u * 60000u; tk(50);                                          /* +30 min in range */
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    tk(200); tk(50);                                                     /* event B at t0 + 30 min */
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 2);
    NOW += 31u * 60000u; tk(50);                                          /* +31 min: A expired, B not */
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    NOW += 61u * 60000u; tk(50);                                          /* > 60 min: everything expired */
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    CHECK(ST == TC_ST_TEMP_AT_SETPT);                                    /* long in-range gaps are harmless */
    /* Init and Reset clear the counts */
    tk(200); tk(50); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    rst0(); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    tk(200); tk(50); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    init0(); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    /* sensor 2 has its own accumulator and counter */
    base(); two_sensor(); init0(); tk2(50, 50);
    tk2(50, 200); tk2(50, 200); tk2(50, 50);
    CHECK(dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    CHECK(dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 150 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);   /* 200 charged, 50 drained */
}

/* ----------------------------------------------------------------------- */
/* R5.5 failure effects, failover  (C12, S7)                               */
/* ----------------------------------------------------------------------- */
static void test_R5_failover(void)
{
    int i;
    /* sensor 1 fails with sensor 2 healthy: no fault, control moves to sensor 2 (offset applied) */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 5; init0();
    tk2n(3, 50, 45);
    CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE && dg(TC_DIAG_ACTIVE_SENSOR) == 1);
    for (i = 0; i < 9; i++) { tk2(NAN, 45); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_TEMP1_OUT_OF_RANGE); }
    tk2(NAN, 45);                                                        /* 1000 ms: sensor 1 fails */
    CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0);
    CHECK(WN == TC_WN_TEMP1_OUT_OF_RANGE);                               /* code 1 masks code 6 while sensor 1 is out of range */
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000);
    CHECKF(dg(TC_DIAG_CONTROL_TEMP), 50);                                /* corrected sensor 2 */
    tk2(50, 45);                                                         /* sensor 1 returns: stays failed, warning 6 */
    CHECK(WN == TC_WN_RUNNING_ON_TEMP2 && dg(TC_DIAG_ACTIVE_SENSOR) == 2 && ST == TC_ST_TEMP_AT_SETPT);
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000);                       /* frozen at failure */
    /* control now follows sensor 2: corrected 40 < LoBand 45 -> heat */
    tk2(50, 35); CHECK(ST == TC_ST_HEAT_PENDING && WN == TC_WN_RUNNING_ON_TEMP2);
    tk2n(5, 50, 35); CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    CHECKF(dg(TC_DIAG_CONTROL_TEMP), 40);
    /* sensor 1 excursions no longer affect control (not active, already failed) */
    tk2(200, 35); CHECK(DH == 1 && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    tk2(50, 35);  CHECK(DH == 1 && WN == TC_WN_RUNNING_ON_TEMP2);
    /* then sensor 2 fails too -> BothSensorsFailed, relays off */
    for (i = 0; i < 9; i++) { tk2(50, NAN); CHECK(DH == 1 && ST == TC_ST_HEATER_ON && WN == TC_WN_TEMP2_OUT_OF_RANGE); }
    tk2(50, NAN);
    CHECK(ST == TC_ST_BOTH_SENSORS_FAILED && DH == 0 && DC == 0 && WN == TC_WN_TEMP2_OUT_OF_RANGE);
    tk2(50, 45); CHECK(ST == TC_ST_BOTH_SENSORS_FAILED && WN == TC_WN_TEMP2_OUT_OF_RANGE);   /* latched, warning frozen */

    /* sensor 2 fails first while sensor 1 is healthy: no fault, no switch, warning only while out of range */
    base(); two_sensor(); init0(); tk2(50, 50);
    tk2n(10, 50, 200);
    CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_TEMP2_OUT_OF_RANGE && dg(TC_DIAG_ACTIVE_SENSOR) == 1);
    CHECK(dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 1000);
    tk2(50, 50); CHECK(WN == TC_WN_NONE && ST == TC_ST_TEMP_AT_SETPT);
    tk2n(20, 50, 50); CHECK(dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 1000);    /* failed: never re-evaluated, never drains */
    /* now sensor 1 fails -> both */
    tk2n(10, NAN, 50); CHECK(ST == TC_ST_BOTH_SENSORS_FAILED);
    /* both fail on the same tick */
    base(); two_sensor(); init0(); tk2(50, 50);
    tk2n(10, NAN, NAN); CHECK(ST == TC_ST_BOTH_SENSORS_FAILED);
    /* single-sensor mode: sensor 1 failure is a fault (10 / 11) */
    base(); init0(); tk(50); tkn(10, 150); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    base(); init0(); tk(50); tkn(10, -1);  CHECK(ST == TC_ST_TEMP1_FAIL_LOW);
    /* R9.7: failed sensors are never re-admitted; Init restores sensor 1 */
    base(); two_sensor(); init0(); tk2(50, 50); tk2n(10, NAN, 50); tk2n(50, 50, 50);
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && WN == TC_WN_RUNNING_ON_TEMP2);
    init0(); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 1 && WN == TC_WN_NONE);
    tk2n(10, NAN, 50); tk2(50, 50); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2);
    rst0(); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 1 && WN == TC_WN_NONE);
}

/* ----------------------------------------------------------------------- */
/* R5.7 control pause while the active sensor is out of range  (C13, S6)   */
/* ----------------------------------------------------------------------- */
static void test_R5_pause(void)
{
    /* heating, at-setpoint countdown running, then an excursion: relay holds, countdown frozen */
    heat_on(30);
    tk(52); tk(52);
    CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    tkn(3, 200);
    CHECK(DH == 1 && ST == TC_ST_HEATER_ON && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 300);
    tk(52); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 100);   /* resumes where it stopped */
    tk(52); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
    /* the same with NaN */
    heat_on(30);
    tk(52); tk(52); tkn(4, NAN); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200 && ISNAN(dg(TC_DIAG_CONTROL_TEMP)));
    tk(52); tk(52); CHECK(DH == 0);
    /* pending countdown freezes too */
    base(); init0(); tk(50);
    tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    tk(NAN); tk(NAN); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400 && DH == 0);
    tk(30); tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 100 && DH == 0);
    tk(30); CHECK(DH == 1);
    /* idle in band, then an excursion: status holds, nothing engages */
    base(); init0(); tk(50); tkn(5, 200); CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0);
    /* an excursion on the non-active sensor does not touch control */
    base(); two_sensor(); init0(); tk2(30, 30);
    tk2n(5, 30, 200); CHECK(DH == 1 && ST == TC_ST_HEATER_ON && WN == TC_WN_TEMP2_OUT_OF_RANGE);
    tk2(52, 200); tk2(52, 200); tk2(52, 200); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 100);
    tk2(52, 200); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
    /* pause ends in failure: single sensor -> fault while the relay was held */
    heat_on(30);
    tkn(9, 200); CHECK(DH == 1);
    tk(200); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && DH == 0);
    /* pause ends in failover: relay kept on, control continues on sensor 2 */
    base(); two_sensor(); init0(); tk2(30, 30); tkn(5, 30); CHECK(DH == 1);
    tk2n(10, NAN, 30); CHECK(DH == 1 && ST == TC_ST_HEATER_ON && dg(TC_DIAG_ACTIVE_SENSOR) == 2);
    tk2(NAN, 52); tk2(NAN, 52); tk2(NAN, 52); tk2(NAN, 52); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
}

/* ----------------------------------------------------------------------- */
/* R6.1 offset  (C5)                                                       */
/* ----------------------------------------------------------------------- */
static void test_R6_offset(void)
{
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 5; S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2(50, 95); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_TEMP2_CORRECTED) == 100);   /* 100 is not > HiLimit */
    tk2(50, 96); CHECK(WN == TC_WN_TEMP2_OUT_OF_RANGE && dg(TC_DIAG_TEMP2_CORRECTED) == 101);
    tk2(50, -4); CHECK(WN == TC_WN_NONE);                                /* corrected 1 >= LoLimit 0 */
    tk2(50, -6); CHECK(WN == TC_WN_TEMP2_OUT_OF_RANGE);
    /* comparison uses the corrected value: raw 50/50 disagree by the offset */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 5; S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2n(4, 50, 45); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    tk2(50, 50); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000);          /* |50 - 55| > 3 observed */
    tk2(50, 50); tk2(50, 50); CHECK(WN == TC_WN_TEMP_DISAGREE);
    /* negative offset */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = -10; S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2(50, 60); CHECKF(dg(TC_DIAG_TEMP2_CORRECTED), 50); CHECKF(dg(TC_DIAG_TEMP2_AVG), 50);
    /* post-switch control uses the corrected value (C5: offset 5, temp2 95 -> 100) */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 5; init0();
    tk2n(11, NAN, 45); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2);
    tk2n(6, NAN, 95);                                                    /* corrected 100: in range, above HiBand */
    CHECK(DC == 1 && ST == TC_ST_COOLER_ON && dg(TC_DIAG_CONTROL_TEMP) == 100);
    tk2n(6, 50, 96); CHECK(DC == 1 && WN == TC_WN_TEMP2_OUT_OF_RANGE);    /* corrected 101: out of range, paused */
}

/* ----------------------------------------------------------------------- */
/* R6.3-R6.6 two-stage disagreement  (C6, C7, S8)                          */
/* ----------------------------------------------------------------------- */
static void test_R6_disagreement(void)
{
    int i;
    /* in band, flag set on tick 1, averages full on tick 4 */
    base(); two_sensor(); init0();
    tk2n(4, 50, 50);
    CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 1 && WN == TC_WN_NONE);
    tk2(50, 60); CHECKF(dg(TC_DIAG_TEMP2_AVG), 52.5); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    tk2(50, 60); CHECKF(dg(TC_DIAG_TEMP2_AVG), 55);   CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000);   /* observed */
    tk2(50, 60); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 1900);
    tk2(50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 1800);   /* 200 ms = T/10 */
    for (i = 0; i < 17; i++) { tk2(50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE && ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0); }
    CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 100);
    tk2(50, 60);
    CHECK(ST == TC_ST_TEMP_DISAGREE_FAULT && WN == TC_WN_TEMP_DISAGREE && DH == 0 && DC == 0);
    tk2(50, 50); CHECK(ST == TC_ST_TEMP_DISAGREE_FAULT && WN == TC_WN_TEMP_DISAGREE);   /* latched, frozen */

    /* recovery between the stages clears the warning at once and restarts the countdown */
    base(); two_sensor(); init0(); tk2n(4, 50, 50);
    tk2n(6, 50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE);                   /* avg 60 by now */
    tk2(50, 50); tk2(50, 50); CHECK(WN == TC_WN_TEMP_DISAGREE);         /* avg 55: still disagree */
    tk2(50, 50); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);   /* avg 52.5: agree */
    tk2n(3, 50, 60); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 1900);   /* restarted from zero */
    tk2(50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE);
    /* disagreement in the other direction */
    base(); two_sensor(); init0(); tk2n(4, 50, 50);
    tk2n(4, 50, 40); CHECK(WN == TC_WN_TEMP_DISAGREE);
    /* exactly the tolerance is not a disagreement */
    base(); two_sensor(); init0(); tk2n(4, 50, 50);
    tk2n(20, 50, 53); CHECK(WN == TC_WN_NONE);
    tk2n(6, 50, 53.001); CHECK(WN == TC_WN_TEMP_DISAGREE);              /* average crosses on the 4th, warning 200 ms later */
    /* T/10 == 0: the warning appears on the first qualifying tick; fault on the next */
    base(); two_sensor(); S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 5; S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2(50, 50); tk2(50, 60);
    CHECK(WN == TC_WN_TEMP_DISAGREE && ST == TC_ST_TEMP_AT_SETPT);
    tk2(50, 60); CHECK(ST == TC_ST_TEMP_DISAGREE_FAULT);
    /* disagreement never changes control while it runs (heating continues) */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2n(4, 50, 50); tk2n(6, 30, 40); CHECK(DH == 1 && WN == TC_WN_TEMP_DISAGREE);
}

/* ----------------------------------------------------------------------- */
/* R6.2 / R6.5 comparison gating  (S9)                                     */
/* ----------------------------------------------------------------------- */
static void test_R6_gating(void)
{
    int i;
    /* not before Initial_HC_Flag: heat-up with a 10-degree disagreement produces no warning */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 1; init0();
    for (i = 0; i < 10; i++) { tk2(30, 40); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0); }
    CHECK(DH == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk2(52, 62); tk2(52, 62); tk2(52, 62);                               /* at-setpoint countdown */
    CHECK(DH == 1 && WN == TC_WN_NONE);
    tk2(52, 62);                                                         /* relay drops, flag set */
    CHECK(DH == 0 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1 && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0 && WN == TC_WN_NONE);
    tk2(52, 62); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000 && WN == TC_WN_NONE);   /* comparison observed on the next tick */
    tk2(52, 62); tk2(52, 62); CHECK(WN == TC_WN_TEMP_DISAGREE);
    /* not during a range excursion: countdown and warning restart from zero afterwards */
    tk2n(5, 52, 62); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 1300);
    tk2(52, 200); CHECK(WN == TC_WN_TEMP2_OUT_OF_RANGE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    tk2(52, 62); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000);
    tk2(52, 62); tk2(52, 62); CHECK(WN == TC_WN_TEMP_DISAGREE);
    tk2(NAN, 62); CHECK(WN == TC_WN_TEMP1_OUT_OF_RANGE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    /* not before both averages are full */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 8; init0();
    for (i = 0; i < 7; i++) { tk2(50, 60); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0); }
    tk2(50, 60); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000);          /* 8th sample: observed */
    tk2(50, 60); tk2(50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE);
    /* an out-of-range sample does not fill the average, so the gate waits for it */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 2; init0();
    tk2(50, 60); tk2(50, 200); tk2(50, 200); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    tk2(50, 60); CHECK(dg(TC_DIAG_COMPARE_REMAIN_MS) == 2000);
    /* not once a sensor has failed */
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2(50, 50); tk2n(10, 50, 200); tk2n(30, 50, 60);
    CHECK(WN == TC_WN_NONE && ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    /* not with Temp2Enable = 0 */
    base(); S[TC_SETUP_FILTER_POINTS] = 1; init0();
    tk2n(30, 50, 80); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
}

/* ----------------------------------------------------------------------- */
/* R7 deadband engage / at-setpoint release  (C8, S1, S3)                  */
/* ----------------------------------------------------------------------- */
static void test_R7_control(void)
{
    int i;
    /* S1: cold start -> HeatPending -> HeaterON -> at-setpoint -> TempAtSetPt */
    base(); NOW = 0; init0();
    CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    for (i = 4; i >= 1; i--) { tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 100.0 * i); }
    tk(30); CHECK(ST == TC_ST_HEATER_ON && DH == 1 && DC == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    tkn(5, 30); CHECK(DH == 1);
    tk(45); tk(49.9); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(50); CHECK(DH == 1 && ST == TC_ST_HEATER_ON && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);   /* C8: one sample at SP does not drop */
    tk(50); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    tk(50); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 100);
    tk(50); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
    tkn(10, 50); CHECK(ST == TC_ST_TEMP_AT_SETPT && DH == 0 && DC == 0);
    /* breaking the at-setpoint condition resets that countdown */
    heat_on(30);
    tk(50); tk(50); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    tk(49); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(50); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);
    tk(50); tk(50); CHECK(DH == 1); tk(50); CHECK(DH == 0);
    /* R7.1: re-entering the band clears the countdown; crossing to the other side restarts it */
    base(); init0(); tk(50);
    tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    tk(50); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    tk(30); tk(70); CHECK(ST == TC_ST_COOL_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500 && DH == 0 && DC == 0);
    tkn(5, 70); CHECK(DC == 1 && DH == 0 && ST == TC_ST_COOLER_ON);
    /* band edges are inclusive: exactly HiBand / LoBand is inside */
    base(); init0(); tk(50);
    tkn(10, 55); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tkn(10, 45); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tk(55.001); CHECK(ST == TC_ST_COOL_PENDING);
    tk(44.999); CHECK(ST == TC_ST_HEAT_PENDING);
    /* S3: cool-down; release at or below the setpoint; overshoot goes straight to HeatPending */
    base(); init0();
    tkn(6, 70); CHECK(DC == 1 && ST == TC_ST_COOLER_ON);
    tk(50.1); CHECK(DC == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(50); tk(50); tk(50); CHECK(DC == 1);
    tk(50); CHECK(DC == 0 && ST == TC_ST_TEMP_AT_SETPT);
    base(); init0(); tkn(6, 70);
    tkn(4, 40); CHECK(DC == 0 && ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);   /* dropped at 40: below LoBand */
    /* R7.3: never both; deadband countdown idle while a relay is on */
    heat_on(30);
    for (i = 0; i < 20; i++) { tk(i % 2 ? 30 : 70); CHECK(!(DH && DC) && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0); }
    CHECK(DH == 1);                                                      /* 70 is >= setpoint but not held 300 ms */
    tk(70); tk(70); tk(70); tk(70); CHECK(DH == 0 && ST == TC_ST_COOL_PENDING);   /* released above the band: cool pending */
    /* setpoint change by Init moves the bands (deadbands follow the setpoint) */
    base(); init0(); tk(50); S[TC_SETUP_SETPOINT] = 70; init0();
    CHECK(dg(TC_DIAG_HI_BAND) == 75 && dg(TC_DIAG_LO_BAND) == 65);
    tk(50); CHECK(ST == TC_ST_HEAT_PENDING);
}

/* ----------------------------------------------------------------------- */
/* R7.5 Initial_HC_Flag  (C14)                                             */
/* ----------------------------------------------------------------------- */
static void test_R7_initial_hc(void)
{
    base(); init0(); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk(50); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 1);                     /* in-band start: tick 1 */
    rst0(); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk(55); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 1);                     /* band edge counts as in band */
    init0(); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tkn(6, 30); CHECK(DH == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);      /* out-of-band start: not while heating */
    tk(47); tk(48); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 0);             /* in band but not idle */
    tk(50); tk(50); tk(50); CHECK(DH == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk(50); CHECK(DH == 0 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1);          /* set when the countdown completes */
    /* pending (idle, out of band) does not set it */
    init0(); tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk(50); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
    /* cooling completion sets it too */
    init0(); tkn(6, 70); tkn(4, 50); CHECK(DC == 0 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
    /* once set it stays set through later cycles until Init/Reset */
    tkn(6, 30); CHECK(DH == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
}

/* ----------------------------------------------------------------------- */
/* R8 relay feedback  (C15, S10)                                           */
/* ----------------------------------------------------------------------- */
static void test_R8_feedback(void)
{
    int i;
    /* honest relays through a whole heat cycle: never a warning */
    base(); feedback(); init0();
    tkn(6, 30); CHECK(DH == 1 && WN == TC_WN_NONE);
    tkn(4, 50); CHECK(DH == 0 && WN == TC_WN_NONE);
    /* 1-tick mismatch: warning only */
    tkf(50, 1, 0); CHECK(WN == TC_WN_HEATER_FB_MISMATCH && ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 400);
    tkf(50, 0, 0); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0);
    tkf(50, 0, 1); CHECK(WN == TC_WN_COOLER_FB_MISMATCH && dg(TC_DIAG_COOLER_FB_REMAIN_MS) == 400);
    tkf(50, 0, 0); CHECK(WN == TC_WN_NONE);
    /* sustained heater mismatch faults the heater after RelayFeedbackTimeout */
    for (i = 4; i >= 1; i--) { tkf(50, 1, 0); CHECK(WN == TC_WN_HEATER_FB_MISMATCH && ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 100.0 * i); }
    tkf(50, 1, 0); CHECK(ST == TC_ST_HEATER_FB_FAULT && DH == 0 && DC == 0 && WN == TC_WN_HEATER_FB_MISMATCH);
    tkf(50, 0, 0); CHECK(ST == TC_ST_HEATER_FB_FAULT && WN == TC_WN_HEATER_FB_MISMATCH);   /* latched, frozen */
    /* cooler independently */
    base(); feedback(); init0(); tk(50);
    for (i = 0; i < 4; i++) tkf(50, 0, 1);
    CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_COOLER_FB_MISMATCH);
    tkf(50, 0, 1); CHECK(ST == TC_ST_COOLER_FB_FAULT);
    /* both mismatched: the heater fault wins on the same tick; a cooler mismatch does not reset the heater countdown */
    base(); feedback(); init0(); tk(50);
    tkf(50, 1, 0); tkf(50, 1, 0); tkf(50, 1, 1); tkf(50, 1, 1);
    CHECK(WN == TC_WN_HEATER_FB_MISMATCH && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 100 && dg(TC_DIAG_COOLER_FB_REMAIN_MS) == 300);
    tkf(50, 1, 1); CHECK(ST == TC_ST_HEATER_FB_FAULT);
    base(); feedback(); init0(); tk(50);
    for (i = 0; i < 5; i++) tkf(50, 1, 1);
    CHECK(ST == TC_ST_HEATER_FB_FAULT);
    /* stuck-open heater while heating: command 1, feedback 0 */
    base(); feedback(); init0(); tkn(6, 30); CHECK(DH == 1);
    for (i = 0; i < 4; i++) { tkf(30, 0, 0); CHECK(DH == 1 && WN == TC_WN_HEATER_FB_MISMATCH); }
    tkf(30, 0, 0); CHECK(ST == TC_ST_HEATER_FB_FAULT && DH == 0);
    /* a slow DO loop (feedback lags the command by one tick) only warns on each transition */
    base(); feedback(); init0();
    {
        int prevH = 0, prevC = 0, warned = 0;
        for (i = 0; i < 7; i++) { int h = DH, c = DC; tkf(30, prevH, prevC); prevH = h; prevC = c; if (WN == TC_WN_HEATER_FB_MISMATCH) warned++; }
        CHECK(DH == 1 && ST == TC_ST_HEATER_ON && WN == TC_WN_HEATER_FB_MISMATCH && warned == 1);   /* the tick after the transition */
        for (i = 0; i < 5; i++) { int h = DH, c = DC; tkf(30, prevH, prevC); prevH = h; prevC = c; if (WN == TC_WN_HEATER_FB_MISMATCH) warned++; }
        CHECK(DH == 1 && WN == TC_WN_NONE && ST == TC_ST_HEATER_ON && warned == 1);
    }
    /* FeedbackEnable = 0: silent under any feedback */
    base(); init0(); tk(50);
    for (i = 0; i < 20; i++) { tkf(50, 1, 1); CHECK(WN == TC_WN_NONE && ST == TC_ST_TEMP_AT_SETPT); }
    CHECK(dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0 && dg(TC_DIAG_COOLER_FB_REMAIN_MS) == 0);
    /* no checking while stopped: a sensor fault is not replaced by a feedback fault */
    base(); feedback(); init0(); tk(50); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    for (i = 0; i < 20; i++) tkf(50, 1, 1);
    CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0);
    /* not while disabled */
    base(); feedback(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; init0();
    for (i = 0; i < 20; i++) tkf(50, 1, 1);
    CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    /* R8.3: after Reset the previous command is 0; after a re-Init that keeps the relays it is the kept state */
    base(); feedback(); init0(); tkn(6, 30); CHECK(DH == 1);
    init0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 1);                   /* running + valid: heater kept */
    tkf(30, 1, 0); CHECK(DH == 1 && WN == TC_WN_NONE);                   /* feedback 1 matches the kept command */
    rst0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    tkf(30, 1, 0); CHECK(DH == 0 && WN == TC_WN_HEATER_FB_MISMATCH);      /* feedback 1 vs previous command 0 */
    /* any non-zero feedback value means 1 */
    base(); feedback(); init0(); tkn(6, 30);
    tkf(30, 5, 0); CHECK(WN == TC_WN_NONE);
    tkf(30, -1, 0); CHECK(WN == TC_WN_NONE);
}

/* ----------------------------------------------------------------------- */
/* R9.5 config check  (C16, S11)                                           */
/* ----------------------------------------------------------------------- */
static void bad_setup_faults(int idx, double v)
{
    /* Enable = 1: ConfigFault, stopped, relays 0, Reset does not clear it, a passing Init does */
    base(); two_sensor(); feedback(); S[idx] = v;
    CHECK(init0() == TC_OK);
    CHECK(ST == TC_ST_CONFIG_FAULT && WN == TC_WN_NONE);
    tk2(30, 30); CHECK(ST == TC_ST_CONFIG_FAULT && DH == 0 && DC == 0 && RC == TC_OK);
    tk2n(10, 30, 30); CHECK(ST == TC_ST_CONFIG_FAULT && DH == 0);
    rst0(); CHECK(ST == TC_ST_CONFIG_FAULT);
    tk2(30, 30); CHECK(ST == TC_ST_CONFIG_FAULT && DH == 0);
    CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_CONFIG_FAULT && dg(TC_DIAG_ZONE_INITIALIZED) == 1);
    base(); two_sensor(); feedback(); init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tk2(30, 30); CHECK(ST == TC_ST_HEAT_PENDING);
    /* Enable = 0: ConfigInvalid warning, still disabled and inert */
    base(); two_sensor(); feedback(); S[idx] = v; S[TC_SETUP_TEMP_CTRL_ENABLE] = 0;
    CHECK(init0() == TC_OK);
    CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID);
    tk2(30, 30); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID && DH == 0);
    rst0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID);
    base(); two_sensor(); feedback(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; init0();
    CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);          /* the next passing Init clears it */
}
static void test_R9_config(void)
{
    bad_setup_faults(TC_SETUP_TEMP_UNITS, 2);
    bad_setup_faults(TC_SETUP_DEADBAND_HI, -1);                          /* reversed band */
    bad_setup_faults(TC_SETUP_DEADBAND_LO, -0.001);
    bad_setup_faults(TC_SETUP_DEADBAND_HI, NAN);
    bad_setup_faults(TC_SETUP_HI_LIMIT, 54);                             /* band edge outside the limits */
    bad_setup_faults(TC_SETUP_HI_LIMIT, 55);                             /* strictly inside: HiBand == HiLimit fails */
    bad_setup_faults(TC_SETUP_LO_LIMIT, 45);
    bad_setup_faults(TC_SETUP_LO_LIMIT, 46);
    bad_setup_faults(TC_SETUP_SETPOINT, NAN);
    bad_setup_faults(TC_SETUP_SETPOINT, INFINITY);
    bad_setup_faults(TC_SETUP_SETPOINT, 200);                            /* setpoint outside the limits */
    bad_setup_faults(TC_SETUP_HI_LIMIT, NAN);
    bad_setup_faults(TC_SETUP_LO_LIMIT, NAN);
    bad_setup_faults(TC_SETUP_ERROR_TIMEOUT, 0);
    bad_setup_faults(TC_SETUP_ERROR_TIMEOUT, -1);
    bad_setup_faults(TC_SETUP_ERROR_TIMEOUT, NAN);
    bad_setup_faults(TC_SETUP_ERROR_TIMEOUT, 0.5);                       /* truncates to 0 ms */
    bad_setup_faults(TC_SETUP_DEADBAND_TIMEOUT, 0);
    bad_setup_faults(TC_SETUP_DEADBAND_TIMEOUT, NAN);
    bad_setup_faults(TC_SETUP_AT_SETPT_TIMEOUT, 0);
    bad_setup_faults(TC_SETUP_AT_SETPT_TIMEOUT, -5);
    bad_setup_faults(TC_SETUP_TEMP2_OFFSET, NAN);
    bad_setup_faults(TC_SETUP_TEMP2_OFFSET, INFINITY);
    bad_setup_faults(TC_SETUP_TEMP2_TOLERANCE, -1);
    bad_setup_faults(TC_SETUP_TEMP2_TOLERANCE, NAN);
    bad_setup_faults(TC_SETUP_TEMP_COMPARE_TIMEOUT, 0);
    bad_setup_faults(TC_SETUP_TEMP_COMPARE_TIMEOUT, NAN);
    bad_setup_faults(TC_SETUP_RELAY_FEEDBACK_TIMEOUT, 0);
    bad_setup_faults(TC_SETUP_RELAY_FEEDBACK_TIMEOUT, NAN);
    bad_setup_faults(TC_SETUP_TEMP2_ENABLE, NAN);
    bad_setup_faults(TC_SETUP_FEEDBACK_ENABLE, NAN);
    /* both deadbands zero */
    base(); S[TC_SETUP_DEADBAND_HI] = 0; S[TC_SETUP_DEADBAND_LO] = 0; init0(); CHECK(ST == TC_ST_CONFIG_FAULT);
    /* one zero deadband is allowed */
    base(); S[TC_SETUP_DEADBAND_HI] = 0; init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tk(50.001); CHECK(ST == TC_ST_COOL_PENDING);
    /* NaN enable: check fails, zone disabled -> ConfigInvalid */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = NAN; init0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID);
    /* parameters of disabled features are not checked */
    base(); S[TC_SETUP_TEMP2_OFFSET] = NAN; S[TC_SETUP_TEMP2_TOLERANCE] = -1; S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 0;
    S[TC_SETUP_RELAY_FEEDBACK_TIMEOUT] = NAN; S[TC_SETUP_FILTER_POINTS] = NAN;
    init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    tkn(6, 30); CHECK(DH == 1);
    /* boundary values pass: 1 ms timeouts, tolerance 0, offset 0 */
    base(); two_sensor(); feedback();
    S[TC_SETUP_ERROR_TIMEOUT] = 1; S[TC_SETUP_DEADBAND_TIMEOUT] = 1; S[TC_SETUP_AT_SETPT_TIMEOUT] = 1.9;
    S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 1; S[TC_SETUP_RELAY_FEEDBACK_TIMEOUT] = 1; S[TC_SETUP_TEMP2_TOLERANCE] = 0;
    init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    /* a 1 ms timeout still needs a later tick (one tick of grace) */
    tk2(30, 30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);
    tk2(30, 30); CHECK(DH == 1);
    /* the config check at Init while running: a failing setup stops the zone, relays 0 (R9.3) */
    heat_on(30);
    S[TC_SETUP_HI_LIMIT] = 10; init0();
    CHECK(ST == TC_ST_CONFIG_FAULT && dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    tk(30); CHECK(DH == 0);
    /* huge timeouts are accepted and saturate rather than wrap */
    base(); S[TC_SETUP_ERROR_TIMEOUT] = 1e12; init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tkn(50, NAN); CHECK(ST == TC_ST_TEMP_AT_SETPT);
}

/* ----------------------------------------------------------------------- */
/* R9.1-R9.3 Init and Reset  (C17, C18, S2, S13)                           */
/* ----------------------------------------------------------------------- */
static void test_R9_init_reset(void)
{
    int i;
    /* S2: setpoint change by re-Init while heating keeps the heater, control continues to the new setpoint */
    heat_on(30);
    tk(52); tk(52); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    S[TC_SETUP_SETPOINT] = 60; init0();
    CHECK(ST == TC_ST_HEATER_ON && dg(TC_DIAG_DO_HEATER_MIRROR) == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(52); CHECK(DH == 1 && ST == TC_ST_HEATER_ON && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);   /* 52 < 60: keeps heating */
    tk(60); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);
    tk(60); tk(60); tk(60); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
    /* re-Init while heating where the new logic drops the relay: setpoint moved below the temperature */
    heat_on(30);
    S[TC_SETUP_SETPOINT] = 20; init0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 1);
    tk(30); CHECK(DH == 1);                                              /* 30 >= 20: at-setpoint observed */
    tk(30); tk(30); CHECK(DH == 1); tk(30); CHECK(DH == 0);
    tk(30); CHECK(ST == TC_ST_COOL_PENDING);
    /* cooler kept as well */
    base(); init0(); tkn(6, 70); CHECK(DC == 1);
    S[TC_SETUP_SETPOINT] = 40; init0(); CHECK(ST == TC_ST_COOLER_ON && dg(TC_DIAG_DO_COOLER_MIRROR) == 1);
    tk(70); CHECK(DC == 1);
    /* re-Init with Enable = 0 drops the relays */
    heat_on(30);
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; init0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    tk(30); CHECK(DH == 0);
    /* Init after a fault starts with relays off, clears the fault */
    heat_on(30); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);
    /* Init on a previously disabled zone: relays off even if the old state had a relay on */
    heat_on(30);
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; init0();
    S[TC_SETUP_TEMP_CTRL_ENABLE] = 1; init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    /* first Init after power-up: relays off */
    base(); initz(7); CHECK(ST == TC_ST_TEMP_AT_SETPT && dgz(7, TC_DIAG_DO_HEATER_MIRROR) == 0 && dgz(7, TC_DIAG_DO_COOLER_MIRROR) == 0);
    /* R9.1: Init clears everything */
    base(); two_sensor(); init0(); tk2(50, 50);
    tk2n(10, NAN, 50); tk2n(4, 50, 60); tk2n(3, 50, 200); tk2(50, 50);
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 250);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
    init0();
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 1 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 0);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 0 && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)) && ISNAN(dg(TC_DIAG_TEMP2_AVG)) && ISNAN(dg(TC_DIAG_CONTROL_TEMP)) && ISNAN(dg(TC_DIAG_TEMP1_RAW)));
    CHECK(WN == TC_WN_NONE && dg(TC_DIAG_WARNING_MIRROR) == 0 && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    /* S13 / C17: Reset keeps the setup, clears history, relays off */
    base(); two_sensor(); S[TC_SETUP_SETPOINT] = 60; init0(); tk2(60, 60);
    tk2n(10, NAN, 60); tk2n(4, 60, 70); tk2(60, 60);
    tkn(6, 30); CHECK(DH == 1 && ST == TC_ST_HEATER_ON);                 /* heating on sensor 2 */
    rst0();
    CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_DO_COOLER_MIRROR) == 0);
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 1 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)) && ISNAN(dg(TC_DIAG_TEMP2_AVG)) && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    CHECK(dg(TC_DIAG_HI_BAND) == 65 && dg(TC_DIAG_LO_BAND) == 55 && dg(TC_DIAG_ZONE_INITIALIZED) == 1);   /* setup kept */
    tk2(60, 60); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_ACTIVE_SENSOR) == 1);
    /* Reset clears a fault; with the condition still present it faults again after the full timeout */
    base(); init0(); tk(50); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    rst0(); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    for (i = 0; i < 9; i++) { tk(NAN); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_TEMP1_OUT_OF_RANGE); }
    tk(NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    /* Reset on a running zone drops a running relay */
    heat_on(30); rst0(); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    tk(30); CHECK(DH == 0 && ST == TC_ST_HEAT_PENDING);
    /* Reset on a disabled zone stays disabled */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; init0(); rst0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED);
    /* Reset sets a new time reference: the next tick's elapsed counts from the reset */
    base(); init0(); tk(50); tk(30); tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    NOW += 5000; rst0();
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
}

/* ----------------------------------------------------------------------- */
/* R9.6 fault behaviour  (C19)                                             */
/* ----------------------------------------------------------------------- */
static void test_R9_fault_behaviour(void)
{
    double snap[TC_DIAG_COUNT];
    int i;
    /* warning freezes at the value of the fault tick; countdowns and accumulators stop */
    heat_on(30); two_sensor(); feedback(); init0();                      /* keeps the heater (running, valid) */
    tk2n(10, NAN, 30); CHECK(DH == 1 && dg(TC_DIAG_ACTIVE_SENSOR) == 2 && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    tk2(30, 30); CHECK(WN == TC_WN_RUNNING_ON_TEMP2);
    tk2(30, 52); tk2(30, 52); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    for (i = 0; i < 10; i++) { NOW += P; tkz(0, 30, 200, DH, DC); }       /* excursion on sensor 2 until it fails */
    CHECK(ST == TC_ST_BOTH_SENSORS_FAILED && DH == 0 && DC == 0 && WN == TC_WN_TEMP2_OUT_OF_RANGE);
    TcGetDiag(0, snap, TC_DIAG_COUNT);
    CHECK(snap[TC_DIAG_AT_SETPT_REMAIN_MS] == 200 && snap[TC_DIAG_DO_HEATER_MIRROR] == 0 && snap[TC_DIAG_STATUS_MIRROR] == TC_ST_BOTH_SENSORS_FAILED);
    for (i = 0; i < 20; i++) { tkf(50, 1, 1); CHECK(ST == TC_ST_BOTH_SENSORS_FAILED && WN == TC_WN_TEMP2_OUT_OF_RANGE && DH == 0 && DC == 0); }
    TcGetDiag(0, DG, TC_DIAG_COUNT);
    CHECK(memcmp(snap, DG, sizeof snap) == 0);                           /* nothing moves while stopped */
    /* first fault wins: sensor failure and a feedback timeout maturing on the same tick */
    base(); feedback(); init0(); tk(50);
    tkn(5, NAN);
    for (i = 0; i < 4; i++) { tkf(NAN, 1, 0); CHECK(ST == TC_ST_TEMP_AT_SETPT); }
    tkf(NAN, 1, 0);                                                      /* accumulator 1000 and heater feedback 400 */
    CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    /* disagreement before feedback */
    base(); two_sensor(); feedback(); S[TC_SETUP_FILTER_POINTS] = 1; S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 400; init0();
    tk2(50, 50);
    for (i = 0; i < 4; i++) { NOW += P; tkz(0, 50, 60, 1, 0); }
    CHECK(ST == TC_ST_TEMP_AT_SETPT);
    NOW += P; tkz(0, 50, 60, 1, 0);
    CHECK(ST == TC_ST_TEMP_DISAGREE_FAULT);
    /* heater feedback before cooler feedback (same tick) */
    base(); feedback(); init0(); tk(50);
    for (i = 0; i < 5; i++) tkf(50, 1, 1);
    CHECK(ST == TC_ST_HEATER_FB_FAULT);
    /* a later fault never overwrites the first */
    base(); feedback(); init0(); tk(50);
    for (i = 0; i < 5; i++) tkf(50, 0, 1);
    CHECK(ST == TC_ST_COOLER_FB_FAULT);
    tkn(20, NAN); CHECK(ST == TC_ST_COOLER_FB_FAULT);
    /* every fault code is reachable and >= TC_ST_FAULT_FIRST; every state < 10 */
    CHECK(TC_ST_TEMP1_FAIL_HIGH >= TC_ST_FAULT_FIRST && TC_ST_COOLER_FB_FAULT >= TC_ST_FAULT_FIRST && TC_ST_COOL_PENDING < TC_ST_FAULT_FIRST);
    /* the mirrors always equal the outputs of the last call */
    heat_on(30);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == DH && dg(TC_DIAG_DO_COOLER_MIRROR) == DC && dg(TC_DIAG_STATUS_MIRROR) == ST && dg(TC_DIAG_WARNING_MIRROR) == WN);
    tk(200); CHECK(dg(TC_DIAG_WARNING_MIRROR) == WN && WN == TC_WN_TEMP1_OUT_OF_RANGE);
}

/* ----------------------------------------------------------------------- */
/* R1.4 / R1.5 time  (C21, S15)                                            */
/* ----------------------------------------------------------------------- */
static void test_time(void)
{
    int i;
    /* 2^32 wrap in the middle of a countdown */
    base(); NOW = 0xFFFFFFFFu - 250u; init0();
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    for (i = 0; i < 4; i++) { tk(30); CHECK(DH == 0); }
    CHECK(NOW < 1000u);                                                  /* wrapped */
    tk(30); CHECK(DH == 1);
    /* wrap during the accumulator and the hourly ring */
    base(); NOW = 0xFFFFFFFFu - 450u; init0(); tk(50);
    tkn(9, NAN); CHECK(ST == TC_ST_TEMP_AT_SETPT); tk(NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    /* a backwards step counts as 0 ms: nothing expires, the countdown keeps its value */
    base(); NOW = 100000; init0(); tk(50);
    tk(30); tk(30); tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    NOW -= 50000; tkz(0, 30, 30, DH, DC);
    CHECK(ST == TC_ST_HEAT_PENDING && DH == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    NOW -= 5; tkz(0, 30, 30, DH, DC); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 200);               /* counting resumes from the new reference */
    tk(30); tk(30); CHECK(DH == 1);
    /* a backwards step during an out-of-range stream adds nothing to the accumulator */
    base(); NOW = 100000; init0(); tk(50); tkn(5, NAN);
    NOW -= 20000; tkz(0, NAN, NAN, DH, DC); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 500 && ST == TC_ST_TEMP_AT_SETPT);
    /* the same call time twice: 0 ms elapsed */
    base(); NOW = 1000; init0(); tk(30); tk(30);
    tkz(0, 30, 30, DH, DC); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    /* long gap between calls: a pending countdown expires, an in-range gap is harmless */
    base(); NOW = 0; init0(); tk(50);
    NOW += 600000; tkz(0, 50, 50, DH, DC); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    NOW += 600000; tkz(0, 30, 30, DH, DC); CHECK(DH == 1);              /* 600 s >= 500 ms */
    /* the largest forward step still counts, one more becomes a backwards step */
    base(); NOW = 0; init0(); tk(30);
    NOW += 0x7FFFFFFFu; tkz(0, 30, 30, DH, DC); CHECK(DH == 1);
    base(); NOW = 0; init0(); tk(30);
    NOW += 0x80000000u; tkz(0, 30, 30, DH, DC); CHECK(DH == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
}

/* ----------------------------------------------------------------------- */
/* S14 two zones stepped alternately share nothing                         */
/* ----------------------------------------------------------------------- */
typedef struct { int32_t dh, dc, st, wn; double ctrl; } Rec;
static void run_zone_solo(int zone, int twoSensor, double sp, Rec* rec, int n)
{
    int i;
    base(); if (twoSensor) two_sensor(); S[TC_SETUP_SETPOINT] = sp;
    S[TC_SETUP_HI_LIMIT] = 150; NOW = 0; rstz(zone); initz(zone); DH = DC = 0;
    for (i = 0; i < n; i++) {
        double t1 = (i < 10) ? 30 : (i < 30) ? sp : (i < 45) ? NAN : sp;
        double t2 = (i < 10) ? 32 : sp + 1;
        NOW = (uint32_t)((i + 1) * 100);
        tkz(zone, t1, t2, DH, DC);
        rec[i].dh = DH; rec[i].dc = DC; rec[i].st = ST; rec[i].wn = WN; rec[i].ctrl = dgz(zone, TC_DIAG_CONTROL_TEMP);
    }
}
static void test_two_zones(void)
{
    enum { N = 60 };
    static Rec solo0[N], solo1[N], mix0[N], mix1[N];
    int32_t dh0 = 0, dc0 = 0, dh1 = 0, dc1 = 0;
    int i;
    run_zone_solo(0, 0, 50, solo0, N);
    run_zone_solo(1, 1, 80, solo1, N);
    /* interleaved: same setups, same stimuli, alternated in one tick */
    base(); S[TC_SETUP_SETPOINT] = 50; S[TC_SETUP_HI_LIMIT] = 150; NOW = 0; rstz(0); initz(0);
    base(); two_sensor(); S[TC_SETUP_SETPOINT] = 80; S[TC_SETUP_HI_LIMIT] = 150; NOW = 0; rstz(1); initz(1);
    for (i = 0; i < N; i++) {
        double t1a = (i < 10) ? 30 : (i < 30) ? 50 : (i < 45) ? NAN : 50, t2a = (i < 10) ? 32 : 51;
        double t1b = (i < 10) ? 30 : (i < 30) ? 80 : (i < 45) ? NAN : 80, t2b = (i < 10) ? 32 : 81;
        NOW = (uint32_t)((i + 1) * 100);
        TcCheckTemp(0, NOW, t1a, t2a, dh0, dc0, &dh0, &dc0, &ST, &WN);
        mix0[i].dh = dh0; mix0[i].dc = dc0; mix0[i].st = ST; mix0[i].wn = WN; mix0[i].ctrl = dgz(0, TC_DIAG_CONTROL_TEMP);
        TcCheckTemp(1, NOW, t1b, t2b, dh1, dc1, &dh1, &dc1, &ST, &WN);
        mix1[i].dh = dh1; mix1[i].dc = dc1; mix1[i].st = ST; mix1[i].wn = WN; mix1[i].ctrl = dgz(1, TC_DIAG_CONTROL_TEMP);
    }
    for (i = 0; i < N; i++) {
        CHECK(mix0[i].dh == solo0[i].dh && mix0[i].dc == solo0[i].dc && mix0[i].st == solo0[i].st && mix0[i].wn == solo0[i].wn);
        CHECK(mix1[i].dh == solo1[i].dh && mix1[i].dc == solo1[i].dc && mix1[i].st == solo1[i].st && mix1[i].wn == solo1[i].wn);
        CHECK((ISNAN(mix0[i].ctrl) && ISNAN(solo0[i].ctrl)) || mix0[i].ctrl == solo0[i].ctrl);
        CHECK((ISNAN(mix1[i].ctrl) && ISNAN(solo1[i].ctrl)) || mix1[i].ctrl == solo1[i].ctrl);
    }
    CHECK(solo0[N - 1].st == TC_ST_TEMP1_FAIL_HIGH);                     /* zone 0 (single sensor) faulted on the NaN stream */
    CHECK(solo1[N - 1].st == TC_ST_TEMP_AT_SETPT && solo1[N - 1].wn == TC_WN_RUNNING_ON_TEMP2);   /* zone 1 failed over */
    CHECK(dgz(0, TC_DIAG_HI_BAND) == 55 && dgz(1, TC_DIAG_HI_BAND) == 85);
    /* a fault or Reset on one zone leaves the other alone */
    rstz(0); CHECK(dgz(1, TC_DIAG_ACTIVE_SENSOR) == 2 && dgz(1, TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT);
    /* all 16 zones are usable */
    for (i = 0; i < TC_MAX_ZONES; i++) { base(); S[TC_SETUP_SETPOINT] = 20 + i; NOW = 0; CHECK(initz(i) == TC_OK); }
    for (i = 0; i < TC_MAX_ZONES; i++) CHECK(dgz(i, TC_DIAG_HI_BAND) == 25 + i);
}

/* ----------------------------------------------------------------------- */
/* C22 diagnostics are read-only and report the documented values          */
/* ----------------------------------------------------------------------- */
static void test_diag_readonly(void)
{
    static const double prof[] = { 30, 30, 30, 30, 30, 30, 40, NAN, 50, 50, 52, 52, 52, 52, 55, 200, 200, 50, 50, 50 };
    int32_t log2_[20][4], log3_[20][4];
    double d2[TC_DIAG_COUNT], d3[TC_DIAG_COUNT];
    int i, k;
    base(); two_sensor(); feedback(); NOW = 0; rstz(2); initz(2); DH = DC = 0;
    for (i = 0; i < 20; i++) { NOW += P; tkz(2, prof[i], prof[i] + 1, DH, DC); log2_[i][0] = DH; log2_[i][1] = DC; log2_[i][2] = ST; log2_[i][3] = WN; }
    TcGetDiag(2, d2, TC_DIAG_COUNT);
    base(); two_sensor(); feedback(); NOW = 0; rstz(3); initz(3); DH = DC = 0;
    for (k = 0; k < 5; k++) TcGetDiag(3, d3, TC_DIAG_COUNT);
    for (i = 0; i < 20; i++) {
        NOW += P; tkz(3, prof[i], prof[i] + 1, DH, DC); log3_[i][0] = DH; log3_[i][1] = DC; log3_[i][2] = ST; log3_[i][3] = WN;
        for (k = 0; k < 3; k++) TcGetDiag(3, d3, TC_DIAG_COUNT);
    }
    TcGetDiag(3, d3, TC_DIAG_COUNT);
    CHECK(memcmp(log2_, log3_, sizeof log2_) == 0);
    for (i = 0; i < TC_DIAG_COUNT; i++) CHECK((ISNAN(d2[i]) && ISNAN(d3[i])) || d2[i] == d3[i]);
    /* documented values at a known point: heating, at-setpoint countdown running, sensor 2 excursion */
    base(); two_sensor(); S[TC_SETUP_TEMP2_OFFSET] = 2; S[TC_SETUP_FILTER_POINTS] = 3; NOW = 0; init0();
    tk2n(6, 30, 30); tk2(52, 30); tk2(52, 200);
    TcGetDiag(0, DG, TC_DIAG_COUNT);
    CHECKF(DG[TC_DIAG_CONTROL_TEMP], 52);       CHECK(DG[TC_DIAG_ACTIVE_SENSOR] == 1);
    CHECKF(DG[TC_DIAG_TEMP1_RAW], 52);          CHECKF(DG[TC_DIAG_TEMP2_RAW], 200);   CHECKF(DG[TC_DIAG_TEMP2_CORRECTED], 202);
    CHECKF(DG[TC_DIAG_TEMP1_AVG], (30 + 52 + 52) / 3.0);   CHECKF(DG[TC_DIAG_TEMP2_AVG], 32);   /* 200 not averaged */
    CHECK(DG[TC_DIAG_HI_BAND] == 55 && DG[TC_DIAG_LO_BAND] == 45 && DG[TC_DIAG_INITIAL_HC_FLAG] == 0);
    CHECK(DG[TC_DIAG_DEADBAND_REMAIN_MS] == 0 && DG[TC_DIAG_AT_SETPT_REMAIN_MS] == 200 && DG[TC_DIAG_COMPARE_REMAIN_MS] == 0);
    CHECK(DG[TC_DIAG_HEATER_FB_REMAIN_MS] == 0 && DG[TC_DIAG_COOLER_FB_REMAIN_MS] == 0);
    CHECK(DG[TC_DIAG_TEMP1_OOR_ACCUM_MS] == 0 && DG[TC_DIAG_TEMP2_OOR_ACCUM_MS] == 100);
    CHECK(DG[TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR] == 0 && DG[TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR] == 1);
    CHECK(DG[TC_DIAG_STATUS_MIRROR] == TC_ST_HEATER_ON && DG[TC_DIAG_WARNING_MIRROR] == TC_WN_TEMP2_OUT_OF_RANGE);
    CHECK(DG[TC_DIAG_DO_HEATER_MIRROR] == 1 && DG[TC_DIAG_DO_COOLER_MIRROR] == 0);
    CHECK(DG[TC_DIAG_APPLIED_FILTER_POINTS] == 3 && DG[TC_DIAG_ZONE_INITIALIZED] == 1);
    /* the buffer beyond TC_DIAG_COUNT is untouched */
    {
        double big[TC_DIAG_COUNT + 2]; big[TC_DIAG_COUNT] = 12345; big[TC_DIAG_COUNT + 1] = 54321;
        CHECK(TcGetDiag(0, big, TC_DIAG_COUNT + 2) == TC_OK && big[TC_DIAG_COUNT] == 12345 && big[TC_DIAG_COUNT + 1] == 54321);
    }
    /* Init and Reset mirrors before the first CheckTemp */
    base(); init0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && ISNAN(dg(TC_DIAG_CONTROL_TEMP)) && ISNAN(dg(TC_DIAG_TEMP1_RAW)));
    tk(50); rst0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && ISNAN(dg(TC_DIAG_CONTROL_TEMP)));
}

/* ----------------------------------------------------------------------- */
int main(void)
{
    test_api();
    test_R2_enable();
    test_R3_units();
    test_R4_filter();
    test_R4_nan_and_chatter();
    test_R5_accumulator();
    test_R5_failover();
    test_R5_pause();
    test_R6_offset();
    test_R6_disagreement();
    test_R6_gating();
    test_R7_control();
    test_R7_initial_hc();
    test_R8_feedback();
    test_R9_config();
    test_R9_init_reset();
    test_R9_fault_behaviour();
    test_time();
    test_two_zones();
    test_diag_readonly();
    printf("TempCtl %d.%d.%d unit tests: %d passed, %d failed\n",
           TC_VERSION_MAJOR, TC_VERSION_MINOR, TC_VERSION_PATCH, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
