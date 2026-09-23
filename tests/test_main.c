/*
 * test_main.c - TempCtl v4.0.0 unit tests, compiled together with
 * ../src/tempctl.c (no DLL needed).
 *
 * One function per rule group of TEMPCTL-SPEC-v4.0.0: R1..R9 are the v3
 * rules (change list rows C1..C22 and scenarios S1..S15 of the v3.0.0
 * handoff, named in the comments), R10 is the v4 lifecycle; the v4 tests
 * are labelled with the handoff section 13 item they cover (13.x.y). Tick
 * period is 100 ms; a countdown observed on tick k expires on tick
 * k + timeout/100.
 *
 * Harness convention: init0()/initz() = TcInit + TcStart(permissive 1) and
 * rst0()/rstz() = TcReset + TcStart(permissive 1), so every v3 test keeps
 * its meaning under the v4 lifecycle; oinit0()/orst0() are the raw calls.
 * Ticks use the global PERM as the live permissive (1 unless a test says
 * otherwise).
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
static int32_t  PERM = 1;                       /* live run permissive used by the tick helpers */

/* Enabled, degC, setpoint 50, band 45..55, limits 0..100, ErrorTimeout 1000,
   DeadbandTimeout 500, AtSetPtTimeout 300, single sensor, filter 4, no feedback,
   OperatingConditionTimeout 1000. */
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
    S[TC_SETUP_OPERATING_CONDITION_TIMEOUT] = 1000;
    DH = DC = 0; PERM = 1;
    /* every test starts from a reset (stopped) zone 0 */
    TcReset(0, NOW, &ST, &WN);
}
static void two_sensor(void) { S[TC_SETUP_TEMP2_ENABLE] = 1; }
static void feedback(void)   { S[TC_SETUP_FEEDBACK_ENABLE] = 1; }

/* raw calls */
static int32_t oinitz(int z) { RC = TcInit(z, NOW, S, TC_SETUP_COUNT, &ST, &WN); return RC; }
static int32_t oinit0(void)  { return oinitz(0); }
static int32_t orstz(int z)  { RC = TcReset(z, NOW, &ST, &WN); return RC; }
static int32_t orst0(void)   { return orstz(0); }
static int32_t startz(int z, int perm) { RC = TcStart(z, NOW, perm, &ST, &WN); return RC; }
static int32_t start0(int perm)        { return startz(0, perm); }
static int32_t stopz(int z)  { RC = TcStop(z, NOW, &DH, &DC, &ST, &WN); return RC; }
static int32_t stop0(void)   { return stopz(0); }
/* v3-equivalent: Init / Reset followed by a Start with permissive 1 (RC is the Init / Reset result) */
static int32_t initz(int z) { int32_t rc = oinitz(z); TcStart(z, NOW, 1, &ST, &WN); RC = rc; return rc; }
static int32_t init0(void)  { return initz(0); }
static int32_t rstz(int z)  { int32_t rc = orstz(z); TcStart(z, NOW, 1, &ST, &WN); RC = rc; return rc; }
static int32_t rst0(void)   { return rstz(0); }

/* One tick on zone z with explicit feedback and the global permissive. */
static int32_t tkz(int z, double t1, double t2, int hfb, int cfb)
{
    RC = TcCheckTemp(z, NOW, t1, t2, hfb, cfb, PERM, &DH, &DC, &ST, &WN);
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
    CHECK(TcVersion() == 0x040000);                                      /* 13.1.1 */
    CHECK(TcVersion() == ((TC_VERSION_MAJOR << 16) | (TC_VERSION_MINOR << 8) | TC_VERSION_PATCH));
    CHECK(TcSetupCount() == 18 && TcSetupCount() == TC_SETUP_COUNT);
    CHECK(TcDiagCount() == 28 && TcDiagCount() == TC_DIAG_COUNT);

    base(); NOW = 1000;                                                  /* 13.1.2, 13.1.3 */
    CHECK(TcInit(16, NOW, S, TC_SETUP_COUNT, &a, &b) == TC_ERR_ZONE);
    CHECK(TcInit(-1, NOW, S, TC_SETUP_COUNT, &a, &b) == TC_ERR_ZONE);
    CHECK(TcInit(0, NOW, 0, TC_SETUP_COUNT, &a, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, 17, &a, &b) == TC_ERR_ARG);                  /* the v3 length is refused */
    CHECK(TcInit(0, NOW, S, 19, &a, &b) == TC_ERR_ARG);                  /* exact length only (V4-D4) */
    CHECK(TcInit(0, NOW, S, TC_SETUP_COUNT, 0, &b) == TC_ERR_ARG);
    CHECK(TcInit(0, NOW, S, TC_SETUP_COUNT, &a, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7);                               /* nothing written on a negative return */

    CHECK(TcCheckTemp(16, NOW, 50, 50, 0, 0, 1, &a, &b, &c, &e) == TC_ERR_ZONE);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, 1, 0, &b, &c, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, 1, &a, 0, &c, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, 1, &a, &b, 0, &e) == TC_ERR_ARG);
    CHECK(TcCheckTemp(0, NOW, 50, 50, 0, 0, 1, &a, &b, &c, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7 && c == 7 && e == 7);

    CHECK(TcReset(99, NOW, &a, &b) == TC_ERR_ZONE);
    CHECK(TcReset(0, NOW, 0, &b) == TC_ERR_ARG);
    CHECK(TcReset(0, NOW, &a, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7);

    CHECK(TcStart(16, NOW, 1, &a, &b) == TC_ERR_ZONE);
    CHECK(TcStart(-1, NOW, 1, &a, &b) == TC_ERR_ZONE);
    CHECK(TcStart(0, NOW, 1, 0, &b) == TC_ERR_ARG);
    CHECK(TcStart(0, NOW, 1, &a, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7);
    CHECK(TcStop(16, NOW, &a, &b, &c, &e) == TC_ERR_ZONE);
    CHECK(TcStop(0, NOW, 0, &b, &c, &e) == TC_ERR_ARG);
    CHECK(TcStop(0, NOW, &a, 0, &c, &e) == TC_ERR_ARG);
    CHECK(TcStop(0, NOW, &a, &b, 0, &e) == TC_ERR_ARG);
    CHECK(TcStop(0, NOW, &a, &b, &c, 0) == TC_ERR_ARG);
    CHECK(a == 7 && b == 7 && c == 7 && e == 7);
    /* a refused call changes no state: zone 0 is still uninitialised (base() only Reset it) */
    CHECK(dg(TC_DIAG_ZONE_INITIALIZED) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0);

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
    /* R8.3: after Init, Reset, Stop or Start the previous command is 0 (v4 drops the R9.3 relay keeping) */
    base(); feedback(); init0(); tkn(6, 30); CHECK(DH == 1);
    init0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0);                   /* 13.2.2: re-Init no longer keeps the heater */
    tkf(30, 1, 0); CHECK(DH == 0 && WN == TC_WN_HEATER_FB_MISMATCH);      /* a relay still physically on is a mismatch */
    tkf(30, 0, 0); CHECK(WN == TC_WN_NONE);
    rst0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    tkf(30, 1, 0); CHECK(DH == 0 && WN == TC_WN_HEATER_FB_MISMATCH);      /* feedback 1 vs previous command 0 */
    /* 13.4.8 / R10.9: the intentional off transition of a Stop is not a mismatch; a stuck relay after Stop still warns once started */
    base(); feedback(); init0(); tkn(6, 30); CHECK(DH == 1);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    for (i = 0; i < 10; i++) { tkf(30, 1, 0); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0); }
    tkf(30, 0, 0); CHECK(WN == TC_WN_NONE);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tkf(30, 1, 0); CHECK(WN == TC_WN_HEATER_FB_MISMATCH && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 400);
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
    bad_setup_faults(TC_SETUP_OPERATING_CONDITION_TIMEOUT, 0);           /* v4 check 8 */
    bad_setup_faults(TC_SETUP_OPERATING_CONDITION_TIMEOUT, 0.999);
    bad_setup_faults(TC_SETUP_OPERATING_CONDITION_TIMEOUT, -1);
    bad_setup_faults(TC_SETUP_OPERATING_CONDITION_TIMEOUT, NAN);
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
    S[TC_SETUP_OPERATING_CONDITION_TIMEOUT] = 1;
    init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    /* a 1 ms timeout still needs a later tick (one tick of grace) */
    tk2(30, 30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0);
    tk2(30, 30); CHECK(DH == 1);
    /* the config check at Init while running: a failing setup faults the zone, relays 0 */
    heat_on(30);
    S[TC_SETUP_HI_LIMIT] = 10; init0();
    CHECK(ST == TC_ST_CONFIG_FAULT && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    tk(30); CHECK(DH == 0);
    start0(1); CHECK(ST == TC_ST_CONFIG_FAULT);                          /* Start cannot lift a config fault */
    /* huge operating-condition timeout: accepted */
    base(); S[TC_SETUP_OPERATING_CONDITION_TIMEOUT] = 1e12; init0(); CHECK(ST == TC_ST_TEMP_AT_SETPT);
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
    /* S2 (v4 form, 13.2.2): a setpoint change by re-Init while heating drops the heater and leaves the zone
       stopped; control continues to the new setpoint only after Start. The v3 relay keeping (R9.3) is gone. */
    heat_on(30);
    tk(52); tk(52); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    S[TC_SETUP_SETPOINT] = 60; oinit0();
    CHECK(ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    tk(52); CHECK(DH == 0 && ST == TC_ST_IDLE_STOPPED);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tk(52); CHECK(DH == 0 && ST == TC_ST_HEAT_PENDING);                 /* 52 < LoBand 55 */
    tkn(5, 52); CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    tk(60); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);
    tk(60); tk(60); tk(60); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
    /* re-Init while heating with the setpoint moved below the temperature: the relay is dropped, not held */
    heat_on(30);
    S[TC_SETUP_SETPOINT] = 20; init0(); CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && ST == TC_ST_TEMP_AT_SETPT);
    tk(30); CHECK(DH == 0 && ST == TC_ST_COOL_PENDING);                 /* 30 > HiBand 25 */
    /* cooler dropped as well */
    base(); init0(); tkn(6, 70); CHECK(DC == 1);
    S[TC_SETUP_SETPOINT] = 40; oinit0(); CHECK(ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_COOLER_MIRROR) == 0);
    tk(70); CHECK(DC == 0 && ST == TC_ST_IDLE_STOPPED);
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
    /* first Init after power-up: relays off, stopped */
    base(); oinitz(7); CHECK(ST == TC_ST_IDLE_STOPPED && dgz(7, TC_DIAG_DO_HEATER_MIRROR) == 0 && dgz(7, TC_DIAG_DO_COOLER_MIRROR) == 0);
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
    base(); two_sensor(); feedback(); init0(); tk2n(6, 30, 30); CHECK(DH == 1);
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
    /* zone 1 first: base() resets zone 0, which under v4 would stop it again */
    base(); two_sensor(); S[TC_SETUP_SETPOINT] = 80; S[TC_SETUP_HI_LIMIT] = 150; NOW = 0; rstz(1); initz(1);
    base(); S[TC_SETUP_SETPOINT] = 50; S[TC_SETUP_HI_LIMIT] = 150; NOW = 0; rstz(0); initz(0);
    for (i = 0; i < N; i++) {
        double t1a = (i < 10) ? 30 : (i < 30) ? 50 : (i < 45) ? NAN : 50, t2a = (i < 10) ? 32 : 51;
        double t1b = (i < 10) ? 30 : (i < 30) ? 80 : (i < 45) ? NAN : 80, t2b = (i < 10) ? 32 : 81;
        NOW = (uint32_t)((i + 1) * 100);
        TcCheckTemp(0, NOW, t1a, t2a, dh0, dc0, 1, &dh0, &dc0, &ST, &WN);
        mix0[i].dh = dh0; mix0[i].dc = dc0; mix0[i].st = ST; mix0[i].wn = WN; mix0[i].ctrl = dgz(0, TC_DIAG_CONTROL_TEMP);
        TcCheckTemp(1, NOW, t1b, t2b, dh1, dc1, 1, &dh1, &dc1, &ST, &WN);
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
    CHECK(DG[TC_DIAG_RUN_PERMISSIVE] == 1 && DG[TC_DIAG_OPERATING_CONDITION_REMAIN_MS] == 0 && DG[TC_DIAG_CONTROLLER_STARTED] == 1);
    /* the buffer beyond TC_DIAG_COUNT is untouched (13.1.5) */
    {
        double big[TC_DIAG_COUNT + 2]; big[TC_DIAG_COUNT] = 12345; big[TC_DIAG_COUNT + 1] = 54321;
        CHECK(TcGetDiag(0, big, TC_DIAG_COUNT + 2) == TC_OK && big[TC_DIAG_COUNT] == 12345 && big[TC_DIAG_COUNT + 1] == 54321);
    }
    /* Init and Reset mirrors before the first CheckTemp */
    base(); init0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && ISNAN(dg(TC_DIAG_CONTROL_TEMP)) && ISNAN(dg(TC_DIAG_TEMP1_RAW)));
    tk(50); rst0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && ISNAN(dg(TC_DIAG_CONTROL_TEMP)));
}

/* ----------------------------------------------------------------------- */
/* R10.2 / R10.5 Init and Reset leave the zone stopped  (13.2)             */
/* ----------------------------------------------------------------------- */
static void test_R10_init_reset(void)
{
    int i;
    /* 13.2.1 valid enabled Init: IdleStopped, relays off, Started 0, permissive NaN */
    base(); oinit0();
    CHECK(RC == TC_OK && ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_DO_COOLER_MIRROR) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)) && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && dg(TC_DIAG_WARNING_MIRROR) == 0);
    /* CheckTemp alone never starts control */
    for (i = 0; i < 20; i++) { tk(30); CHECK(RC == TC_OK && ST == TC_ST_IDLE_STOPPED && DH == 0 && DC == 0 && WN == TC_WN_NONE); }
    CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0 && ISNAN(dg(TC_DIAG_CONTROL_TEMP)));
    CHECK(dg(TC_DIAG_TEMP1_RAW) == 30 && ISNAN(dg(TC_DIAG_TEMP1_AVG)));     /* V4-D1: raw mirrored, nothing evaluated */
    CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1 && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    /* stopped ticks evaluate nothing: an out-of-range stream neither warns nor fails the sensor */
    for (i = 0; i < 20; i++) { tk(NAN); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE); }
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0 && ISNAN(dg(TC_DIAG_TEMP1_RAW)));
    start0(1); tk(50); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    /* 13.2.2 Init while heating drops the relay and stops */
    heat_on(30);
    S[TC_SETUP_SETPOINT] = 60; oinit0();
    CHECK(ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    tk(30); CHECK(DH == 0 && ST == TC_ST_IDLE_STOPPED);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0 && dg(TC_DIAG_HI_BAND) == 65);   /* new setup, fresh countdown */
    tkn(5, 30); CHECK(DH == 1);
    /* 13.2.11 a direct Init / Reset clears the internal commands (mirrors 0) although it returns none */
    heat_on(30); orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_DO_COOLER_MIRROR) == 0);
    base(); init0(); tkn(6, 70); CHECK(DC == 1); oinit0(); CHECK(dg(TC_DIAG_DO_COOLER_MIRROR) == 0 && ST == TC_ST_IDLE_STOPPED);
    /* 13.2.8 Reset leaves the controller stopped; CheckTemp alone cannot resume it */
    base(); init0(); tk(50); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE && dg(TC_DIAG_CONTROLLER_STARTED) == 0 && ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));
    for (i = 0; i < 10; i++) { tk(30); CHECK(ST == TC_ST_IDLE_STOPPED && DH == 0); }
    start0(1); tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    /* 13.2.9 ConfigFault survives Reset, Start and Stop */
    base(); S[TC_SETUP_DEADBAND_HI] = -1; oinit0(); CHECK(ST == TC_ST_CONFIG_FAULT);
    orst0(); CHECK(ST == TC_ST_CONFIG_FAULT); start0(1); CHECK(ST == TC_ST_CONFIG_FAULT && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    stop0(); CHECK(ST == TC_ST_CONFIG_FAULT && DH == 0 && DC == 0);
    tk(30); CHECK(ST == TC_ST_CONFIG_FAULT && DH == 0);
    /* disabled Init and Reset: TempCtrlDisabled */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; oinit0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    orst0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    /* 13.2.10 host sequences: Stop -> apply zeros -> Init, and Stop -> apply zeros -> Reset; both leave IdleStopped */
    heat_on(30);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED);
    S[TC_SETUP_SETPOINT] = 55; oinit0(); CHECK(ST == TC_ST_IDLE_STOPPED);
    tk(30); CHECK(ST == TC_ST_IDLE_STOPPED && DH == 0);
    start0(1); tkn(6, 30); CHECK(DH == 1 && dg(TC_DIAG_LO_BAND) == 50);
    stop0(); CHECK(DH == 0);
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED);
    tk(30); CHECK(ST == TC_ST_IDLE_STOPPED && DH == 0);
    /* Reset clears the blocked and tripped lifecycle states */
    base(); oinit0(); start0(0); CHECK(ST == TC_ST_IDLE_START_BLOCKED);
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    heat_on(30); PERM = 0; tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING);
    PERM = 1; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    heat_on(30); PERM = 0; tk(30); tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 900);
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    PERM = 1; tkn(20, 30); CHECK(ST == TC_ST_IDLE_STOPPED);
}

/* ----------------------------------------------------------------------- */
/* R10.3 Start  (13.2.3 - 13.2.5, 13.3)                                    */
/* ----------------------------------------------------------------------- */
static void test_R10_start(void)
{
    int i;
    /* 13.2.3 uninitialised / disabled zones cannot start */
    NOW = 1000; DH = DC = 5;
    CHECK(startz(11, 1) == TC_OK && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    CHECK(dgz(11, TC_DIAG_ZONE_INITIALIZED) == 0 && dgz(11, TC_DIAG_CONTROLLER_STARTED) == 0 && ISNAN(dgz(11, TC_DIAG_RUN_PERMISSIVE)));
    tkz(11, 30, 30, 0, 0); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && DH == 0 && DC == 0);
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; oinit0();
    CHECK(start0(1) == TC_OK && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    tk(30); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && DH == 0);
    CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));                              /* 13.6.6: disabled calls do not record it */
    /* disabled with ConfigInvalid keeps that warning through Start and Stop */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; S[TC_SETUP_DEADBAND_HI] = -1; oinit0();
    start0(1); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID);
    stop0();   CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID && DH == 0 && DC == 0);
    /* 13.2.4 accepted Start: Started 1, relays stay 0 until a qualified CheckTemp decision */
    base(); oinit0(); NOW = 5000;
    CHECK(start0(1) == TC_OK && ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE);
    CHECK(dg(TC_DIAG_CONTROLLER_STARTED) == 1 && dg(TC_DIAG_RUN_PERMISSIVE) == 1 && dg(TC_DIAG_DO_HEATER_MIRROR) == 0);
    CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && ISNAN(dg(TC_DIAG_CONTROL_TEMP)));
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && DH == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    tkn(4, 30); CHECK(DH == 0);
    tk(30); CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    /* 13.2.5 / 13.4.15 Start while started is a no-op: no countdown restart, no permissive evaluation, no time re-base */
    base(); init0(); tk(30); tk(30); tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    CHECK(start0(1) == TC_OK && ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    CHECK(start0(0) == TC_OK && ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300 && dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    NOW += 5000; start0(1); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 300);
    tk(30); CHECK(DH == 1);                                                /* 5.1 s >= the 300 ms left */
    heat_on(30); tk(52); tk(52); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200);
    start0(1); CHECK(ST == TC_ST_HEATER_ON && DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 200 && dg(TC_DIAG_DO_HEATER_MIRROR) == 1);
    tk(52); CHECK(DH == 1 && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 100);
    tk(52); CHECK(DH == 0 && ST == TC_ST_TEMP_AT_SETPT);
    /* ... and the next CheckTemp performs the live safety action */
    heat_on(30); start0(0); CHECK(DH == 1 && ST == TC_ST_HEATER_ON);
    PERM = 0; tk(30); CHECK(DH == 0 && DC == 0 && ST == TC_ST_OPERATING_CONDITION_PENDING);
    /* 13.3.1 blocked Start: TC_OK, IdleStartBlocked, warning 8, relays 0, no countdown */
    base(); oinit0(); NOW = 0;
    CHECK(start0(0) == TC_OK && ST == TC_ST_IDLE_START_BLOCKED && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    CHECK(dg(TC_DIAG_CONTROLLER_STARTED) == 0 && dg(TC_DIAG_RUN_PERMISSIVE) == 0 && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_DO_COOLER_MIRROR) == 0);
    /* 13.3.2 repeated CheckTemp while false: no promotion, ever */
    PERM = 0;
    for (i = 0; i < 50; i++) { tk(30); CHECK(ST == TC_ST_IDLE_START_BLOCKED && WN == TC_WN_OPERATING_CONDITION_NOT_MET && DH == 0 && DC == 0); }
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    NOW += 3600000u; tk(30); CHECK(ST == TC_ST_IDLE_START_BLOCKED);
    /* 13.3.3 recovery does not auto-start; warning 8 clears live, status 7 stays */
    PERM = 1;
    for (i = 0; i < 20; i++) { tk(30); CHECK(ST == TC_ST_IDLE_START_BLOCKED && WN == TC_WN_NONE && DH == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0); }
    CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    PERM = 0; tk(30); CHECK(WN == TC_WN_OPERATING_CONDITION_NOT_MET);    /* and back */
    PERM = 1;
    start0(0); CHECK(ST == TC_ST_IDLE_START_BLOCKED && WN == TC_WN_OPERATING_CONDITION_NOT_MET);   /* still blocked */
    /* 13.3.4 a later successful Start clears the blocked status */
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE && dg(TC_DIAG_CONTROLLER_STARTED) == 1);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    /* 13.3.5 Stop acknowledges a blocked Start */
    base(); oinit0(); start0(0); CHECK(ST == TC_ST_IDLE_START_BLOCKED);
    stop0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE && DH == 0 && DC == 0);
    PERM = 0; tk(30); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);   /* IdleStopped never warns 8 */
    PERM = 1;
    /* Start from IdleStopped after a normal Stop */
    heat_on(30); stop0(); CHECK(ST == TC_ST_IDLE_STOPPED);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    /* Start preserves sensor failure / failover, accumulators and hourly counts; clears averages and Initial_HC_Flag (13.2.7, 13.6.7) */
    base(); two_sensor(); init0(); tk2n(4, 50, 50); CHECK(dg(TC_DIAG_INITIAL_HC_FLAG) == 1);
    tk2n(10, NAN, 50); tk2(50, 50); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && WN == TC_WN_RUNNING_ON_TEMP2);
    tk2(50, 200); tk2(50, 50); CHECK(dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 50);
    stop0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_RUNNING_ON_TEMP2);
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 50);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_RUNNING_ON_TEMP2);
    CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 1000 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 50);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1);
    CHECK(ISNAN(dg(TC_DIAG_TEMP1_AVG)) && ISNAN(dg(TC_DIAG_TEMP2_AVG)) && dg(TC_DIAG_INITIAL_HC_FLAG) == 0);
    tk2(50, 50); CHECK(dg(TC_DIAG_ACTIVE_SENSOR) == 2 && WN == TC_WN_RUNNING_ON_TEMP2 && ST == TC_ST_TEMP_AT_SETPT);
    CHECKF(dg(TC_DIAG_TEMP2_AVG), 50);                                    /* the average restarts from the new samples */
    /* hourly counts age in wall time across a Stop (at the Start, and on stopped ticks) */
    base(); init0(); tk(50); tk(200); tk(50); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    stop0(); NOW += 61u * 60000u; start0(1); CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    tk(50); CHECK(ST == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    base(); init0(); tk(50); tk(200); tk(50); stop0(); CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50);
    for (i = 0; i < 61; i++) { NOW += 60000u; tk(50); }
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0 && ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 50);   /* the accumulator does not drain while stopped */
    /* V4-D10: repeated Stop calls must age both rings before rebasing time. */
    base(); two_sensor(); NOW = 0xFFFFFF00u; init0();
    tk2(200, 200); stop0();
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1);
    NOW += 59u * 60000u; stop0();
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 1);
    NOW += 2u * 60000u; stop0();
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0 && dg(TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR) == 0);
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100 && dg(TC_DIAG_TEMP2_OOR_ACCUM_MS) == 100);
    CHECK(ST == TC_ST_IDLE_STOPPED && DH == 0 && DC == 0);
    base(); NOW = 0; init0(); tk(200); stop0();
    for (i = 0; i < 61; i++) { NOW += 60000u; stop0(); tk(50); }
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0 && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100);
    /* Same/backwards timestamps do not age history; Start must not count Stop's gap twice. */
    base(); NOW = 100000u; init0(); tk(200); stop0(); stop0();
    NOW -= 50000u; stop0();
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
    NOW += 30u * 60000u; stop0(); start0(1);
    CHECK(dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);
}

/* ----------------------------------------------------------------------- */
/* R10.4 Stop  (13.2.6, 13.2.7, 13.6.8, 13.6.9)                            */
/* ----------------------------------------------------------------------- */
static void test_R10_stop(void)
{
    int i;
    /* 13.2.6 Stop from every active status returns 0/0 and IdleStopped */
    base(); init0(); tk(50); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    DH = DC = 5; stop0(); CHECK(RC == TC_OK && DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    base(); init0(); tk(30); tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    heat_on(30); tk(52); CHECK(dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 300);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_AT_SETPT_REMAIN_MS) == 0);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0 && dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED);
    base(); init0(); tk(70); tk(70); CHECK(ST == TC_ST_COOL_PENDING); stop0(); CHECK(DC == 0 && ST == TC_ST_IDLE_STOPPED);
    base(); init0(); tkn(6, 70); CHECK(DC == 1 && ST == TC_ST_COOLER_ON);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_COOLER_MIRROR) == 0);
    /* after the Stop nothing runs; a Start is needed; stopped time is excluded (13.5.5) */
    for (i = 0; i < 10; i++) { tk(70); CHECK(ST == TC_ST_IDLE_STOPPED && DC == 0); }
    NOW += 100000; start0(1); tk(70); CHECK(ST == TC_ST_COOL_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    /* Stop from IdleStopped stays IdleStopped; Stop is idempotent */
    stop0(); stop0(); CHECK(ST == TC_ST_IDLE_STOPPED && DH == 0 && DC == 0);
    /* 13.6.9 Stop from an uninitialised zone */
    DH = DC = 5; ST = WN = 5; CHECK(stopz(12) == TC_OK && DH == 0 && DC == 0 && ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    CHECK(dgz(12, TC_DIAG_ZONE_INITIALIZED) == 0);
    /* ... from a disabled zone, with and without ConfigInvalid */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; oinit0(); stop0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; S[TC_SETUP_LO_LIMIT] = 60; oinit0(); stop0(); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_CONFIG_INVALID);
    /* ... from a faulted zone: fault and frozen warning kept, never cleared by Stop */
    base(); init0(); tk(50); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    stop0(); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && WN == TC_WN_TEMP1_OUT_OF_RANGE && DH == 0 && DC == 0);
    tk(50); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    /* 13.6.8 ordinary Stop clears transient warnings 1..5 */
    base(); init0(); tk(50); tk(200); CHECK(WN == TC_WN_TEMP1_OUT_OF_RANGE);
    stop0(); CHECK(WN == TC_WN_NONE && ST == TC_ST_IDLE_STOPPED);
    CHECK(dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 100 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 1);   /* the history itself is kept (13.2.7) */
    base(); two_sensor(); init0(); tk2(50, 50); tk2(50, 200); CHECK(WN == TC_WN_TEMP2_OUT_OF_RANGE);
    stop0(); CHECK(WN == TC_WN_NONE);
    base(); feedback(); init0(); tk(50); tkf(50, 1, 0); CHECK(WN == TC_WN_HEATER_FB_MISMATCH);
    stop0(); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0);
    base(); feedback(); init0(); tk(50); tkf(50, 0, 1); CHECK(WN == TC_WN_COOLER_FB_MISMATCH);
    stop0(); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COOLER_FB_REMAIN_MS) == 0);
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 1; init0(); tk2(50, 50); tk2n(3, 50, 60); CHECK(WN == TC_WN_TEMP_DISAGREE);
    stop0(); CHECK(WN == TC_WN_NONE && dg(TC_DIAG_COMPARE_REMAIN_MS) == 0);
    /* ... but RunningOnTemp2 persists (v3 rule) */
    base(); two_sensor(); init0(); tk2(50, 50); tk2n(10, NAN, 50); tk2(50, 50); CHECK(WN == TC_WN_RUNNING_ON_TEMP2);
    stop0(); CHECK(WN == TC_WN_RUNNING_ON_TEMP2 && ST == TC_ST_IDLE_STOPPED);
    tk2(50, 50); CHECK(WN == TC_WN_RUNNING_ON_TEMP2 && ST == TC_ST_IDLE_STOPPED);
    /* Stop leaves the RunPermissive diagnostic unchanged */
    base(); init0(); tk(50); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1); stop0(); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    base(); oinit0(); CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE))); stop0(); CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));
    /* Stop does not evaluate a permissive: a later Start does */
    base(); oinit0(); stop0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    start0(0); CHECK(ST == TC_ST_IDLE_START_BLOCKED); start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT);
}

/* ----------------------------------------------------------------------- */
/* R10.7 / R10.8 permissive loss during control  (13.4)                    */
/* ----------------------------------------------------------------------- */
static void test_R10_permissive_loss(void)
{
    int i;
    double snap[TC_DIAG_COUNT];
    /* 13.4.1 first false sample: both relays 0 at once, pending, full timeout, Started 0 */
    heat_on(30); PERM = 0;
    tk(30); CHECK(DH == 0 && DC == 0 && ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 1000 && dg(TC_DIAG_CONTROLLER_STARTED) == 0 && dg(TC_DIAG_RUN_PERMISSIVE) == 0);
    CHECK(dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && dg(TC_DIAG_STATUS_MIRROR) == TC_ST_OPERATING_CONDITION_PENDING);
    /* the same from a running cooler and from the pending / at-setpoint states */
    base(); init0(); tkn(6, 70); CHECK(DC == 1); PERM = 0; tk(70); CHECK(DC == 0 && DH == 0 && ST == TC_ST_OPERATING_CONDITION_PENDING);
    base(); init0(); tk(30); tk(30); PERM = 0; tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    base(); init0(); tk(50); PERM = 0; tk(50); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING);
    /* 13.4.2 one-tick loss then recovery: tripped, no fault, no restart; warning 8 clears live (13.6.2) */
    heat_on(30); PERM = 0; tk(30); PERM = 1;
    tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE && DH == 0 && DC == 0);
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0 && dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    for (i = 0; i < 50; i++) { tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && DH == 0 && WN == TC_WN_NONE); }
    CHECK(dg(TC_DIAG_TEMP1_RAW) == 30 && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 0);
    PERM = 0; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_OPERATING_CONDITION_NOT_MET && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    tkn(30, 30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);    /* a false permissive while tripped never counts down */
    PERM = 1; tk(30); CHECK(WN == TC_WN_NONE && ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);
    /* 13.3.6 / 13.4.7 a refused Start from tripped keeps the trip cause; 13.4.6 a successful Start clears it */
    start0(0); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE);   /* PERM is 1 again */
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE && dg(TC_DIAG_CONTROLLER_STARTED) == 1);
    tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    tkn(5, 30); CHECK(DH == 1);
    /* 13.4.3 false held continuously: fault on the exact qualified tick (observation tick + 10) */
    heat_on(30); PERM = 0;
    tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 1000);
    for (i = 9; i >= 1; i--) { tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_OPERATING_CONDITION_NOT_MET && DH == 0 && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 100.0 * i); }
    tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT && WN == TC_WN_OPERATING_CONDITION_NOT_MET && DH == 0 && DC == 0);
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 0 && TC_ST_OPERATING_CONDITION_FAULT >= TC_ST_FAULT_FIRST);
    /* 13.4.11 / 13.6.3 latched, warning frozen; later permissive changes, Start and Stop change nothing */
    TcGetDiag(0, snap, TC_DIAG_COUNT);
    PERM = 1; for (i = 0; i < 20; i++) { tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT && WN == TC_WN_OPERATING_CONDITION_NOT_MET && DH == 0); }
    start0(1); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    stop0();   CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT && WN == TC_WN_OPERATING_CONDITION_NOT_MET && DH == 0 && DC == 0);
    TcGetDiag(0, DG, TC_DIAG_COUNT); CHECK(memcmp(snap, DG, sizeof snap) == 0);   /* RunPermissive frozen at 0 too */
    CHECK(DG[TC_DIAG_RUN_PERMISSIVE] == 0);
    /* Reset clears it and leaves the zone stopped; Start then works */
    orst0(); CHECK(ST == TC_ST_IDLE_STOPPED && WN == TC_WN_NONE);
    tk(30); CHECK(ST == TC_ST_IDLE_STOPPED); start0(1); tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    /* 13.4.4 recovery one tick before the timeout cancels the fault */
    heat_on(30); PERM = 0; tk(30); tkn(9, 30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 100);
    PERM = 1; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    PERM = 0; tkn(20, 30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);   /* no second countdown from the tripped state */
    PERM = 1;
    /* 13.4.5 Stop during pending cancels the escalation and keeps the cause; warning 8 retained while the last permissive was false (13.6.8) */
    heat_on(30); PERM = 0; tk(30); tk(30); tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    stop0(); CHECK(DH == 0 && DC == 0 && ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0);
    for (i = 0; i < 30; i++) { tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_OPERATING_CONDITION_NOT_MET); }
    PERM = 1; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE);
    stop0(); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE);   /* Stop from tripped keeps the cause */
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT);
    /* Stop from pending when the last evaluated permissive was true: no warning 8 */
    heat_on(30); PERM = 0; tk(30); PERM = 1; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);
    stop0(); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE);
    /* 13.4.13 Start with false permissive while pending: pending kept, countdown and time reference untouched */
    heat_on(30); PERM = 0; tk(30); tk(30); tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    start0(0); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_OPERATING_CONDITION_NOT_MET && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    NOW += 250; start0(0); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800 && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 450);     /* 250 + 100 ms since the last CheckTemp */
    tkn(4, 30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 50);
    tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT);
    /* 13.4.14 Start with true permissive while pending restarts explicitly and clears the countdown */
    heat_on(30); PERM = 0; tk(30); tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 900);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT && WN == TC_WN_NONE && dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0 && dg(TC_DIAG_CONTROLLER_STARTED) == 1);
    PERM = 1; tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    /* ... and while tripped */
    heat_on(30); PERM = 0; tk(30); PERM = 1; tk(30); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);
    start0(1); CHECK(ST == TC_ST_TEMP_AT_SETPT); tk(30); CHECK(ST == TC_ST_HEAT_PENDING);
    /* 13.4.9 sensor / control state frozen while pending, tripped and stopped: no accumulation, no averaging, no failure */
    heat_on(30); PERM = 0; tk(30);
    for (i = 0; i < 5; i++) tk(NAN);
    CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_OPERATING_CONDITION_NOT_MET && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    CHECK(ISNAN(dg(TC_DIAG_TEMP1_RAW)) && dg(TC_DIAG_CONTROL_TEMP) == 30 && dg(TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR) == 0);
    CHECKF(dg(TC_DIAG_TEMP1_AVG), 30);
    PERM = 1; tkn(20, NAN); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 0);
    /* 13.4.8 relay feedback: the intentional off transition is never a mismatch; a relay still closed is caught after the restart */
    base(); feedback(); init0(); tkn(6, 30); CHECK(DH == 1); PERM = 0;
    tkf(30, 1, 0); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && DH == 0 && WN == TC_WN_OPERATING_CONDITION_NOT_MET && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0);
    for (i = 0; i < 8; i++) { tkf(30, 1, 0); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 0); }
    PERM = 1; tkf(30, 1, 0); CHECK(ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED && WN == TC_WN_NONE);
    start0(1); tkf(30, 1, 0); CHECK(WN == TC_WN_HEATER_FB_MISMATCH && ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_HEATER_FB_REMAIN_MS) == 400);
    for (i = 0; i < 4; i++) tkf(30, 1, 0);
    CHECK(ST == TC_ST_HEATER_FB_FAULT);
    /* 13.4.10 an existing fault maturing on the first false tick wins: sensor, disagreement, heater fb, cooler fb */
    base(); init0(); tk(50); tkn(9, NAN); PERM = 0; tk(NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && WN == TC_WN_TEMP1_OUT_OF_RANGE);
    PERM = 1; tk(50); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH);
    base(); two_sensor(); S[TC_SETUP_FILTER_POINTS] = 1; S[TC_SETUP_TEMP_COMPARE_TIMEOUT] = 400; init0();
    tk2(50, 50); tk2n(4, 50, 60); PERM = 0; tk2(50, 60); CHECK(ST == TC_ST_TEMP_DISAGREE_FAULT);
    PERM = 1;
    base(); feedback(); init0(); tk(50); for (i = 0; i < 4; i++) tkf(50, 1, 0);
    PERM = 0; tkf(50, 1, 0); CHECK(ST == TC_ST_HEATER_FB_FAULT); PERM = 1;
    base(); feedback(); init0(); tk(50); for (i = 0; i < 4; i++) tkf(50, 0, 1);
    PERM = 0; tkf(50, 0, 1); CHECK(ST == TC_ST_COOLER_FB_FAULT); PERM = 1;
    /* a fault that would have matured one tick LATER does not happen: the trip freezes it */
    base(); init0(); tk(50); tkn(8, NAN); PERM = 0; tk(NAN); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 900);
    tkn(5, NAN); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && dg(TC_DIAG_TEMP1_OOR_ACCUM_MS) == 900);
    PERM = 1;
    /* 13.4.12 TcGetDiag at any rate does not advance the countdown */
    heat_on(30); PERM = 0; tk(30); tk(30);
    for (i = 0; i < 100; i++) TcGetDiag(0, DG, TC_DIAG_COUNT);
    CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 900 && ST == TC_ST_OPERATING_CONDITION_PENDING);
    tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    PERM = 1;
    /* the trip clears transient warnings like Stop does; RunningOnTemp2 masks warning 8 (13.6.1) */
    base(); init0(); tk(50); tk(200); CHECK(WN == TC_WN_TEMP1_OUT_OF_RANGE);
    PERM = 0; tk(200); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    PERM = 1;
    base(); two_sensor(); init0(); tk2(50, 50); tk2n(10, NAN, 50); tk2(50, 50); CHECK(WN == TC_WN_RUNNING_ON_TEMP2);
    PERM = 0; tk2(50, 50); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING && WN == TC_WN_RUNNING_ON_TEMP2);
    tkn(10, 50); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT && WN == TC_WN_RUNNING_ON_TEMP2);
    PERM = 1;
}

/* ----------------------------------------------------------------------- */
/* R10.7 timing of the operating-condition countdown  (13.5)               */
/* ----------------------------------------------------------------------- */
static void test_R10_time(void)
{
    int i;
    /* 13.5.1 across the 2^32 wrap */
    base(); NOW = 0xFFFFFFFFu - 850u; init0(); tkn(6, 30); CHECK(DH == 1);
    PERM = 0; tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING);
    for (i = 0; i < 9; i++) { tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_PENDING); }
    CHECK(NOW < 2000u);                                                  /* wrapped */
    tk(30); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT);
    PERM = 1;
    /* 13.5.2 / 13.5.3 a backwards step and the same timestamp add nothing */
    heat_on(30); PERM = 0; tk(30); tk(30); tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    NOW -= 50000; tkz(0, 30, 30, 0, 0); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800 && ST == TC_ST_OPERATING_CONDITION_PENDING);
    tkz(0, 30, 30, 0, 0); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 800);
    tk(30); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 700);
    /* 13.5.4 a long gap expires the pending timeout */
    NOW += 600000; tkz(0, 30, 30, 0, 0); CHECK(ST == TC_ST_OPERATING_CONDITION_FAULT);
    PERM = 1;
    /* 13.5.5 stopped time is excluded: the Start re-bases the time reference */
    base(); init0(); tk(30); tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    stop0(); NOW += 3600000u; tk(30); CHECK(ST == TC_ST_IDLE_STOPPED);
    NOW += 3600000u; start0(1); tk(30); CHECK(ST == TC_ST_HEAT_PENDING && dg(TC_DIAG_DEADBAND_REMAIN_MS) == 500);
    tk(30); CHECK(dg(TC_DIAG_DEADBAND_REMAIN_MS) == 400);
    /* the pending countdown runs on CheckTemp wall time */
    heat_on(30); PERM = 0; tk(30); NOW += 400; tkz(0, 30, 30, 0, 0); CHECK(dg(TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 600);
    PERM = 1;
}

/* ----------------------------------------------------------------------- */
/* status / warning consistency and the new diagnostics  (13.6)            */
/* ----------------------------------------------------------------------- */
static void test_R10_status_warning(void)
{
    int i;
    /* 13.6.4 codes */
    CHECK(TC_ST_IDLE_STOPPED < TC_ST_FAULT_FIRST && TC_ST_IDLE_START_BLOCKED < TC_ST_FAULT_FIRST);
    CHECK(TC_ST_OPERATING_CONDITION_PENDING < TC_ST_FAULT_FIRST && TC_ST_IDLE_OPERATING_CONDITION_TRIPPED < TC_ST_FAULT_FIRST);
    CHECK(TC_ST_OPERATING_CONDITION_FAULT == 17 && TC_ST_IDLE_STOPPED == 6 && TC_ST_IDLE_START_BLOCKED == 7);
    CHECK(TC_ST_OPERATING_CONDITION_PENDING == 8 && TC_ST_IDLE_OPERATING_CONDITION_TRIPPED == 9 && TC_WN_OPERATING_CONDITION_NOT_MET == 8);
    CHECK(TC_SETUP_OPERATING_CONDITION_TIMEOUT == 17 && TC_DIAG_RUN_PERMISSIVE == 25 && TC_DIAG_OPERATING_CONDITION_REMAIN_MS == 26 && TC_DIAG_CONTROLLER_STARTED == 27);
    /* 13.6.5 the mirrors follow every stateful call, not TcGetDiag */
    base(); oinit0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == ST && dg(TC_DIAG_WARNING_MIRROR) == WN && ST == TC_ST_IDLE_STOPPED);
    start0(0); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_START_BLOCKED && dg(TC_DIAG_WARNING_MIRROR) == TC_WN_OPERATING_CONDITION_NOT_MET);
    start0(1); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_TEMP_AT_SETPT && dg(TC_DIAG_WARNING_MIRROR) == 0);
    tkn(6, 30); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_HEATER_ON && dg(TC_DIAG_DO_HEATER_MIRROR) == 1 && DH == 1);
    stop0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && dg(TC_DIAG_DO_HEATER_MIRROR) == 0 && DH == 0);
    orst0(); CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && dg(TC_DIAG_WARNING_MIRROR) == 0);
    for (i = 0; i < 3; i++) TcGetDiag(0, DG, TC_DIAG_COUNT);
    CHECK(dg(TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && dg(TC_DIAG_CONTROLLER_STARTED) == 0);
    /* 13.6.6 RunPermissive: NaN after Init / Reset; enabled non-faulted Start and CheckTemp update it;
       disabled, faulted and idempotent calls do not; Stop leaves it */
    base(); oinit0(); CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));
    start0(0); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 0);
    PERM = 1; tk(30); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    start0(1); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    start0(0); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);                     /* idempotent Start: not evaluated */
    PERM = 0; tk(30); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 0 && ST == TC_ST_OPERATING_CONDITION_PENDING);
    PERM = 1; tk(30); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1 && ST == TC_ST_IDLE_OPERATING_CONDITION_TRIPPED);
    start0(0); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 0);                     /* refused Start from tripped: evaluated (V4-D2) */
    stop0(); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 0 && WN == TC_WN_OPERATING_CONDITION_NOT_MET);
    orst0(); CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));
    tk(30); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1); oinit0(); CHECK(ISNAN(dg(TC_DIAG_RUN_PERMISSIVE)));
    base(); init0(); tk(50); tkn(10, NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_RUN_PERMISSIVE) == 1);
    PERM = 0; tk(50); start0(0); CHECK(dg(TC_DIAG_RUN_PERMISSIVE) == 1);   /* faulted: not evaluated */
    PERM = 1;
    /* the fault tick itself records the permissive it evaluated */
    base(); init0(); tk(50); tkn(9, NAN); PERM = 0; tk(NAN); CHECK(ST == TC_ST_TEMP1_FAIL_HIGH && dg(TC_DIAG_RUN_PERMISSIVE) == 0);
    PERM = 1;
    /* status 0 never warns 8: disabled with a false permissive */
    base(); S[TC_SETUP_TEMP_CTRL_ENABLE] = 0; oinit0(); start0(0); CHECK(ST == TC_ST_TEMP_CTRL_DISABLED && WN == TC_WN_NONE);
    PERM = 0; tk(30); CHECK(WN == TC_WN_NONE && ST == TC_ST_TEMP_CTRL_DISABLED); PERM = 1;
}

/* ----------------------------------------------------------------------- */
/* independent lifecycle per zone  (13.7)                                  */
/* ----------------------------------------------------------------------- */
static void test_R10_two_zones(void)
{
    int i;
    int32_t dh0 = 0, dc0 = 0, dh1 = 0, dc1 = 0, st0 = 0, wn0 = 0, st1 = 0, wn1 = 0;
    /* zone 0 trips and faults on its permissive while zone 1 keeps heating */
    base(); NOW = 0; orstz(1); oinitz(1); startz(1, 1);
    base(); NOW = 0; orstz(0); oinitz(0); startz(0, 1);              /* zone 0 last: base() resets it */
    for (i = 1; i <= 6; i++) {
        NOW = (uint32_t)(i * 100);
        TcCheckTemp(0, NOW, 30, 30, dh0, dc0, 1, &dh0, &dc0, &st0, &wn0);
        TcCheckTemp(1, NOW, 30, 30, dh1, dc1, 1, &dh1, &dc1, &st1, &wn1);
    }
    CHECK(dh0 == 1 && dh1 == 1);
    for (i = 7; i <= 17; i++) {
        NOW = (uint32_t)(i * 100);
        TcCheckTemp(0, NOW, 30, 30, dh0, dc0, 0, &dh0, &dc0, &st0, &wn0);
        TcCheckTemp(1, NOW, 30, 30, dh1, dc1, 1, &dh1, &dc1, &st1, &wn1);
        CHECK(dh0 == 0 && dh1 == 1 && st1 == TC_ST_HEATER_ON && wn1 == TC_WN_NONE);
        CHECK(st0 == (i < 17 ? TC_ST_OPERATING_CONDITION_PENDING : TC_ST_OPERATING_CONDITION_FAULT));
    }
    CHECK(dgz(1, TC_DIAG_CONTROLLER_STARTED) == 1 && dgz(0, TC_DIAG_CONTROLLER_STARTED) == 0);
    CHECK(dgz(1, TC_DIAG_OPERATING_CONDITION_REMAIN_MS) == 0 && dgz(1, TC_DIAG_RUN_PERMISSIVE) == 1 && dgz(0, TC_DIAG_RUN_PERMISSIVE) == 0);
    /* Stop / Reset / Start on one zone change nothing on the other */
    stopz(1); CHECK(ST == TC_ST_IDLE_STOPPED && dgz(0, TC_DIAG_STATUS_MIRROR) == TC_ST_OPERATING_CONDITION_FAULT);
    orstz(0); CHECK(dgz(0, TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && dgz(1, TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED);
    startz(1, 0); CHECK(dgz(1, TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_START_BLOCKED && dgz(0, TC_DIAG_STATUS_MIRROR) == TC_ST_IDLE_STOPPED && ISNAN(dgz(0, TC_DIAG_RUN_PERMISSIVE)));
    /* all 16 zones hold independent lifecycle states */
    for (i = TC_MAX_ZONES - 1; i >= 0; i--) { base(); NOW = 0; orstz(i); oinitz(i); startz(i, i % 2); }
    for (i = 0; i < TC_MAX_ZONES; i++) CHECK(dgz(i, TC_DIAG_STATUS_MIRROR) == (i % 2 ? TC_ST_TEMP_AT_SETPT : TC_ST_IDLE_START_BLOCKED));
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
    test_R10_init_reset();
    test_R10_start();                 /* needs zone 11 untouched: before the 16-zone loop of test_two_zones */
    test_R10_stop();                  /* needs zone 12 untouched */
    test_R10_permissive_loss();
    test_R10_time();
    test_R10_status_warning();
    test_two_zones();
    test_diag_readonly();
    test_R10_two_zones();
    printf("TempCtl %d.%d.%d unit tests: %d passed, %d failed\n",
           TC_VERSION_MAJOR, TC_VERSION_MINOR, TC_VERSION_PATCH, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
