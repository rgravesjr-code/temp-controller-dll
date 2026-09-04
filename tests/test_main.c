/* test_main.c - TempCtl v2 unit tests, compiled together with ../src/tempctl.c (no DLL needed). */
#include "../src/tempctl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKF(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-3)

/* ----------------------------------------------------------------------- */
static float cfg[TC_SIGNAL_COUNT];
static float out[TC_SIGNAL_COUNT];

/* Setpoint 50, bands 40..60, limits 0..100, ErrorTimeout 1000, DeadbandTimeout 500,
   filter 1 point (v1-equivalent behaviour), single sensor, no feedback. */
static void base_cfg(void)
{
    memset(cfg, 0, sizeof cfg);
    cfg[TC_SETPOINT] = 50; cfg[TC_DEADBAND_HI] = 10; cfg[TC_DEADBAND_LO] = 10;
    cfg[TC_HI_LIMIT] = 100; cfg[TC_LO_LIMIT] = 0;
    cfg[TC_ERROR_TIMEOUT] = 1000; cfg[TC_DEADBAND_TIMEOUT] = 500;
    cfg[TC_FILTER_POINTS] = 1;
    cfg[TC_TEMP1] = 50; cfg[TC_TEMP2] = 50; cfg[TC_TEMP2_TOLERANCE] = 5;
}
static int32_t st(int zone, int action, uint32_t ms, float temp1)
{
    cfg[TC_TEMP1] = temp1;
    return TcStep(zone, action, ms, cfg, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT);
}
static int32_t st2(int zone, int action, uint32_t ms, float temp1, float temp2)
{
    cfg[TC_TEMP2] = temp2;
    return st(zone, action, ms, temp1);
}
#define HEAT   out[TC_HEATING_CMD]
#define COOL   out[TC_COOLING_CMD]
#define ERR    ((unsigned)out[TC_ERROR_STATUS])
#define STATUS ((int)out[TC_TEMP_STATUS])
#define ACTIVE ((int)out[TC_ACTIVE_SENSOR])
#define CTRL   out[TC_CONTROL_TEMP]

static void test_controller_basic(void)
{
    base_cfg();
    CHECK(st(0, TC_ACTION_INIT, 0, 50) == TC_OK);
    CHECK(HEAT == 0 && COOL == 0 && ERR == 0 && STATUS == TC_STATUS_IN_BAND && ACTIVE == 1);
    CHECKF(out[TC_HI_LIMIT], 100);                       /* config echoed */
    CHECKF(out[TC_HI_BAND], 60); CHECKF(out[TC_LO_BAND], 40);
    CHECKF(CTRL, 50); CHECKF(out[TC_TEMP1_FILTERED], 50);
    CHECK(isnan(out[TC_TEMP2_FILTERED]));                /* Temp2 disabled */

    /* below LoBand: countdown, then heat */
    CHECK(st(0, TC_ACTION_STEP, 100, 30) == TC_OK);
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 500); CHECK(STATUS == TC_STATUS_HEAT_PENDING);
    CHECK(st(0, TC_ACTION_STEP, 400, 30) == TC_OK);
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 200);
    CHECK(st(0, TC_ACTION_STEP, 600, 30) == TC_OK);
    CHECK(HEAT == 1 && COOL == 0); CHECKF(out[TC_DB_REMAIN_MS], 0); CHECK(STATUS == TC_STATUS_HEATING);

    /* heating persists until >= setpoint, even while back inside the band */
    st(0, TC_ACTION_STEP, 700, 45);    CHECK(HEAT == 1);
    st(0, TC_ACTION_STEP, 800, 49.9f); CHECK(HEAT == 1);
    st(0, TC_ACTION_STEP, 900, 50.0f); CHECK(HEAT == 0 && COOL == 0 && STATUS == TC_STATUS_IN_BAND);

    /* above HiBand: countdown, then cool */
    st(0, TC_ACTION_STEP, 1000, 70); CHECK(COOL == 0 && STATUS == TC_STATUS_COOL_PENDING);
    st(0, TC_ACTION_STEP, 1499, 70); CHECK(COOL == 0);
    st(0, TC_ACTION_STEP, 1500, 70); CHECK(COOL == 1 && HEAT == 0 && STATUS == TC_STATUS_COOLING);
    st(0, TC_ACTION_STEP, 1600, 55); CHECK(COOL == 1);
    st(0, TC_ACTION_STEP, 1700, 49); CHECK(COOL == 0 && HEAT == 0);
}

static void test_deadband_offsets_follow_setpoint(void)
{
    base_cfg();
    cfg[TC_DEADBAND_HI] = 3; cfg[TC_DEADBAND_LO] = 8;
    st(1, TC_ACTION_INIT, 0, 50);
    CHECKF(out[TC_HI_BAND], 53); CHECKF(out[TC_LO_BAND], 42);
    cfg[TC_SETPOINT] = 30;                                /* live: bands move with it */
    st(1, TC_ACTION_STEP, 100, 50);
    CHECKF(out[TC_HI_BAND], 33); CHECKF(out[TC_LO_BAND], 22);
    CHECK(STATUS == TC_STATUS_COOL_PENDING);
    st(1, TC_ACTION_STEP, 600, 50); CHECK(COOL == 1);
    st(1, TC_ACTION_STEP, 700, 30); CHECK(COOL == 0);

    /* countdown resets when the condition clears before expiry */
    st(1, TC_ACTION_STEP, 800, 10);   /* below 22 */
    st(1, TC_ACTION_STEP, 1100, 25);  /* back inside */
    CHECKF(out[TC_DB_REMAIN_MS], 0);
    st(1, TC_ACTION_STEP, 1200, 10);
    st(1, TC_ACTION_STEP, 1600, 10);  CHECK(HEAT == 0);
    st(1, TC_ACTION_STEP, 1700, 10);  CHECK(HEAT == 1);
}

static void test_fault_latch_and_reset(void)
{
    base_cfg();
    st(2, TC_ACTION_INIT, 0, 50);
    st(2, TC_ACTION_STEP, 100, 120);  CHECK(ERR == 0); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    CHECK(STATUS == TC_STATUS_ERROR_PENDING);
    st(2, TC_ACTION_STEP, 900, 120);  CHECK(ERR == 0); CHECKF(out[TC_ERROR_REMAIN_MS], 200);
    st(2, TC_ACTION_STEP, 1100, 120); CHECK(ERR == TC_ERRBIT_T1_HI && HEAT == 0 && COOL == 0);
    CHECK(STATUS == TC_STATUS_STOPPED); CHECKF(out[TC_ERROR_REMAIN_MS], 0);
    st(2, TC_ACTION_STEP, 1200, 50);  CHECK(ERR == TC_ERRBIT_T1_HI);          /* latched */
    st(2, TC_ACTION_STEP, 2000, 30);  CHECK(ERR == TC_ERRBIT_T1_HI && HEAT == 0 && STATUS == TC_STATUS_STOPPED);
    CHECKF(out[TC_TEMP1_FILTERED], 30);                                      /* filters keep running while stopped */
    CHECK(st(2, TC_ACTION_RESET, 2100, 50) == TC_OK);
    CHECK(ERR == 0 && HEAT == 0 && COOL == 0 && STATUS == TC_STATUS_IN_BAND);
    st(2, TC_ACTION_STEP, 2200, 50);  CHECK(ERR == 0);

    /* low limit; countdown resets if it clears in time */
    st(2, TC_ACTION_STEP, 2300, -5);
    st(2, TC_ACTION_STEP, 3200, 10);  CHECK(ERR == 0);            /* 900 ms then cleared */
    st(2, TC_ACTION_STEP, 3300, -5);
    st(2, TC_ACTION_STEP, 4200, -5);  CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 4300, -5);  CHECK(ERR == TC_ERRBIT_T1_LO);
    /* reset with the condition still present: counts again from full,
       starting at the first STEP that sees it */
    st(2, TC_ACTION_RESET, 4400, -5); CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 4500, -5);  CHECK(ERR == 0); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    st(2, TC_ACTION_STEP, 5400, -5);  CHECK(ERR == 0);
    st(2, TC_ACTION_STEP, 5500, -5);  CHECK(ERR == TC_ERRBIT_T1_LO);
}

static void test_fault_while_heating(void)
{
    base_cfg();
    st(3, TC_ACTION_INIT, 0, 50);
    st(3, TC_ACTION_STEP, 100, 30);
    st(3, TC_ACTION_STEP, 600, 30);  CHECK(HEAT == 1);
    /* heater failing: temp keeps falling through LoLimit while heating */
    st(3, TC_ACTION_STEP, 700, -5);  CHECK(HEAT == 1 && ERR == 0 && STATUS == TC_STATUS_ERROR_PENDING);
    st(3, TC_ACTION_STEP, 1600, -5); CHECK(HEAT == 1 && ERR == 0);
    st(3, TC_ACTION_STEP, 1700, -5); CHECK(ERR == TC_ERRBIT_T1_LO && HEAT == 0);
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
    CHECKF(out[TC_TEMP1_FILTERED], 30);                                     /* NaN not averaged */
    st(4, TC_ACTION_STEP, 800, 45);  CHECK(ERR == 0 && HEAT == 0);              /* recovered, idle */
    st(4, TC_ACTION_STEP, 900, INFINITY);
    st(4, TC_ACTION_STEP, 1800, INFINITY); CHECK(ERR == 0);
    st(4, TC_ACTION_STEP, 1900, INFINITY); CHECK(ERR == TC_ERRBIT_T1_BAD && STATUS == TC_STATUS_STOPPED);
    st(4, TC_ACTION_STEP, 2000, 50); CHECK(ERR == TC_ERRBIT_T1_BAD);

    /* never a valid sample: filtered NaN counts as a bad reading */
    base_cfg();
    st(5, TC_ACTION_INIT, 0, NAN);
    CHECK(isnan(out[TC_TEMP1_FILTERED]) && isnan(CTRL));
    st(5, TC_ACTION_STEP, 100, NAN); CHECK(STATUS == TC_STATUS_ERROR_PENDING);
    st(5, TC_ACTION_STEP, 1100, NAN); CHECK(ERR == TC_ERRBIT_T1_BAD);
}

static void test_init_state_and_live_setpoint(void)
{
    base_cfg();
    cfg[TC_HEATING_CMD] = 1;
    CHECK(st(6, TC_ACTION_INIT, 0, 30) == TC_OK);
    CHECK(HEAT == 1 && COOL == 0 && STATUS == TC_STATUS_HEATING);
    cfg[TC_HEATING_CMD] = 0;                 /* input relay fields are ignored on STEP */
    st(6, TC_ACTION_STEP, 100, 45); CHECK(HEAT == 1);
    cfg[TC_SETPOINT] = 45;                   /* live setpoint change */
    st(6, TC_ACTION_STEP, 200, 45); CHECK(HEAT == 0);

    cfg[TC_HEATING_CMD] = 1; cfg[TC_COOLING_CMD] = 1;   /* heat wins if both set */
    st(7, TC_ACTION_INIT, 0, 50); CHECK(HEAT == 1 && COOL == 0);
    cfg[TC_HEATING_CMD] = 0;
    st(7, TC_ACTION_INIT, 0, 50); CHECK(HEAT == 0 && COOL == 1);
    cfg[TC_COOLING_CMD] = 0;
}

static void test_zero_timeouts_and_wrap(void)
{
    base_cfg();
    cfg[TC_ERROR_TIMEOUT] = 0; cfg[TC_DEADBAND_TIMEOUT] = 0;
    st(8, TC_ACTION_INIT, 0, 50);
    st(8, TC_ACTION_STEP, 10, 30);  CHECK(HEAT == 1);             /* zero timeout: immediate */
    st(8, TC_ACTION_STEP, 20, 120); CHECK(ERR == TC_ERRBIT_T1_HI && HEAT == 0);

    base_cfg();
    st(9, TC_ACTION_INIT, 0xFFFFFF00u, 50);
    st(9, TC_ACTION_STEP, 0xFFFFFF80u, 30);
    st(9, TC_ACTION_STEP, 0x00000100u, 30);                   /* dt = 384 across wrap */
    CHECK(HEAT == 0); CHECKF(out[TC_DB_REMAIN_MS], 116);
    st(9, TC_ACTION_STEP, 0x00000180u, 30);
    CHECK(HEAT == 1);
}

static void test_errors_config_and_aliasing(void)
{
    base_cfg();
    CHECK(TcVersion() == 0x020000);
    CHECK(TcInputCount() == 17 && TcSignalCount() == 27);
    CHECK(TcStep(10, TC_ACTION_STEP, 0, cfg, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_NOT_INIT);
    CHECK(TcStep(-1, TC_ACTION_INIT, 0, cfg, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ZONE);
    CHECK(TcStep(TC_MAX_ZONES, TC_ACTION_INIT, 0, cfg, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ZONE);
    CHECK(TcStep(0, 99, 0, cfg, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ACTION);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, cfg, 16, out, TC_SIGNAL_COUNT) == TC_ERR_ARG);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, cfg, TC_INPUT_COUNT, out, 26) == TC_ERR_ARG);
    CHECK(TcStep(0, TC_ACTION_INIT, 0, NULL, TC_INPUT_COUNT, out, TC_SIGNAL_COUNT) == TC_ERR_ARG);

    /* config warnings: still runs, TC_ERRBIT_CONFIG follows the config */
    cfg[TC_DEADBAND_HI] = 60;                                 /* HiBand 110 >= HiLimit */
    CHECK(st(10, TC_ACTION_INIT, 0, 50) == TC_WARN_CONFIG);
    CHECK(ERR == TC_ERRBIT_CONFIG);
    CHECK(st(10, TC_ACTION_STEP, 100, 50) == TC_WARN_CONFIG);  /* reported every tick */
    cfg[TC_DEADBAND_HI] = 10;
    CHECK(st(10, TC_ACTION_STEP, 200, 50) == TC_OK && ERR == 0);
    cfg[TC_DEADBAND_LO] = -1;  CHECK(st(10, TC_ACTION_STEP, 300, 50) == TC_WARN_CONFIG); cfg[TC_DEADBAND_LO] = 10;
    cfg[TC_ERROR_TIMEOUT] = -1; CHECK(st(10, TC_ACTION_STEP, 400, 50) == TC_WARN_CONFIG); cfg[TC_ERROR_TIMEOUT] = 1000;
    cfg[TC_TEMP2_TOLERANCE] = -1; CHECK(st(10, TC_ACTION_STEP, 500, 50) == TC_WARN_CONFIG); cfg[TC_TEMP2_TOLERANCE] = 5;
    cfg[TC_FILTER_POINTS] = 100; CHECK(st(10, TC_ACTION_STEP, 600, 50) == TC_WARN_CONFIG);   /* clamped to 64 */
    cfg[TC_FILTER_POINTS] = 0;   CHECK(st(10, TC_ACTION_STEP, 700, 50) == TC_OK);            /* 0 = default 4 */
    cfg[TC_FILTER_POINTS] = 1;

    /* in-place: in == out, one 27-element array */
    base_cfg();
    float io[TC_SIGNAL_COUNT]; memcpy(io, cfg, sizeof io);
    CHECK(TcStep(11, TC_ACTION_INIT, 0, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    io[TC_TEMP1] = 30;
    CHECK(TcStep(11, TC_ACTION_STEP, 100, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    CHECK(TcStep(11, TC_ACTION_STEP, 600, io, TC_SIGNAL_COUNT, io, TC_SIGNAL_COUNT) == TC_OK);
    CHECK(io[TC_HEATING_CMD] == 1 && io[TC_TEMP1] == 30 && io[TC_HI_LIMIT] == 100 && io[TC_CONTROL_TEMP] == 30);
}

/* ----------------------------------------------------------------------- */
static void test_filter(void)
{
    base_cfg();
    cfg[TC_FILTER_POINTS] = 4;
    st(12, TC_ACTION_INIT, 0, 10);                        /* Init contributes the first sample */
    CHECKF(out[TC_TEMP1_FILTERED], 10); CHECK(STATUS == TC_STATUS_WARMUP);
    st(12, TC_ACTION_STEP, 100, 20); CHECKF(CTRL, 15); CHECK(STATUS == TC_STATUS_WARMUP);
    st(12, TC_ACTION_STEP, 200, 30); CHECKF(CTRL, 20);
    st(12, TC_ACTION_STEP, 300, 40); CHECKF(CTRL, 25); CHECK(STATUS != TC_STATUS_WARMUP);
    st(12, TC_ACTION_STEP, 400, 50); CHECKF(CTRL, 35);   /* window 20 30 40 50 */
    st(12, TC_ACTION_STEP, 500, NAN); CHECKF(CTRL, 35);  /* bad sample skipped, relays untouched (idle anyway) */
    cfg[TC_FILTER_POINTS] = 2;                            /* live change of the window */
    st(12, TC_ACTION_STEP, 600, 50); CHECKF(CTRL, 50);   /* 50 50 */
    cfg[TC_FILTER_POINTS] = 64;
    st(12, TC_ACTION_STEP, 700, 50); CHECK(STATUS == TC_STATUS_WARMUP);
    CHECKF(CTRL, (10 + 20 + 30 + 40 + 50 + 50 + 50) / 7.0);
    for (int i = 0; i < 60; i++) st(12, TC_ACTION_STEP, 800 + 100 * i, 50);
    CHECK(STATUS != TC_STATUS_WARMUP);
    CHECKF(CTRL, (40 + 50 * 63) / 64.0);                 /* 67 samples, ring of 64 wrapped once */

    /* control acts on the filtered value, not the raw one */
    base_cfg();
    cfg[TC_FILTER_POINTS] = 4; cfg[TC_DEADBAND_TIMEOUT] = 0; cfg[TC_DEADBAND_LO] = 20;   /* LoBand 30 */
    st(13, TC_ACTION_INIT, 0, 50);
    st(13, TC_ACTION_STEP, 100, 50);
    st(13, TC_ACTION_STEP, 200, 50);
    st(13, TC_ACTION_STEP, 300, 0);   CHECKF(CTRL, 37.5); CHECK(HEAT == 0);   /* raw 0 < LoBand, filtered inside */
    st(13, TC_ACTION_STEP, 400, 0);   CHECKF(CTRL, 25);   CHECK(HEAT == 1);
}

static void test_failover(void)
{
    base_cfg();
    cfg[TC_TEMP2_ENABLE] = 1;
    st2(14, TC_ACTION_INIT, 0, 50, 51);
    CHECK(ACTIVE == 1 && STATUS == TC_STATUS_IN_BAND); CHECKF(out[TC_TEMP2_FILTERED], 51);
    /* sensor 1 runs away above HiLimit; sensor 2 stays sane */
    st2(14, TC_ACTION_STEP, 100, 150, 51);  CHECK(ERR == 0 && ACTIVE == 1 && STATUS == TC_STATUS_ERROR_PENDING);
    st2(14, TC_ACTION_STEP, 1000, 150, 51); CHECK(ERR == 0 && ACTIVE == 1);
    st2(14, TC_ACTION_STEP, 1100, 150, 51);
    /* cooling had engaged on the runaway sensor 1; it now continues on sensor 2 (51 > Setpoint) */
    CHECK(ERR == TC_ERRBIT_T1_HI && ACTIVE == 2 && STATUS == TC_STATUS_DEGRADED && HEAT == 0 && COOL == 1);
    CHECKF(CTRL, 51);
    /* control now follows sensor 2: cooling ends at <= Setpoint, then heat pending */
    st2(14, TC_ACTION_STEP, 1200, 150, 30); CHECK(HEAT == 0 && COOL == 0);
    st2(14, TC_ACTION_STEP, 1700, 150, 30); CHECK(HEAT == 1 && ACTIVE == 2);
    st2(14, TC_ACTION_STEP, 1800, 150, 50); CHECK(HEAT == 0);
    /* sensor 1 back in limits: stays failed (latched) */
    st2(14, TC_ACTION_STEP, 1900, 50, 50);  CHECK(ERR == TC_ERRBIT_T1_HI && ACTIVE == 2);
    /* sensor 2 fails too -> stopped */
    st2(14, TC_ACTION_STEP, 2000, 50, NAN); CHECK(HEAT == 0 && STATUS == TC_STATUS_ERROR_PENDING);
    st2(14, TC_ACTION_STEP, 2900, 50, NAN); CHECK(STATUS == TC_STATUS_ERROR_PENDING);
    st2(14, TC_ACTION_STEP, 3000, 50, NAN);                 /* 1000 ms after first observation */
    CHECK(ERR == (TC_ERRBIT_T1_HI | TC_ERRBIT_T2_BAD) && STATUS == TC_STATUS_STOPPED);
    st2(14, TC_ACTION_STEP, 3200, 50, 50);  CHECK(STATUS == TC_STATUS_STOPPED);
    /* Reset: both sensors trusted again, sensor 1 active */
    st2(14, TC_ACTION_RESET, 3300, 50, 50); CHECK(ERR == 0 && ACTIVE == 1 && STATUS == TC_STATUS_IN_BAND);

    /* backup sensor fails while the primary is fine: degraded, active stays 1 */
    st2(14, TC_ACTION_STEP, 3400, 50, -20);
    st2(14, TC_ACTION_STEP, 4400, 50, -20); CHECK(ERR == TC_ERRBIT_T2_LO && ACTIVE == 1 && STATUS == TC_STATUS_DEGRADED);
    st2(14, TC_ACTION_STEP, 4500, 30, -20); CHECK(STATUS == TC_STATUS_DEGRADED);   /* degraded shown over heat pending */
    st2(14, TC_ACTION_STEP, 5000, 30, -20); CHECK(HEAT == 1 && STATUS == TC_STATUS_DEGRADED);
    /* then the primary fails: nothing left -> stopped */
    st2(14, TC_ACTION_STEP, 5100, NAN, -20); CHECK(HEAT == 0);
    st2(14, TC_ACTION_STEP, 6100, NAN, -20); CHECK(STATUS == TC_STATUS_STOPPED && ERR == (TC_ERRBIT_T2_LO | TC_ERRBIT_T1_BAD));

    /* Temp2 disabled: sensor 2 is ignored entirely (NaN, out of limits) */
    base_cfg();
    st2(15, TC_ACTION_INIT, 0, 50, NAN);
    st2(15, TC_ACTION_STEP, 2000, 50, 500); CHECK(ERR == 0 && STATUS == TC_STATUS_IN_BAND);
    CHECK(isnan(out[TC_TEMP2_FILTERED]));
    /* disabling Temp2 while running on it falls back to sensor 1 */
    cfg[TC_TEMP2_ENABLE] = 1;
    st2(15, TC_ACTION_RESET, 2100, 150, 50);
    st2(15, TC_ACTION_STEP, 2200, 150, 50);
    st2(15, TC_ACTION_STEP, 3200, 150, 50); CHECK(ACTIVE == 2);
    cfg[TC_TEMP2_ENABLE] = 0;
    st2(15, TC_ACTION_STEP, 3300, 150, 50); CHECK(ACTIVE == 1 && STATUS == TC_STATUS_STOPPED);
}

static void test_disagreement(void)
{
    base_cfg();
    cfg[TC_TEMP2_ENABLE] = 1; cfg[TC_TEMP2_TOLERANCE] = 5;
    st2(0, TC_ACTION_INIT, 0, 50, 54);
    st2(0, TC_ACTION_STEP, 100, 50, 54);  CHECK(ERR == 0 && STATUS == TC_STATUS_IN_BAND);   /* within tolerance */
    st2(0, TC_ACTION_STEP, 200, 50, 56);  CHECK(STATUS == TC_STATUS_ERROR_PENDING); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    st2(0, TC_ACTION_STEP, 700, 50, 54);  CHECK(STATUS == TC_STATUS_IN_BAND);               /* cleared in time */
    st2(0, TC_ACTION_STEP, 800, 50, 60);
    st2(0, TC_ACTION_STEP, 1700, 50, 60); CHECK(ERR == 0);
    st2(0, TC_ACTION_STEP, 1800, 50, 60); CHECK(ERR == TC_ERRBIT_DISAGREE && STATUS == TC_STATUS_IN_BAND && ACTIVE == 1);
    /* keeps running on sensor 1 */
    st2(0, TC_ACTION_STEP, 1900, 30, 60);
    st2(0, TC_ACTION_STEP, 2400, 30, 60); CHECK(HEAT == 1 && ERR == TC_ERRBIT_DISAGREE);
    /* a later limit failure of sensor 1 still fails over */
    st2(0, TC_ACTION_STEP, 2500, 150, 60);
    st2(0, TC_ACTION_STEP, 3500, 150, 60); CHECK(ACTIVE == 2 && ERR == (TC_ERRBIT_DISAGREE | TC_ERRBIT_T1_HI));
    st2(0, TC_ACTION_RESET, 3600, 50, 50); CHECK(ERR == 0);
}

static void test_feedback(void)
{
    base_cfg();
    cfg[TC_FEEDBACK_ENABLE] = 1;
    st(1, TC_ACTION_INIT, 0, 50);
    st(1, TC_ACTION_STEP, 100, 50);  CHECK(ERR == 0);            /* off commanded, off measured */
    st(1, TC_ACTION_STEP, 200, 30);
    st(1, TC_ACTION_STEP, 700, 30);  CHECK(HEAT == 1 && ERR == 0);   /* command issued now */
    /* feedback still 0 on the next call: mismatch countdown starts */
    st(1, TC_ACTION_STEP, 800, 30);  CHECK(STATUS == TC_STATUS_ERROR_PENDING); CHECKF(out[TC_ERROR_REMAIN_MS], 1000);
    cfg[TC_HEATER_FEEDBACK] = 1;
    st(1, TC_ACTION_STEP, 900, 30);  CHECK(STATUS == TC_STATUS_HEATING && ERR == 0);   /* relay answered in time */
    cfg[TC_HEATER_FEEDBACK] = 0;                                  /* relay drops out while commanded */
    st(1, TC_ACTION_STEP, 1000, 30);
    st(1, TC_ACTION_STEP, 1900, 30); CHECK(ERR == 0 && HEAT == 1);
    st(1, TC_ACTION_STEP, 2000, 30); CHECK(ERR == TC_ERRBIT_HEATER_FB && HEAT == 1);   /* set, operation continues */
    CHECK(STATUS == TC_STATUS_HEATING);
    cfg[TC_HEATER_FEEDBACK] = 1;
    st(1, TC_ACTION_STEP, 2100, 30); CHECK(ERR == TC_ERRBIT_HEATER_FB);                 /* latched */
    st(1, TC_ACTION_STEP, 2200, 50); CHECK(HEAT == 0);
    /* heater feedback stuck on after the command went off: still latched, no new info */
    st(1, TC_ACTION_STEP, 2300, 50);
    /* cooler: feedback 1 while commanded off */
    cfg[TC_HEATER_FEEDBACK] = 0; cfg[TC_COOLER_FEEDBACK] = 1;
    st(1, TC_ACTION_STEP, 2400, 50);
    st(1, TC_ACTION_STEP, 3400, 50); CHECK(ERR == (TC_ERRBIT_HEATER_FB | TC_ERRBIT_COOLER_FB) && COOL == 0);
    st(1, TC_ACTION_RESET, 3500, 50); CHECK(ERR == 0);
    cfg[TC_COOLER_FEEDBACK] = 0;

    /* FeedbackEnable = 0: inputs ignored */
    cfg[TC_FEEDBACK_ENABLE] = 0; cfg[TC_HEATER_FEEDBACK] = 1;
    st(1, TC_ACTION_STEP, 3600, 50);
    st(1, TC_ACTION_STEP, 5000, 50); CHECK(ERR == 0);
    cfg[TC_HEATER_FEEDBACK] = 0;

    /* initial relay state from Init counts as the previous command */
    base_cfg();
    cfg[TC_FEEDBACK_ENABLE] = 1; cfg[TC_HEATING_CMD] = 1; cfg[TC_HEATER_FEEDBACK] = 1;
    st(2, TC_ACTION_INIT, 0, 30);
    st(2, TC_ACTION_STEP, 100, 30); CHECK(ERR == 0 && HEAT == 1 && STATUS == TC_STATUS_HEATING);
    cfg[TC_HEATING_CMD] = 0; cfg[TC_HEATER_FEEDBACK] = 0;
}

static void test_error_remain_is_minimum(void)
{
    base_cfg();
    cfg[TC_TEMP2_ENABLE] = 1; cfg[TC_FEEDBACK_ENABLE] = 1;
    st2(3, TC_ACTION_INIT, 0, 50, 50);
    st2(3, TC_ACTION_STEP, 100, 50, 150);   CHECKF(out[TC_ERROR_REMAIN_MS], 1000);   /* T2 hi started */
    st2(3, TC_ACTION_STEP, 400, 50, 150);   CHECKF(out[TC_ERROR_REMAIN_MS], 700);
    cfg[TC_COOLER_FEEDBACK] = 1;                                                       /* cooler fb mismatch starts */
    st2(3, TC_ACTION_STEP, 500, 50, 150);   CHECKF(out[TC_ERROR_REMAIN_MS], 600);    /* min(600, 1000) */
    st2(3, TC_ACTION_STEP, 1100, 50, 150);  CHECK(ERR == TC_ERRBIT_T2_HI); CHECKF(out[TC_ERROR_REMAIN_MS], 400);
    st2(3, TC_ACTION_STEP, 1500, 50, 150);  CHECK(ERR == (TC_ERRBIT_T2_HI | TC_ERRBIT_COOLER_FB));
    CHECKF(out[TC_ERROR_REMAIN_MS], 0);
    CHECK(STATUS == TC_STATUS_DEGRADED);
    cfg[TC_COOLER_FEEDBACK] = 0;
}

int main(void)
{
    test_controller_basic();
    test_deadband_offsets_follow_setpoint();
    test_fault_latch_and_reset();
    test_fault_while_heating();
    test_bad_reading();
    test_init_state_and_live_setpoint();
    test_zero_timeouts_and_wrap();
    test_errors_config_and_aliasing();
    test_filter();
    test_failover();
    test_disagreement();
    test_feedback();
    test_error_remain_is_minimum();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
