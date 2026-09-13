/*
 * tempctl.h - TempCtl v2: temperature controller for LabVIEW CLFN
 *
 * Plain C99, no dependencies beyond <string.h>. Builds as tempctl.dll (Windows
 * x64/x86) and libtempctl.so (Linux x86_64 for cRIO-904x/905x/906x, aarch64
 * for Raspberry Pi). All exports use the C calling convention (cdecl).
 *
 * The controller is pure signals in, signals out: it owns no hardware and no
 * CAN. The caller maps thermocouples and relays to the array below; the
 * output array is laid out so that it can be handed straight to CanTp
 * (CanTp_PackSgl) with the message definition generated from
 * tools/make_tempctl_dbc.py, without reordering.
 *
 * LabVIEW CLFN mapping:
 *   const float* / float*    -> Array Data Pointer of SGL, plus an I32 length
 *   int32_t / uint32_t       -> Numeric, Value
 *
 * Return value of every function is TC_OK (0), a negative TC_ERR_* code, or a
 * positive TC_WARN_* code (call succeeded, but something is worth a look).
 */
#ifndef TEMPCTL_H
#define TEMPCTL_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(TEMPCTL_BUILD)
#    define TC_API __declspec(dllexport)
#  else
#    define TC_API __declspec(dllimport)
#  endif
#else
#  define TC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                   */
/* ------------------------------------------------------------------------ */
#define TC_VERSION_MAJOR 2
#define TC_VERSION_MINOR 0
#define TC_VERSION_PATCH 1
/* Returns (major << 16) | (minor << 8) | patch. */
TC_API uint32_t TcVersion(void);
/* Number of elements the `in` array must have (TC_INPUT_COUNT). */
TC_API int32_t TcInputCount(void);
/* Number of elements the `out` array must have (TC_SIGNAL_COUNT). */
TC_API int32_t TcSignalCount(void);

/* ------------------------------------------------------------------------ */
/* Return codes                                                              */
/* ------------------------------------------------------------------------ */
#define TC_OK                 0
#define TC_WARN_CONFIG        1   /* init/step: configuration inconsistent (see TC_ERRBIT_CONFIG); the call still ran */
#define TC_ERR_ARG           -1   /* null pointer or array too short          */
#define TC_ERR_ZONE          -2   /* zone index out of range                  */
#define TC_ERR_ACTION        -3   /* unknown action                           */
#define TC_ERR_NOT_INIT      -4   /* step called before init                  */

/* ------------------------------------------------------------------------ */
/* Signal array                                                              */
/* ------------------------------------------------------------------------ */
/*
 * One SGL array in, one SGL array out. Elements 0..TC_INPUT_COUNT-1 are read
 * from `in` on every call (configuration is live: a change takes effect on
 * the next Step, no re-Init). All of them are echoed to `out`, followed by
 * the controller's own outputs. `in` and `out` may be the same array.
 */
enum TcSignal {
    /* --- configuration ------------------------------------------------ */
    TC_SETPOINT          = 0,  /* deg. Heating runs until >= Setpoint, cooling until <= Setpoint  */
    TC_DEADBAND_HI       = 1,  /* deg, offset >= 0: HiBand = Setpoint + DeadbandHi                 */
    TC_DEADBAND_LO       = 2,  /* deg, offset >= 0: LoBand = Setpoint - DeadbandLo                 */
    TC_HI_LIMIT          = 3,  /* deg. A sensor above this for ErrorTimeout ms has failed (hi)     */
    TC_LO_LIMIT          = 4,  /* deg. A sensor below this for ErrorTimeout ms has failed (lo)     */
    TC_ERROR_TIMEOUT     = 5,  /* ms. Limit / bad-reading / disagreement / feedback countdown     */
    TC_DEADBAND_TIMEOUT  = 6,  /* ms. Time outside the band before a relay engages                */
    TC_FILTER_POINTS     = 7,  /* 1..64 samples in the moving average of each sensor (0 -> 4)     */
    TC_TEMP2_ENABLE      = 8,  /* 0/1. Second sensor present: rationality check + failover        */
    TC_TEMP2_TOLERANCE   = 9,  /* deg. |Temp1f - Temp2f| above this for ErrorTimeout -> disagree  */
    TC_FEEDBACK_ENABLE   = 10, /* 0/1. Compare relay feedback inputs with the commands             */
    /* --- measurements ------------------------------------------------- */
    TC_TEMP1             = 11, /* deg, sensor 1 (the primary)                                     */
    TC_TEMP2             = 12, /* deg, sensor 2 (ignored unless Temp2Enable)                      */
    TC_HEATER_FEEDBACK   = 13, /* 0/1 measured heater relay state (ignored unless FeedbackEnable) */
    TC_COOLER_FEEDBACK   = 14, /* 0/1 measured cooler relay state                                 */
    /* --- relay commands: in = initial state (Init only), out = command -- */
    TC_HEATING_CMD       = 15, /* 0/1                                                             */
    TC_COOLING_CMD       = 16, /* 0/1                                                             */
    TC_INPUT_COUNT       = 17,
    /* --- outputs only ------------------------------------------------- */
    TC_ERROR_STATUS      = 17, /* bit mask, TC_ERRBIT_*                                           */
    TC_TEMP_STATUS       = 18, /* TcTempStatus                                                    */
    TC_CONTROL_TEMP      = 19, /* deg, filtered value of the active sensor that drives control   */
    TC_TEMP1_FILTERED    = 20, /* deg, moving average of Temp1 (NaN until a valid sample exists) */
    TC_TEMP2_FILTERED    = 21, /* deg, moving average of Temp2 (NaN when disabled / no sample)   */
    TC_HI_BAND           = 22, /* deg, Setpoint + DeadbandHi                                      */
    TC_LO_BAND           = 23, /* deg, Setpoint - DeadbandLo                                      */
    TC_ERROR_REMAIN_MS   = 24, /* ms, smallest running error countdown, 0 when none is running   */
    TC_DB_REMAIN_MS      = 25, /* ms, remaining deadband countdown, 0 when idle or running       */
    TC_ACTIVE_SENSOR     = 26, /* 1 or 2: the sensor whose filtered value is ControlTemp         */
    TC_SIGNAL_COUNT      = 27
};

/* TC_ERROR_STATUS bits. b0..b2 keep the v1 meaning for a single-sensor
 * system (hi limit, lo limit, bad reading). Bits 0..8 latch until Reset or
 * Init; bit 9 follows the current configuration. */
#define TC_ERRBIT_T1_HI        (1u << 0)  /* Temp1 above HiLimit for ErrorTimeout: sensor 1 failed */
#define TC_ERRBIT_T1_LO        (1u << 1)  /* Temp1 below LoLimit for ErrorTimeout: sensor 1 failed */
#define TC_ERRBIT_T1_BAD       (1u << 2)  /* Temp1 NaN/Inf for ErrorTimeout: sensor 1 failed        */
#define TC_ERRBIT_T2_HI        (1u << 3)
#define TC_ERRBIT_T2_LO        (1u << 4)
#define TC_ERRBIT_T2_BAD       (1u << 5)
#define TC_ERRBIT_DISAGREE     (1u << 6)  /* |Temp1f - Temp2f| > Temp2Tolerance for ErrorTimeout   */
#define TC_ERRBIT_HEATER_FB    (1u << 7)  /* heater feedback != heating command for ErrorTimeout   */
#define TC_ERRBIT_COOLER_FB    (1u << 8)  /* cooler feedback != cooling command for ErrorTimeout   */
#define TC_ERRBIT_CONFIG       (1u << 9)  /* configuration inconsistent (also TC_WARN_CONFIG)      */
#define TC_ERRBIT_SENSOR1      (TC_ERRBIT_T1_HI | TC_ERRBIT_T1_LO | TC_ERRBIT_T1_BAD)
#define TC_ERRBIT_SENSOR2      (TC_ERRBIT_T2_HI | TC_ERRBIT_T2_LO | TC_ERRBIT_T2_BAD)

/* TC_TEMP_STATUS values. When several apply the first in this list wins:
 * STOPPED, ERROR_PENDING, WARMUP, DEGRADED, then the control state. */
enum TcTempStatus {
    TC_STATUS_IN_BAND       = 0, /* idle, ControlTemp inside [LoBand, HiBand]        */
    TC_STATUS_HEAT_PENDING  = 1, /* below LoBand, deadband countdown running          */
    TC_STATUS_HEATING       = 2, /* heating relay on, until ControlTemp >= Setpoint   */
    TC_STATUS_COOL_PENDING  = 3, /* above HiBand, deadband countdown running          */
    TC_STATUS_COOLING       = 4, /* cooling relay on, until ControlTemp <= Setpoint   */
    TC_STATUS_ERROR_PENDING = 5, /* an error countdown is running (ErrorRemainMs > 0) */
    TC_STATUS_STOPPED       = 6, /* fault: relays off, latched until Reset            */
    TC_STATUS_DEGRADED      = 7, /* Temp2Enable and one sensor failed; running on the other */
    TC_STATUS_WARMUP        = 8  /* fewer than FilterPoints samples in the active filter */
};

enum TcAction {
    TC_ACTION_INIT  = 0,  /* clear everything, relay state from in[15..16], sensor 1 active */
    TC_ACTION_STEP  = 1,  /* run one control tick                                          */
    TC_ACTION_RESET = 2   /* clear faults, latched bits, timers; relays off; keep filters  */
};

#define TC_MAX_ZONES     16
#define TC_MAX_FILTER    64
#define TC_DEFAULT_FILTER 4

/*
 * One controller tick.
 *   zone    0..TC_MAX_ZONES-1, independent controller instances.
 *   action  TcAction.
 *   nowMs   free-running millisecond tick (LabVIEW Tick Count (ms)). The
 *           library differences successive values; wrap-around is handled.
 *   in      >= TC_INPUT_COUNT SGL values, order per TcSignal.
 *   out     >= TC_SIGNAL_COUNT SGL values. May be the same array as `in`.
 *
 * Behaviour (STEP), evaluated in this order:
 *
 *   Filtering  Each sensor's valid (non-NaN/Inf) samples enter a moving
 *              average of FilterPoints; the filtered value is NaN until the
 *              first valid sample. Bad samples are not averaged.
 *
 *   Sensors    Per sensor (Temp2 only when Temp2Enable): condition = bad
 *              reading (raw NaN/Inf or no filtered value yet), else filtered
 *              > HiLimit, else filtered < LoLimit. A condition held for
 *              ErrorTimeout ms fails the sensor (latched bit, see
 *              TC_ERRBIT_*). Countdowns start at the first Step that observes
 *              the condition and restart from full when it changes.
 *              Disagreement |Temp1f - Temp2f| > Temp2Tolerance for
 *              ErrorTimeout latches TC_ERRBIT_DISAGREE; the controller keeps
 *              running on the active sensor.
 *
 *   Failover   Temp2Enable = 0: sensor 1 failed -> STOPPED (relays off,
 *              latched). Temp2Enable = 1: the active sensor failed and the
 *              other one has not -> ActiveSensor switches, DEGRADED; both
 *              failed -> STOPPED. A failed sensor stays failed until Reset.
 *
 *   Bad active A raw NaN/Inf on the active sensor drops both relays at once
 *              (and clears the deadband countdown) while its error countdown
 *              runs, exactly as v1.
 *
 *   Control    On ControlTemp (filtered active sensor): heating stays on
 *              until >= Setpoint, cooling until <= Setpoint. Idle: above
 *              HiBand for DeadbandTimeout -> cooling; below LoBand for
 *              DeadbandTimeout -> heating. Heating and cooling are mutually
 *              exclusive.
 *
 *   Feedback   FeedbackEnable = 1: HeaterFeedback / CoolerFeedback (0/1) are
 *              compared with the commands issued by the previous Step. A
 *              mismatch held for ErrorTimeout latches TC_ERRBIT_HEATER_FB /
 *              TC_ERRBIT_COOLER_FB; operation continues.
 *
 *   Config     DeadbandHi/Lo >= 0, LoLimit < LoBand, HiBand < HiLimit,
 *              timeouts >= 0, Temp2Tolerance >= 0, FilterPoints in 1..64 (0
 *              means TC_DEFAULT_FILTER; other values are clamped). Otherwise
 *              TC_WARN_CONFIG is returned and TC_ERRBIT_CONFIG is set for
 *              this call; the controller still runs.
 */
TC_API int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
                      const float* in, int32_t inLen,
                      float* out, int32_t outLen);

#ifdef __cplusplus
}
#endif
#endif /* TEMPCTL_H */
