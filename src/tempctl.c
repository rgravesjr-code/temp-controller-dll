/*
 * tempctl.c - TempCtl v4 controller (see tempctl.h and TEMPCTL-SPEC-v4.0.0.md).
 *
 * All state is static (16 zones), no allocation, no I/O, no <math.h>: the
 * .so imports memcpy/memset from libc only. Rule numbers (Rx.y) refer to
 * the v4.0.0 specification; R1..R9 are the v3 rules kept unchanged, R10 is
 * the v4 lifecycle (Start / Stop / run permissive).
 */
#include "tempctl.h"
#include <string.h>

#define NBUCKET    60          /* R5.6: 60 one-minute buckets                     */
#define BUCKET_MS  60000u
#define COND_NONE  0
#define COND_HEAT  1
#define COND_COOL  2

/* lifecycle of a non-faulted, enabled zone that is not started (R10) */
#define LIFE_IDLE     0        /* IdleStopped                                    */
#define LIFE_BLOCKED  1        /* IdleStartBlocked                               */
#define LIFE_PENDING  2        /* OperatingConditionPending (countdown running)  */
#define LIFE_TRIPPED  3        /* IdleOperatingConditionTripped                  */

typedef struct {
    int      enable, units;
    double   setpoint, dbHi, dbLo, hiLim, loLim, hiBand, loBand;
    uint32_t errTo, dbTo, aspTo;
    int      t2en;
    double   t2off, t2tol;
    uint32_t cmpTo;
    int      filter;
    int      fben;
    uint32_t fbTo;
    uint32_t ocTo;                      /* OperatingConditionTimeout (R10.7)            */
} TcCfg;

typedef struct {
    int      failed, failHigh;          /* latched failure and its direction (R5.5)     */
    int      oor, oorPrev;              /* out of range this tick / previous tick        */
    uint64_t accumHalf;                 /* leaky accumulator in half-ms units (R5.4, DRAIN 0.5) */
    double   raw, val;                  /* as supplied / value used (sensor 2: corrected) */
    double   avgBuf[TC_MAX_FILTER];
    int      avgN, avgHead;
    uint32_t bucket[NBUCKET];           /* hourly transition ring (R5.6)                  */
    int      bucketIdx;
    uint32_t bucketElapsed;
} TcSensor;

typedef struct {
    int      init;
    TcCfg    cfg;
    int      configFault, configInvalid;
    int      faulted;                   /* a fault is latched (status >= TC_ST_FAULT_FIRST) */
    int      started;                   /* Start accepted, control may run (R10.3)        */
    int      life;                      /* LIFE_* while enabled, not faulted, not started */
    int      runPerm;                   /* last evaluated permissive: -1 none, 0, 1       */
    int32_t  status, warning;           /* last reported values (the mirrors)             */
    int      doHeater, doCooler;        /* current commands                              */
    int      prevHeater, prevCooler;    /* commands of the previous CheckTemp (R8.3)     */
    uint32_t lastMs;
    int      activeSensor;
    int      initialHc;
    int      runningOnT2;
    int      seenCheck;                 /* an active CheckTemp has run since Init/Reset   */
    int      seenRaw;                   /* any enabled CheckTemp has run since Init/Reset */
    double   ctrlTemp;
    TcSensor s[2];
    int      dbCond;  uint32_t dbEl;    /* deadband countdown (R7.1)                     */
    int      aspCond; uint32_t aspEl;   /* at-setpoint countdown (R7.2)                  */
    int      cmpCond; uint32_t cmpEl;   /* disagreement countdown (R6.4)                 */
    int      cmpWarn;
    int      hfbCond; uint32_t hfbEl;   /* feedback countdowns (R8.4)                    */
    int      cfbCond; uint32_t cfbEl;
    int      ocCond;  uint32_t ocEl;    /* operating-condition countdown (R10.7)         */
} TcZone;

static TcZone g_zones[TC_MAX_ZONES];

/* ------------------------------------------------------------------------ */
/* numeric helpers (no <math.h>)                                             */
/* ------------------------------------------------------------------------ */
static double quiet_nan(void)
{
    uint64_t bits = 0x7FF8000000000000ull;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}
static int is_nan(double v)    { return v != v; }
static int is_finite(double v) { return v == v && (v - v) == 0.0; }
static int as_bool(double v)   { return v > 0.1 ? 1 : 0; }
static uint32_t sat_add(uint32_t a, uint32_t b) { uint32_t r = a + b; return r < a ? 0xFFFFFFFFu : r; }
static uint32_t remain(int cond, uint32_t el, uint32_t to) { return (cond != COND_NONE && to > el) ? to - el : 0u; }

/* Whole milliseconds, must be >= 1 (fractions truncated). */
static int to_ms(double v, uint32_t* out)
{
    if (!(v >= 1.0)) { *out = 0; return 0; }             /* NaN, 0, negative, fractions below 1 */
    *out = v >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)v;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* setup parsing + config check (R9.5)                                       */
/* ------------------------------------------------------------------------ */
static int parse_cfg(const double* a, TcCfg* c)
{
    int ok = 1;
    double f;

    c->enable = as_bool(a[TC_SETUP_TEMP_CTRL_ENABLE]);
    if (is_nan(a[TC_SETUP_TEMP_CTRL_ENABLE])) ok = 0;

    c->units = a[TC_SETUP_TEMP_UNITS] == 0.0 ? 0 : a[TC_SETUP_TEMP_UNITS] == 1.0 ? 1 : -1;
    if (c->units < 0) ok = 0;                                              /* check 1 */

    c->setpoint = a[TC_SETUP_SETPOINT];
    c->dbHi     = a[TC_SETUP_DEADBAND_HI];
    c->dbLo     = a[TC_SETUP_DEADBAND_LO];
    c->hiLim    = a[TC_SETUP_HI_LIMIT];
    c->loLim    = a[TC_SETUP_LO_LIMIT];
    if (!(c->dbHi >= 0.0) || !(c->dbLo >= 0.0)) ok = 0;                   /* check 2 (NaN fails) */
    if (c->dbHi == 0.0 && c->dbLo == 0.0) ok = 0;
    c->hiBand = c->setpoint + c->dbHi;
    c->loBand = c->setpoint - c->dbLo;
    if (!(c->loLim < c->loBand) || !(c->hiBand < c->hiLim)) ok = 0;       /* check 3 (NaN/Inf fails) */

    if (!to_ms(a[TC_SETUP_ERROR_TIMEOUT],    &c->errTo)) ok = 0;          /* check 4 */
    if (!to_ms(a[TC_SETUP_DEADBAND_TIMEOUT], &c->dbTo))  ok = 0;
    if (!to_ms(a[TC_SETUP_AT_SETPT_TIMEOUT], &c->aspTo)) ok = 0;

    c->t2en  = as_bool(a[TC_SETUP_TEMP2_ENABLE]);
    if (is_nan(a[TC_SETUP_TEMP2_ENABLE])) ok = 0;
    c->t2off = a[TC_SETUP_TEMP2_OFFSET];
    c->t2tol = a[TC_SETUP_TEMP2_TOLERANCE];
    c->cmpTo = 0;
    if (c->t2en) {                                                         /* check 6 */
        if (!is_finite(c->t2off)) ok = 0;
        if (!(c->t2tol >= 0.0)) ok = 0;
        if (!to_ms(a[TC_SETUP_TEMP_COMPARE_TIMEOUT], &c->cmpTo)) ok = 0;
    }

    f = a[TC_SETUP_FILTER_POINTS];                                         /* R4.2: never faults */
    c->filter = (f >= 1.0 && f < (double)(TC_MAX_FILTER + 1)) ? (int)f : TC_DEFAULT_FILTER;

    c->fben = as_bool(a[TC_SETUP_FEEDBACK_ENABLE]);
    if (is_nan(a[TC_SETUP_FEEDBACK_ENABLE])) ok = 0;
    c->fbTo = 0;
    if (c->fben) {                                                         /* check 7 */
        if (!to_ms(a[TC_SETUP_RELAY_FEEDBACK_TIMEOUT], &c->fbTo)) ok = 0;
    }

    if (!to_ms(a[TC_SETUP_OPERATING_CONDITION_TIMEOUT], &c->ocTo)) ok = 0; /* check 8 (v4) */
    return ok;
}

/* ------------------------------------------------------------------------ */
/* per-zone state                                                            */
/* ------------------------------------------------------------------------ */
/* Countdowns that only run while started (R7, R6.4, R8.4). */
static void clear_control_countdowns(TcZone* z)
{
    z->dbCond = z->aspCond = z->cmpCond = z->hfbCond = z->cfbCond = COND_NONE;
    z->dbEl = z->aspEl = z->cmpEl = z->hfbEl = z->cfbEl = 0;
    z->cmpWarn = 0;
}

/* Transient warning conditions 1..5 describe the current tick; once the
 * zone stops evaluating (Stop, permissive trip) they are cleared (R10.4). */
static void clear_transient_conditions(TcZone* z)
{
    z->s[0].oor = z->s[1].oor = 0;
    clear_control_countdowns(z);
}

/* Clear everything except the stored setup and the config flags. */
static void clear_runtime(TcZone* z)
{
    int i;
    z->faulted = 0;
    z->started = 0;
    z->life = LIFE_IDLE;
    z->runPerm = -1;
    z->status = TC_ST_TEMP_CTRL_DISABLED;
    z->warning = TC_WN_NONE;
    z->doHeater = z->doCooler = 0;
    z->prevHeater = z->prevCooler = 0;
    z->activeSensor = 1;
    z->initialHc = 0;
    z->runningOnT2 = 0;
    z->seenCheck = 0;
    z->seenRaw = 0;
    z->ctrlTemp = quiet_nan();
    for (i = 0; i < 2; i++) {
        memset(&z->s[i], 0, sizeof z->s[i]);
        z->s[i].raw = z->s[i].val = quiet_nan();
    }
    clear_control_countdowns(z);
    z->ocCond = COND_NONE; z->ocEl = 0;
}

static uint32_t elapsed_ms(TcZone* z, uint32_t nowMs)
{
    uint32_t d = nowMs - z->lastMs;                 /* 2^32 wrap handled by unsigned arithmetic (R1.4) */
    if (d >= 0x80000000u) d = 0;                    /* backwards step counts as 0 ms (R1.5)          */
    z->lastMs = nowMs;
    return d;
}

/*
 * Time-qualified condition. newCond is what this tick observes (COND_NONE
 * clears). The countdown starts on the tick that first observes a
 * condition and can expire only on a later tick, when elapsed >= timeout.
 * Returns 1 on the tick it expires.
 */
static int countdown(int* cond, uint32_t* el, int newCond, uint32_t timeout, uint32_t dt)
{
    if (newCond == COND_NONE) { *cond = COND_NONE; *el = 0; return 0; }
    if (*cond != newCond)     { *cond = newCond;   *el = 0; return 0; }
    *el = sat_add(*el, dt);
    return *el >= timeout;
}

static double avg_value(const TcSensor* s)
{
    double sum = 0.0;
    int i;
    if (s->avgN == 0) return quiet_nan();
    for (i = 0; i < s->avgN; i++) sum += s->avgBuf[i];
    return sum / (double)s->avgN;
}

static void avg_push(TcSensor* s, double v, int n)
{
    s->avgBuf[s->avgHead] = v;
    s->avgHead = (s->avgHead + 1) % n;
    if (s->avgN < n) s->avgN++;
}

static void avg_clear(TcSensor* s) { s->avgN = 0; s->avgHead = 0; }

static void ring_advance(TcSensor* s, uint32_t dt)
{
    s->bucketElapsed = sat_add(s->bucketElapsed, dt);
    if (s->bucketElapsed >= BUCKET_MS) {
        uint32_t n = s->bucketElapsed / BUCKET_MS;
        s->bucketElapsed %= BUCKET_MS;
        if (n >= NBUCKET) {
            memset(s->bucket, 0, sizeof s->bucket);
        } else {
            while (n--) { s->bucketIdx = (s->bucketIdx + 1) % NBUCKET; s->bucket[s->bucketIdx] = 0; }
        }
    }
}

static uint32_t ring_count(const TcSensor* s)
{
    uint32_t total = 0;
    int i;
    for (i = 0; i < NBUCKET; i++) total = sat_add(total, s->bucket[i]);
    return total;
}

/* The hourly rings age in wall time even while the zone is stopped (R10.6). */
static void rings_age(TcZone* z, uint32_t dt)
{
    ring_advance(&z->s[0], dt);
    ring_advance(&z->s[1], dt);
}

/* Range check, hourly transitions, leaky accumulator, failure, average (R4, R5). */
static void update_sensor(TcSensor* s, double raw, double val, const TcCfg* c, uint32_t dt)
{
    int bad  = !is_finite(val);                     /* NaN/Inf: out of range high (R4.4) */
    int high = bad || val > c->hiLim;
    int low  = !bad && val < c->loLim;

    s->raw = raw;
    s->val = val;
    s->oorPrev = s->oor;
    s->oor = high || low;

    ring_advance(s, dt);
    if (s->oor && !s->oorPrev) s->bucket[s->bucketIdx] = sat_add(s->bucket[s->bucketIdx], 1);

    if (!s->failed) {                               /* R5.3: a failed sensor is not re-evaluated */
        /* R5.4 (Amendment A): out of range charges the full elapsed time, in range drains half
         * of it (DRAIN = 0.5), so anything out of range more than a third of the time fails.
         * Kept in half-ms units so odd loop periods stay exact. Every out-of-range tick charges,
         * the first one included (the one-tick grace applies to the relay countdowns only). */
        if (s->oor) s->accumHalf += 2ull * dt;
        else        s->accumHalf = s->accumHalf > dt ? s->accumHalf - dt : 0;
        if (s->accumHalf >= 2ull * c->errTo) { s->failed = 1; s->failHigh = high; }
    }
    if (!s->oor) avg_push(s, val, c->filter);       /* R4.3: only in-range samples are averaged */
}

/* Mirror the raw inputs while nothing is evaluated (stopped, blocked, pending, tripped). */
static void mirror_raw(TcZone* z, double temp1, double temp2)
{
    z->seenRaw = 1;
    z->s[0].raw = z->s[0].val = temp1;
    if (z->cfg.t2en) { z->s[1].raw = temp2; z->s[1].val = temp2 + z->cfg.t2off; }
    else             { z->s[1].raw = z->s[1].val = quiet_nan(); }
}

/* Warning 8 describes a false permissive seen in a blocked / pending / tripped state. */
static int oc_warn(const TcZone* z)
{
    return !z->started && z->life != LIFE_IDLE && z->runPerm == 0;
}

static int32_t compute_warning(const TcZone* z)
{
    const TcCfg* c = &z->cfg;
    if (z->s[0].oor)                 return TC_WN_TEMP1_OUT_OF_RANGE;
    if (c->t2en && z->s[1].oor)      return TC_WN_TEMP2_OUT_OF_RANGE;
    if (c->fben && z->hfbCond)       return TC_WN_HEATER_FB_MISMATCH;
    if (c->fben && z->cfbCond)       return TC_WN_COOLER_FB_MISMATCH;
    if (z->cmpWarn)                  return TC_WN_TEMP_DISAGREE;
    if (z->runningOnT2)              return TC_WN_RUNNING_ON_TEMP2;
    if (oc_warn(z))                  return TC_WN_OPERATING_CONDITION_NOT_MET;
    return TC_WN_NONE;
}

/* Latch a fault: relays 0, Started 0, status frozen (R9.6). */
static void fault_zone(TcZone* z, int32_t fault)
{
    z->faulted = 1;
    z->started = 0;
    z->doHeater = z->doCooler = 0;
    z->status = fault;
}

/* Status of an enabled, non-started, non-faulted zone (R10.6). */
static int32_t idle_status(const TcZone* z)
{
    switch (z->life) {
    case LIFE_BLOCKED: return TC_ST_IDLE_START_BLOCKED;
    case LIFE_PENDING: return TC_ST_OPERATING_CONDITION_PENDING;
    case LIFE_TRIPPED: return TC_ST_IDLE_OPERATING_CONDITION_TRIPPED;
    default:           return TC_ST_IDLE_STOPPED;
    }
}

static void write_outputs(const TcZone* z, int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning)
{
    if (doHeater) *doHeater = z->doHeater;
    if (doCooler) *doCooler = z->doCooler;
    *status = z->status;
    *warning = z->warning;
}

/* Status after Init/Reset (R10.2, R10.5). */
static int32_t rest_status(const TcZone* z)
{
    if (!z->cfg.enable) return TC_ST_TEMP_CTRL_DISABLED;
    if (z->faulted)     return z->status;
    return TC_ST_IDLE_STOPPED;
}

/* ------------------------------------------------------------------------ */
/* exports                                                                   */
/* ------------------------------------------------------------------------ */
TC_API int32_t TcVersion(void)    { return (TC_VERSION_MAJOR << 16) | (TC_VERSION_MINOR << 8) | TC_VERSION_PATCH; }
TC_API int32_t TcSetupCount(void) { return TC_SETUP_COUNT; }
TC_API int32_t TcDiagCount(void)  { return TC_DIAG_COUNT; }

TC_API int32_t TcInit(int32_t zone, uint32_t nowMs,
                      const double* setupArray, int32_t setupLen,
                      int32_t* status, int32_t* warning)
{
    TcZone* z;
    TcCfg c;
    int ok;

    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!setupArray || !status || !warning || setupLen != TC_SETUP_COUNT) return TC_ERR_ARG;
    z = &g_zones[zone];

    ok = parse_cfg(setupArray, &c);
    clear_runtime(z);                                                      /* R10.2: relays 0, not started; no relay keeping */
    z->init = 1;
    z->cfg = c;
    z->lastMs = nowMs;
    z->configFault = z->configInvalid = 0;
    if (!ok) {                                                             /* R9.5 */
        if (c.enable) { z->configFault = 1; fault_zone(z, TC_ST_CONFIG_FAULT); }
        else          { z->configInvalid = 1; }
    }
    z->status = rest_status(z);
    z->warning = z->configInvalid ? TC_WN_CONFIG_INVALID : TC_WN_NONE;
    write_outputs(z, 0, 0, status, warning);
    return TC_OK;
}

TC_API int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning)
{
    TcZone* z;
    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!status || !warning) return TC_ERR_ARG;
    z = &g_zones[zone];
    if (!z->init) { *status = TC_ST_TEMP_CTRL_DISABLED; *warning = TC_WN_NONE; return TC_OK; }   /* R2.4 */

    clear_runtime(z);                                                      /* R9.2 / R10.5: keeps cfg + config flags */
    z->lastMs = nowMs;
    if (z->configFault) fault_zone(z, TC_ST_CONFIG_FAULT);                 /* Reset does not clear it */
    z->status = rest_status(z);
    z->warning = z->configInvalid ? TC_WN_CONFIG_INVALID : TC_WN_NONE;
    write_outputs(z, 0, 0, status, warning);
    return TC_OK;
}

TC_API int32_t TcStart(int32_t zone, uint32_t nowMs, int32_t runPermissive,
                       int32_t* status, int32_t* warning)
{
    TcZone* z;
    uint32_t gap;

    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!status || !warning) return TC_ERR_ARG;
    z = &g_zones[zone];

    if (!z->init || !z->cfg.enable) {                                      /* R10.1: cannot start */
        z->status = TC_ST_TEMP_CTRL_DISABLED;
        z->warning = z->configInvalid ? TC_WN_CONFIG_INVALID : TC_WN_NONE;
        write_outputs(z, 0, 0, status, warning);
        return TC_OK;
    }
    if (z->faulted || z->started) {                                        /* R10.3 steps 3, 4: no evaluation */
        write_outputs(z, 0, 0, status, warning);
        return TC_OK;
    }

    z->runPerm = runPermissive != 0;                                       /* evaluated (V4-D2) */
    if (!z->runPerm) {                                                     /* steps 5..7: refused */
        if (z->life != LIFE_PENDING && z->life != LIFE_TRIPPED) z->life = LIFE_BLOCKED;
        z->status = idle_status(z);
        z->warning = compute_warning(z);
        write_outputs(z, 0, 0, status, warning);
        return TC_OK;
    }

    /* step 8: accepted. Stopped time never feeds a countdown, but the hourly rings age. */
    gap = nowMs - z->lastMs;
    if (gap < 0x80000000u) rings_age(z, gap);
    z->lastMs = nowMs;
    z->started = 1;
    z->life = LIFE_IDLE;
    z->doHeater = z->doCooler = z->prevHeater = z->prevCooler = 0;
    clear_transient_conditions(z);
    z->ocCond = COND_NONE; z->ocEl = 0;
    avg_clear(&z->s[0]); avg_clear(&z->s[1]);
    z->initialHc = 0;
    z->status = TC_ST_TEMP_AT_SETPT;                                       /* provisional until the first CheckTemp */
    z->warning = compute_warning(z);                                       /* RunningOnTemp2 survives (R5.5) */
    write_outputs(z, 0, 0, status, warning);
    return TC_OK;
}

TC_API int32_t TcStop(int32_t zone, uint32_t nowMs,
                      int32_t* doHeater, int32_t* doCooler,
                      int32_t* status, int32_t* warning)
{
    TcZone* z;
    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!doHeater || !doCooler || !status || !warning) return TC_ERR_ARG;
    z = &g_zones[zone];

    if (!z->init) {
        *doHeater = *doCooler = 0; *status = TC_ST_TEMP_CTRL_DISABLED; *warning = TC_WN_NONE;
        return TC_OK;
    }
    if (!z->cfg.enable) {
        z->doHeater = z->doCooler = 0;
        z->status = TC_ST_TEMP_CTRL_DISABLED;
        z->warning = z->configInvalid ? TC_WN_CONFIG_INVALID : TC_WN_NONE;
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }
    if (z->faulted) {                                                      /* fault and frozen warning kept */
        z->doHeater = z->doCooler = 0;
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }

    z->started = 0;
    z->doHeater = z->doCooler = z->prevHeater = z->prevCooler = 0;
    z->lastMs = nowMs;
    clear_transient_conditions(z);                                         /* warnings 1..5 gone */
    if (z->life == LIFE_PENDING || z->life == LIFE_TRIPPED) {
        z->ocCond = COND_NONE; z->ocEl = 0;                                /* escalation cancelled, cause kept */
        z->life = LIFE_TRIPPED;
    } else {
        z->life = LIFE_IDLE;                                               /* Stop acknowledges a blocked Start */
    }
    z->status = idle_status(z);
    z->warning = compute_warning(z);
    write_outputs(z, doHeater, doCooler, status, warning);
    return TC_OK;
}

TC_API int32_t TcCheckTemp(int32_t zone, uint32_t nowMs,
                           double temp1, double temp2,
                           int32_t diHeaterFB, int32_t diCoolerFB,
                           int32_t runPermissive,
                           int32_t* doHeater, int32_t* doCooler,
                           int32_t* status, int32_t* warning)
{
    TcZone* z;
    const TcCfg* c;
    uint32_t dt;
    int32_t fault = 0;
    int activeOor, perm;

    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!doHeater || !doCooler || !status || !warning) return TC_ERR_ARG;
    z = &g_zones[zone];
    c = &z->cfg;

    /* R2: disabled or never initialised -> no action */
    if (!z->init || !c->enable) {
        z->doHeater = z->doCooler = 0;
        z->status = TC_ST_TEMP_CTRL_DISABLED;
        z->warning = z->configInvalid ? TC_WN_CONFIG_INVALID : TC_WN_NONE;
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }
    dt = elapsed_ms(z, nowMs);

    /* R9.6: latched fault -> relays 0, status latched, warning frozen, permissive not evaluated */
    if (z->faulted) {
        z->doHeater = z->doCooler = 0;
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }
    perm = runPermissive != 0;
    z->runPerm = perm;

    /* R10.6: not started -> nothing evaluated but the pending permissive countdown */
    if (!z->started) {
        z->doHeater = z->doCooler = 0;
        mirror_raw(z, temp1, temp2);
        rings_age(z, dt);
        if (z->life == LIFE_PENDING) {
            if (perm) {                                                    /* recovered before the timeout: no fault, no restart */
                z->ocCond = COND_NONE; z->ocEl = 0;
                z->life = LIFE_TRIPPED;
            } else if (countdown(&z->ocCond, &z->ocEl, COND_HEAT, c->ocTo, dt)) {
                z->warning = compute_warning(z);                           /* frozen from here on */
                fault_zone(z, TC_ST_OPERATING_CONDITION_FAULT);
                write_outputs(z, doHeater, doCooler, status, warning);
                return TC_OK;
            }
        }
        z->status = idle_status(z);
        z->warning = compute_warning(z);
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }
    z->seenCheck = 1;
    z->seenRaw = 1;

    /* ---- sensors (R4, R5, R6.1) ------------------------------------- */
    update_sensor(&z->s[0], temp1, temp1, c, dt);
    if (c->t2en) update_sensor(&z->s[1], temp2, temp2 + c->t2off, c, dt);
    else { z->s[1].oor = 0; z->s[1].raw = z->s[1].val = quiet_nan(); }

    /* ---- sensor failure effects (R5.5) ------------------------------ */
    if (!c->t2en) {
        if (z->s[0].failed) fault = z->s[0].failHigh ? TC_ST_TEMP1_FAIL_HIGH : TC_ST_TEMP1_FAIL_LOW;
    } else if (z->s[0].failed && z->s[1].failed) {
        fault = TC_ST_BOTH_SENSORS_FAILED;
    } else if (z->s[0].failed && z->activeSensor == 1) {
        z->activeSensor = 2;                                               /* failover, never back */
        z->runningOnT2 = 1;
    }
    z->ctrlTemp = z->activeSensor == 1 ? z->s[0].val : z->s[1].val;

    /* ---- two-sensor comparison (R6) --------------------------------- */
    if (!fault) {
        int gate = c->t2en && !z->s[0].failed && !z->s[1].failed && z->initialHc
                && z->s[0].avgN == c->filter && z->s[1].avgN == c->filter
                && !z->s[0].oor && !z->s[1].oor;                           /* R6.2 */
        if (gate) {
            double diff = avg_value(&z->s[0]) - avg_value(&z->s[1]);
            int dis = (diff > c->t2tol || -diff > c->t2tol) ? COND_HEAT : COND_NONE;   /* R6.3 */
            if (countdown(&z->cmpCond, &z->cmpEl, dis, c->cmpTo, dt)) fault = TC_ST_TEMP_DISAGREE_FAULT;
            z->cmpWarn = z->cmpCond != COND_NONE && z->cmpEl >= c->cmpTo / 10u;        /* R6.4 */
        } else {                                                           /* R6.5: pause = restart from zero */
            z->cmpCond = COND_NONE; z->cmpEl = 0; z->cmpWarn = 0;
        }
    }

    /* ---- relay feedback (R8) ---------------------------------------- */
    if (c->fben) {
        int hm = ((diHeaterFB != 0) != (z->prevHeater != 0)) ? COND_HEAT : COND_NONE;
        int cm = ((diCoolerFB != 0) != (z->prevCooler != 0)) ? COND_COOL : COND_NONE;
        int hf = countdown(&z->hfbCond, &z->hfbEl, hm, c->fbTo, dt);
        int cf = countdown(&z->cfbCond, &z->cfbEl, cm, c->fbTo, dt);
        if (!fault && hf) fault = TC_ST_HEATER_FB_FAULT;
        if (!fault && cf) fault = TC_ST_COOLER_FB_FAULT;
    }

    if (fault) {                                                           /* R9.6: an existing fault wins (R10.7 step 1) */
        z->warning = compute_warning(z);
        fault_zone(z, fault);
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }

    /* ---- run permissive lost (R10.7, R10.8): both relays off on this sample */
    if (!perm) {
        z->started = 0;
        z->life = LIFE_PENDING;
        z->doHeater = z->doCooler = z->prevHeater = z->prevCooler = 0;
        clear_transient_conditions(z);                                     /* R10.9: no feedback fault from this transition */
        countdown(&z->ocCond, &z->ocEl, COND_HEAT, c->ocTo, dt);           /* observed: full timeout remaining */
        z->status = TC_ST_OPERATING_CONDITION_PENDING;
        z->warning = compute_warning(z);
        write_outputs(z, doHeater, doCooler, status, warning);
        return TC_OK;
    }

    /* ---- control (R7), paused while the active sensor is out of range (R5.7) */
    activeOor = z->activeSensor == 1 ? z->s[0].oor : z->s[1].oor;
    if (!activeOor) {
        double ct = z->ctrlTemp;
        if (z->doHeater) {                                                 /* R7.2 release */
            if (countdown(&z->aspCond, &z->aspEl, ct >= c->setpoint ? COND_HEAT : COND_NONE, c->aspTo, dt)) {
                z->doHeater = 0; z->initialHc = 1; z->aspCond = COND_NONE; z->aspEl = 0;
            }
        } else if (z->doCooler) {
            if (countdown(&z->aspCond, &z->aspEl, ct <= c->setpoint ? COND_COOL : COND_NONE, c->aspTo, dt)) {
                z->doCooler = 0; z->initialHc = 1; z->aspCond = COND_NONE; z->aspEl = 0;
            }
        }
        if (!z->doHeater && !z->doCooler) {                                /* idle: R7.1 engage */
            int cond = ct > c->hiBand ? COND_COOL : ct < c->loBand ? COND_HEAT : COND_NONE;
            if (cond == COND_NONE) z->initialHc = 1;                       /* R7.5 (2) */
            if (countdown(&z->dbCond, &z->dbEl, cond, c->dbTo, dt)) {
                if (cond == COND_HEAT) z->doHeater = 1; else z->doCooler = 1;
                z->dbCond = COND_NONE; z->dbEl = 0;
            }
        }
    }

    z->status = z->doHeater ? TC_ST_HEATER_ON
              : z->doCooler ? TC_ST_COOLER_ON
              : z->dbCond == COND_HEAT ? TC_ST_HEAT_PENDING
              : z->dbCond == COND_COOL ? TC_ST_COOL_PENDING
              : TC_ST_TEMP_AT_SETPT;
    z->warning = compute_warning(z);
    z->prevHeater = z->doHeater;
    z->prevCooler = z->doCooler;
    write_outputs(z, doHeater, doCooler, status, warning);
    return TC_OK;
}

TC_API int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen)
{
    const TcZone* z;
    const TcCfg* c;
    double nan = quiet_nan();
    double* d = diagArray;

    if (zone < 0 || zone >= TC_MAX_ZONES) return TC_ERR_ZONE;
    if (!diagArray || diagLen < TC_DIAG_COUNT) return TC_ERR_ARG;
    z = &g_zones[zone];
    c = &z->cfg;

    if (!z->init) {
        int i;
        for (i = 0; i < TC_DIAG_COUNT; i++) d[i] = 0.0;
        d[TC_DIAG_CONTROL_TEMP] = d[TC_DIAG_TEMP1_RAW] = d[TC_DIAG_TEMP2_RAW] = d[TC_DIAG_TEMP2_CORRECTED] = nan;
        d[TC_DIAG_TEMP1_AVG] = d[TC_DIAG_TEMP2_AVG] = d[TC_DIAG_HI_BAND] = d[TC_DIAG_LO_BAND] = nan;
        d[TC_DIAG_ACTIVE_SENSOR] = 1.0;
        d[TC_DIAG_RUN_PERMISSIVE] = nan;
        return TC_OK;
    }
    d[TC_DIAG_CONTROL_TEMP]     = z->seenCheck ? z->ctrlTemp : nan;
    d[TC_DIAG_ACTIVE_SENSOR]    = (double)z->activeSensor;
    d[TC_DIAG_TEMP1_RAW]        = z->seenRaw ? z->s[0].raw : nan;
    d[TC_DIAG_TEMP2_RAW]        = (z->seenRaw && c->t2en) ? z->s[1].raw : nan;
    d[TC_DIAG_TEMP2_CORRECTED]  = (z->seenRaw && c->t2en) ? z->s[1].val : nan;
    d[TC_DIAG_TEMP1_AVG]        = avg_value(&z->s[0]);
    d[TC_DIAG_TEMP2_AVG]        = c->t2en ? avg_value(&z->s[1]) : nan;
    d[TC_DIAG_HI_BAND]          = c->hiBand;
    d[TC_DIAG_LO_BAND]          = c->loBand;
    d[TC_DIAG_INITIAL_HC_FLAG]  = (double)z->initialHc;
    d[TC_DIAG_DEADBAND_REMAIN_MS]  = (double)remain(z->dbCond,  z->dbEl,  c->dbTo);
    d[TC_DIAG_AT_SETPT_REMAIN_MS]  = (double)remain(z->aspCond, z->aspEl, c->aspTo);
    d[TC_DIAG_COMPARE_REMAIN_MS]   = (double)remain(z->cmpCond, z->cmpEl, c->cmpTo);
    d[TC_DIAG_HEATER_FB_REMAIN_MS] = (double)remain(z->hfbCond, z->hfbEl, c->fbTo);
    d[TC_DIAG_COOLER_FB_REMAIN_MS] = (double)remain(z->cfbCond, z->cfbEl, c->fbTo);
    d[TC_DIAG_TEMP1_OOR_ACCUM_MS]  = (double)z->s[0].accumHalf / 2.0;
    d[TC_DIAG_TEMP2_OOR_ACCUM_MS]  = (double)z->s[1].accumHalf / 2.0;
    d[TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR] = (double)ring_count(&z->s[0]);
    d[TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR] = (double)ring_count(&z->s[1]);
    d[TC_DIAG_STATUS_MIRROR]    = (double)z->status;
    d[TC_DIAG_WARNING_MIRROR]   = (double)z->warning;
    d[TC_DIAG_DO_HEATER_MIRROR] = (double)z->doHeater;
    d[TC_DIAG_DO_COOLER_MIRROR] = (double)z->doCooler;
    d[TC_DIAG_APPLIED_FILTER_POINTS] = (double)c->filter;
    d[TC_DIAG_ZONE_INITIALIZED] = 1.0;
    d[TC_DIAG_RUN_PERMISSIVE]   = z->runPerm < 0 ? nan : (double)z->runPerm;
    d[TC_DIAG_OPERATING_CONDITION_REMAIN_MS] = (double)remain(z->ocCond, z->ocEl, c->ocTo);
    d[TC_DIAG_CONTROLLER_STARTED] = (double)z->started;
    return TC_OK;
}
