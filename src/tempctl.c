/* tempctl.c - temperature controller state machine. See tempctl.h. */
#include "tempctl.h"
#include <string.h>

enum { MODE_IDLE = 0, MODE_HEAT = 1, MODE_COOL = 2 };

typedef struct {
    int      inited;
    int      mode;        /* MODE_* */
    int      fault;       /* 0 none, 1 hi, 2 lo, 3 bad reading (latched) */
    int      errCond;     /* condition currently being timed, 0 none */
    float    errRemainMs;
    int      dbCond;      /* 0 none, 1 above HiDeadband, 2 below LoDeadband */
    float    dbRemainMs;
    uint32_t lastMs;
} TcZone;

static TcZone g_zones[TC_MAX_ZONES];

TC_API uint32_t TcVersion(void)
{
    return ((uint32_t)TC_VERSION_MAJOR << 16) | ((uint32_t)TC_VERSION_MINOR << 8) | TC_VERSION_PATCH;
}

/* isnan/isinf without <math.h>: NaN != NaN; Inf is out of float range. */
static int bad_reading(float t)
{
    if (t != t) return 1;
    if (t > 3.4028235e38f || t < -3.4028235e38f) return 1;
    return 0;
}

static void clear_timers(TcZone* z)
{
    z->errCond = 0; z->errRemainMs = 0.0f;
    z->dbCond = 0;  z->dbRemainMs = 0.0f;
}

static void write_out(const TcZone* z, const float* in, float* out, int32_t outLen)
{
    float tmp[TC_SIGNAL_COUNT];
    memcpy(tmp, in, sizeof tmp);              /* in and out may alias */
    tmp[TC_COOLING_ACTIVE] = (z->mode == MODE_COOL) ? 1.0f : 0.0f;
    tmp[TC_HEATING_ACTIVE] = (z->mode == MODE_HEAT) ? 1.0f : 0.0f;
    tmp[TC_ERROR_STATUS]   = (float)z->fault;
    memcpy(out, tmp, sizeof tmp);
    if (outLen >= TC_SIGNAL_COUNT_EXT) {
        out[TC_ERROR_REMAIN_MS] = (z->errCond != 0 && z->fault == 0) ? z->errRemainMs : 0.0f;
        out[TC_DB_REMAIN_MS]    = (z->dbCond  != 0) ? z->dbRemainMs : 0.0f;
    }
}

static int32_t config_ok(const float* in)
{
    float lo = in[TC_LO_LIMIT], lodb = in[TC_LO_DEADBAND], sp = in[TC_SETPOINT];
    float hidb = in[TC_HI_DEADBAND], hi = in[TC_HI_LIMIT];
    if (!(lo < lodb && lodb <= sp && sp <= hidb && hidb < hi)) return TC_WARN_CONFIG;
    if (in[TC_ERROR_TIMEOUT] < 0.0f || in[TC_DEADBAND_TIMEOUT] < 0.0f) return TC_WARN_CONFIG;
    return TC_OK;
}

static void step(TcZone* z, const float* in, uint32_t nowMs)
{
    float dt = (float)(uint32_t)(nowMs - z->lastMs);   /* wrap-safe */
    z->lastMs = nowMs;

    if (z->fault) return;                    /* latched until reset/init */

    float t    = in[TC_ACTUAL_TEMP];
    int   bad  = bad_reading(t);
    int   cond = bad ? 3 : (t > in[TC_HI_LIMIT]) ? 1 : (t < in[TC_LO_LIMIT]) ? 2 : 0;

    /* --- error countdown ------------------------------------------------ */
    if (cond != z->errCond) {                /* countdown starts at first observation */
        z->errCond = cond;
        z->errRemainMs = in[TC_ERROR_TIMEOUT];
        dt = 0.0f;
    }
    if (cond != 0) {
        z->errRemainMs -= dt;
        if (z->errRemainMs <= 0.0f) {
            z->fault = cond;
            z->mode = MODE_IDLE;
            z->errRemainMs = 0.0f;
            z->dbCond = 0; z->dbRemainMs = 0.0f;
            return;
        }
    }

    if (bad) {                               /* relays off while the reading is unusable */
        z->mode = MODE_IDLE;
        z->dbCond = 0; z->dbRemainMs = 0.0f;
        return;
    }

    /* --- heating / cooling ---------------------------------------------- */
    float sp = in[TC_SETPOINT];
    if (z->mode == MODE_HEAT && t >= sp) z->mode = MODE_IDLE;
    else if (z->mode == MODE_COOL && t <= sp) z->mode = MODE_IDLE;

    if (z->mode != MODE_IDLE) {              /* running: deadband timer is not in play */
        z->dbCond = 0; z->dbRemainMs = 0.0f;
        return;
    }

    int dbc = (t > in[TC_HI_DEADBAND]) ? 1 : (t < in[TC_LO_DEADBAND]) ? 2 : 0;
    if (dbc != z->dbCond) {
        z->dbCond = dbc;
        z->dbRemainMs = in[TC_DEADBAND_TIMEOUT];
        dt = 0.0f;
    }
    if (dbc != 0) {
        z->dbRemainMs -= dt;
        if (z->dbRemainMs <= 0.0f) {
            z->mode = (dbc == 1) ? MODE_COOL : MODE_HEAT;
            z->dbCond = 0; z->dbRemainMs = 0.0f;
        }
    }
}

TC_API int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
                      const float* in, int32_t inLen,
                      float* out, int32_t outLen)
{
    if (!in || !out || inLen < TC_SIGNAL_COUNT || outLen < TC_SIGNAL_COUNT) return TC_ERR_ARG;
    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    TcZone* z = &g_zones[zone];
    int32_t rc = TC_OK;

    switch (action) {
    case TC_ACTION_INIT:
        memset(z, 0, sizeof *z);
        z->inited = 1;
        z->lastMs = nowMs;
        if (in[TC_HEATING_ACTIVE] > 0.5f)      z->mode = MODE_HEAT;
        else if (in[TC_COOLING_ACTIVE] > 0.5f) z->mode = MODE_COOL;
        rc = config_ok(in);
        break;
    case TC_ACTION_RESET:
        z->inited = 1;
        z->fault = 0;
        z->mode = MODE_IDLE;
        z->lastMs = nowMs;
        clear_timers(z);
        break;
    case TC_ACTION_STEP:
        if (!z->inited) return TC_ERR_NOT_INIT;
        step(z, in, nowMs);
        rc = config_ok(in);                  /* live config is validated every tick */
        break;
    default:
        return TC_ERR_ACTION;
    }
    write_out(z, in, out, outLen);
    return rc;
}
