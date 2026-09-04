/* tempctl.c - TempCtl v2 controller state machine. The spec is tempctl.h. */
#include "tempctl.h"
#include <string.h>

enum { MODE_IDLE = 0, MODE_HEAT = 1, MODE_COOL = 2 };
enum { COND_NONE = 0, COND_HI = 1, COND_LO = 2, COND_BAD = 3 };

typedef struct {
    float buf[TC_MAX_FILTER];
    int   head;                 /* next write position                    */
    int   count;                /* valid samples stored, <= TC_MAX_FILTER */
} TcFilter;

typedef struct {
    int      inited;
    int      mode;              /* MODE_*                                  */
    int      stopped;           /* latched fault: relays off until Reset   */
    uint32_t errBits;           /* latched TC_ERRBIT_* (bits 0..8)         */
    int      active;            /* 1 or 2                                  */
    TcFilter filt[2];
    float    filtered[2];       /* NaN until a valid sample exists         */
    int      warmup[2];
    int      sensCond[2];  float sensRemain[2];
    int      disCond;      float disRemain;
    int      fbCond[2];    float fbRemain[2];    /* heater, cooler          */
    int      dbCond;       float dbRemain;
    int      prevHeat, prevCool;                 /* commands of the last call */
    uint32_t lastMs;
} TcZone;

static TcZone g_zones[TC_MAX_ZONES];

TC_API uint32_t TcVersion(void)
{
    return ((uint32_t)TC_VERSION_MAJOR << 16) | ((uint32_t)TC_VERSION_MINOR << 8) | TC_VERSION_PATCH;
}
TC_API int32_t TcInputCount(void)  { return TC_INPUT_COUNT; }
TC_API int32_t TcSignalCount(void) { return TC_SIGNAL_COUNT; }

/* NaN / isnan / isinf without <math.h> (keeps the .so on libc only). */
static float quiet_nan(void)
{
    uint32_t b = 0x7FC00000u; float f; memcpy(&f, &b, 4); return f;
}
static int bad_reading(float t)
{
    if (t != t) return 1;
    if (t > 3.4028235e38f || t < -3.4028235e38f) return 1;
    return 0;
}
static int is_nan(float t) { return t != t; }

/* ---- moving average ---------------------------------------------------- */
static void filter_push(TcFilter* f, float v)
{
    f->buf[f->head] = v;
    f->head = (f->head + 1) % TC_MAX_FILTER;
    if (f->count < TC_MAX_FILTER) f->count++;
}
static float filter_value(const TcFilter* f, int n)
{
    int use = (f->count < n) ? f->count : n;
    if (use <= 0) return quiet_nan();
    double sum = 0.0;
    int idx = f->head;
    for (int i = 0; i < use; i++) {
        idx = (idx + TC_MAX_FILTER - 1) % TC_MAX_FILTER;
        sum += f->buf[idx];
    }
    return (float)(sum / use);
}

/* ---- countdown: returns 1 when it expires this tick ---------------------- */
static int countdown(int* cond, float* remain, int newCond, float timeout, float dt)
{
    if (newCond != *cond) {                 /* starts at first observation */
        *cond = newCond;
        *remain = timeout;
        dt = 0.0f;
    }
    if (newCond == COND_NONE) { *remain = 0.0f; return 0; }
    *remain -= dt;
    if (*remain <= 0.0f) { *remain = 0.0f; return 1; }
    return 0;
}
static void clear_cd(int* cond, float* remain) { *cond = COND_NONE; *remain = 0.0f; }
static void clear_timers(TcZone* z)
{
    clear_cd(&z->sensCond[0], &z->sensRemain[0]);
    clear_cd(&z->sensCond[1], &z->sensRemain[1]);
    clear_cd(&z->disCond, &z->disRemain);
    clear_cd(&z->fbCond[0], &z->fbRemain[0]);
    clear_cd(&z->fbCond[1], &z->fbRemain[1]);
    clear_cd(&z->dbCond, &z->dbRemain);
}

/* ---- configuration ------------------------------------------------------ */
typedef struct {
    float sp, dbHi, dbLo, hi, lo, errTO, dbTO, tol, hiBand, loBand;
    int   nFilt, t2en, fbEn, warn;
} TcCfg;

static void read_cfg(const float* in, TcCfg* c)
{
    c->sp = in[TC_SETPOINT]; c->dbHi = in[TC_DEADBAND_HI]; c->dbLo = in[TC_DEADBAND_LO];
    c->hi = in[TC_HI_LIMIT]; c->lo = in[TC_LO_LIMIT];
    c->errTO = in[TC_ERROR_TIMEOUT]; c->dbTO = in[TC_DEADBAND_TIMEOUT];
    c->tol = in[TC_TEMP2_TOLERANCE];
    c->t2en = in[TC_TEMP2_ENABLE] > 0.5f;
    c->fbEn = in[TC_FEEDBACK_ENABLE] > 0.5f;
    c->hiBand = c->sp + c->dbHi;
    c->loBand = c->sp - c->dbLo;
    c->warn = 0;

    float fp = in[TC_FILTER_POINTS];
    if (is_nan(fp) || fp < 0.0f) { c->nFilt = TC_DEFAULT_FILTER; c->warn = 1; }
    else if (fp < 1.0f)          { c->nFilt = TC_DEFAULT_FILTER; }          /* 0 = default */
    else if (fp > (float)TC_MAX_FILTER) { c->nFilt = TC_MAX_FILTER; c->warn = 1; }
    else                         { c->nFilt = (int)fp; }

    if (!(c->dbHi >= 0.0f && c->dbLo >= 0.0f)) c->warn = 1;
    if (!(c->lo < c->loBand && c->hiBand < c->hi)) c->warn = 1;
    if (!(c->errTO >= 0.0f && c->dbTO >= 0.0f)) c->warn = 1;
    if (!(c->tol >= 0.0f)) c->warn = 1;
}

static void update_filters(TcZone* z, const float* in, const TcCfg* c)
{
    for (int s = 0; s < 2; s++) {
        float raw = in[TC_TEMP1 + s];
        if (!bad_reading(raw)) filter_push(&z->filt[s], raw);
        z->filtered[s] = filter_value(&z->filt[s], c->nFilt);
        z->warmup[s] = z->filt[s].count < c->nFilt;
    }
}

static uint32_t sensor_bit(int s, int cond)
{
    uint32_t b = (cond == COND_HI) ? TC_ERRBIT_T1_HI : (cond == COND_LO) ? TC_ERRBIT_T1_LO : TC_ERRBIT_T1_BAD;
    return s == 0 ? b : (b << 3);
}

static void stop(TcZone* z)
{
    z->stopped = 1;
    z->mode = MODE_IDLE;
    z->prevHeat = z->prevCool = 0;
    clear_timers(z);
}

/* ---- one control tick --------------------------------------------------- */
static void step(TcZone* z, const float* in, const TcCfg* c, uint32_t nowMs)
{
    float dt = (float)(uint32_t)(nowMs - z->lastMs);   /* wrap-safe */
    z->lastMs = nowMs;

    update_filters(z, in, c);
    if (z->stopped) return;                              /* latched until Reset/Init */

    /* --- sensor rationality ---------------------------------------------- */
    int nSens = c->t2en ? 2 : 1;
    for (int s = 0; s < 2; s++) {
        uint32_t mask = s == 0 ? TC_ERRBIT_SENSOR1 : TC_ERRBIT_SENSOR2;
        if (s >= nSens || (z->errBits & mask)) { clear_cd(&z->sensCond[s], &z->sensRemain[s]); continue; }
        float raw = in[TC_TEMP1 + s], f = z->filtered[s];
        int cond = (bad_reading(raw) || is_nan(f)) ? COND_BAD : (f > c->hi) ? COND_HI : (f < c->lo) ? COND_LO : COND_NONE;
        if (countdown(&z->sensCond[s], &z->sensRemain[s], cond, c->errTO, dt)) {
            z->errBits |= sensor_bit(s, cond);
            clear_cd(&z->sensCond[s], &z->sensRemain[s]);
        }
    }
    int failed1 = (z->errBits & TC_ERRBIT_SENSOR1) != 0;
    int failed2 = c->t2en && (z->errBits & TC_ERRBIT_SENSOR2) != 0;

    /* --- failover / stop ------------------------------------------------- */
    if (c->t2en) {
        if (z->active == 1 && failed1 && !failed2) z->active = 2;
        else if (z->active == 2 && failed2 && !failed1) z->active = 1;
        if (failed1 && failed2) { stop(z); return; }
    } else {
        z->active = 1;
        if (failed1) { stop(z); return; }
    }

    /* --- disagreement ---------------------------------------------------- */
    if (c->t2en && !failed1 && !failed2 && !(z->errBits & TC_ERRBIT_DISAGREE)
        && !is_nan(z->filtered[0]) && !is_nan(z->filtered[1])) {
        float d = z->filtered[0] - z->filtered[1];
        if (d < 0.0f) d = -d;
        int cond = d > c->tol ? COND_HI : COND_NONE;
        if (countdown(&z->disCond, &z->disRemain, cond, c->errTO, dt)) {
            z->errBits |= TC_ERRBIT_DISAGREE;
            clear_cd(&z->disCond, &z->disRemain);
        }
    } else {
        clear_cd(&z->disCond, &z->disRemain);
    }

    /* --- relay feedback (against the commands of the previous call) ----- */
    for (int r = 0; r < 2; r++) {
        uint32_t bit = r == 0 ? TC_ERRBIT_HEATER_FB : TC_ERRBIT_COOLER_FB;
        if (!c->fbEn || (z->errBits & bit)) { clear_cd(&z->fbCond[r], &z->fbRemain[r]); continue; }
        int fb  = in[r == 0 ? TC_HEATER_FEEDBACK : TC_COOLER_FEEDBACK] > 0.5f;
        int cmd = r == 0 ? z->prevHeat : z->prevCool;
        int cond = fb != cmd ? COND_HI : COND_NONE;
        if (countdown(&z->fbCond[r], &z->fbRemain[r], cond, c->errTO, dt)) {
            z->errBits |= bit;
            clear_cd(&z->fbCond[r], &z->fbRemain[r]);
        }
    }

    /* --- heating / cooling on ControlTemp -------------------------------- */
    float t = z->filtered[z->active - 1];
    if (bad_reading(in[TC_TEMP1 + z->active - 1]) || is_nan(t)) {
        z->mode = MODE_IDLE;                             /* relays off while the reading is unusable */
        clear_cd(&z->dbCond, &z->dbRemain);
    } else {
        if (z->mode == MODE_HEAT && t >= c->sp) z->mode = MODE_IDLE;
        else if (z->mode == MODE_COOL && t <= c->sp) z->mode = MODE_IDLE;

        if (z->mode != MODE_IDLE) {
            clear_cd(&z->dbCond, &z->dbRemain);          /* running: deadband timer not in play */
        } else {
            int dbc = (t > c->hiBand) ? COND_HI : (t < c->loBand) ? COND_LO : COND_NONE;
            if (countdown(&z->dbCond, &z->dbRemain, dbc, c->dbTO, dt)) {
                z->mode = (dbc == COND_HI) ? MODE_COOL : MODE_HEAT;
                clear_cd(&z->dbCond, &z->dbRemain);
            }
        }
    }
    z->prevHeat = z->mode == MODE_HEAT;
    z->prevCool = z->mode == MODE_COOL;
}

/* ---- outputs ------------------------------------------------------------ */
static float min_remain(const TcZone* z)
{
    float m = 0.0f;
    const int*   conds[]   = { &z->sensCond[0], &z->sensCond[1], &z->disCond, &z->fbCond[0], &z->fbCond[1] };
    const float* remains[] = { &z->sensRemain[0], &z->sensRemain[1], &z->disRemain, &z->fbRemain[0], &z->fbRemain[1] };
    for (int i = 0; i < 5; i++) {
        if (*conds[i] == COND_NONE) continue;
        if (m == 0.0f || *remains[i] < m) m = *remains[i];
    }
    return m;
}

static void write_out(const TcZone* z, const float* in, const TcCfg* c, float* out)
{
    float tmp[TC_INPUT_COUNT];
    memcpy(tmp, in, sizeof tmp);                        /* in and out may alias */
    tmp[TC_HEATING_CMD] = (z->mode == MODE_HEAT) ? 1.0f : 0.0f;
    tmp[TC_COOLING_CMD] = (z->mode == MODE_COOL) ? 1.0f : 0.0f;
    memcpy(out, tmp, sizeof tmp);

    int failed1 = (z->errBits & TC_ERRBIT_SENSOR1) != 0;
    int failed2 = c->t2en && (z->errBits & TC_ERRBIT_SENSOR2) != 0;
    float errRemain = z->stopped ? 0.0f : min_remain(z);

    int status;
    if (z->stopped)                          status = TC_STATUS_STOPPED;
    else if (errRemain > 0.0f)               status = TC_STATUS_ERROR_PENDING;
    else if (z->warmup[z->active - 1])       status = TC_STATUS_WARMUP;
    else if (c->t2en && (failed1 != failed2)) status = TC_STATUS_DEGRADED;
    else if (z->mode == MODE_HEAT)           status = TC_STATUS_HEATING;
    else if (z->mode == MODE_COOL)           status = TC_STATUS_COOLING;
    else if (z->dbCond == COND_HI)           status = TC_STATUS_COOL_PENDING;
    else if (z->dbCond == COND_LO)           status = TC_STATUS_HEAT_PENDING;
    else                                     status = TC_STATUS_IN_BAND;

    out[TC_ERROR_STATUS]    = (float)(z->errBits | (c->warn ? TC_ERRBIT_CONFIG : 0u));
    out[TC_TEMP_STATUS]     = (float)status;
    out[TC_CONTROL_TEMP]    = z->filtered[z->active - 1];
    out[TC_TEMP1_FILTERED]  = z->filtered[0];
    out[TC_TEMP2_FILTERED]  = c->t2en ? z->filtered[1] : quiet_nan();
    out[TC_HI_BAND]         = c->hiBand;
    out[TC_LO_BAND]         = c->loBand;
    out[TC_ERROR_REMAIN_MS] = errRemain;
    out[TC_DB_REMAIN_MS]    = (z->dbCond != COND_NONE) ? z->dbRemain : 0.0f;
    out[TC_ACTIVE_SENSOR]   = (float)z->active;
}

TC_API int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
                      const float* in, int32_t inLen,
                      float* out, int32_t outLen)
{
    if (!in || !out || inLen < TC_INPUT_COUNT || outLen < TC_SIGNAL_COUNT) return TC_ERR_ARG;
    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    TcZone* z = &g_zones[zone];
    TcCfg cfg;
    read_cfg(in, &cfg);

    switch (action) {
    case TC_ACTION_INIT:
        memset(z, 0, sizeof *z);
        z->inited = 1;
        z->active = 1;
        z->lastMs = nowMs;
        if (in[TC_HEATING_CMD] > 0.5f)      z->mode = MODE_HEAT;
        else if (in[TC_COOLING_CMD] > 0.5f) z->mode = MODE_COOL;
        z->prevHeat = z->mode == MODE_HEAT;
        z->prevCool = z->mode == MODE_COOL;
        update_filters(z, in, &cfg);
        break;
    case TC_ACTION_RESET:
        z->inited = 1;
        z->stopped = 0;
        z->errBits = 0;
        z->mode = MODE_IDLE;
        z->active = 1;
        z->prevHeat = z->prevCool = 0;
        z->lastMs = nowMs;
        clear_timers(z);
        update_filters(z, in, &cfg);
        break;
    case TC_ACTION_STEP:
        if (!z->inited) return TC_ERR_NOT_INIT;
        step(z, in, &cfg, nowMs);
        break;
    default:
        return TC_ERR_ACTION;
    }
    write_out(z, in, &cfg, out);
    return cfg.warn ? TC_WARN_CONFIG : TC_OK;
}
