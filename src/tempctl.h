/*
 * tempctl.h - TempCtl v3: deadband temperature controller with sensor
 * validation, for LabVIEW's Call Library Function Node.
 *
 * Plain C99, no dependencies beyond <string.h>. Builds as tempctl.dll
 * (Windows x64/x86) and libtempctl.so (NI Linux RT x86_64 for the cRIO,
 * aarch64 for the Raspberry Pi bench). All exports use the C calling
 * convention (cdecl). Signals in, signals out: the library owns no
 * hardware, no CAN and no timing.
 *
 * This header is written for LabVIEW's Import Shared Library wizard:
 *   - only int32_t, uint32_t and double in signatures;
 *   - arrays are a pointer plus an int32_t length (mark setupArray and
 *     diagArray as arrays in the wizard; nothing else needs correcting);
 *   - no structs, enums, typedefs or 64-bit integers; every code is a
 *     plain int32_t with a #define name below.
 *
 * Call Library Function Node settings: calling convention C, "Run in any
 * thread" (no allocation, I/O or blocking inside), pointers to int32 as
 * "Numeric, Pointer to Value", arrays as "Array Data Pointer" with the
 * length wired separately.
 *
 * Behaviour reference: TEMPCTL-SPEC-v3.0.0.md (rule numbers R1..R9 are
 * quoted in the comments below).
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
/* Version and sizes                                                         */
/* ------------------------------------------------------------------------ */
#define TC_VERSION_MAJOR 3
#define TC_VERSION_MINOR 0
#define TC_VERSION_PATCH 0

#define TC_MAX_ZONES      16   /* zone index 0..15                            */
#define TC_SETUP_COUNT    17   /* elements in setupArray                      */
#define TC_DIAG_COUNT     25   /* elements in diagArray                       */
#define TC_MAX_FILTER     64   /* largest FilterPoints                        */
#define TC_DEFAULT_FILTER  4   /* FilterPoints used when the setup value is not 1..64 */

/* ------------------------------------------------------------------------ */
/* Return codes (the call result, not the controller state)                  */
/* ------------------------------------------------------------------------ */
#define TC_OK          0   /* call executed                                              */
#define TC_ERR_ARG    -1   /* null pointer, setupLen != TC_SETUP_COUNT, diagLen < TC_DIAG_COUNT */
#define TC_ERR_ZONE   -2   /* zone outside 0..TC_MAX_ZONES-1                             */
/* On a negative return nothing runs and no output is written. */

/* ------------------------------------------------------------------------ */
/* setupArray indexes (TcInit)                                               */
/* ------------------------------------------------------------------------ */
/* Booleans: value > 0.1 -> 1, else 0 (NaN -> 0). Timeouts: whole ms,
 * fractions truncated, must be >= 1. Temperatures: used as supplied, in
 * the unit named by TempUnits (label only, no conversion). */
#define TC_SETUP_TEMP_CTRL_ENABLE       0   /* bool   master switch for the zone                      */
#define TC_SETUP_TEMP_UNITS             1   /* 0 = degF, 1 = degC (label only)                        */
#define TC_SETUP_SETPOINT               2   /* deg                                                    */
#define TC_SETUP_DEADBAND_HI            3   /* deg >= 0: HiBand = Setpoint + DeadbandHi               */
#define TC_SETUP_DEADBAND_LO            4   /* deg >= 0: LoBand = Setpoint - DeadbandLo               */
#define TC_SETUP_HI_LIMIT               5   /* deg, sensor valid range upper end, > HiBand           */
#define TC_SETUP_LO_LIMIT               6   /* deg, sensor valid range lower end, < LoBand           */
#define TC_SETUP_ERROR_TIMEOUT          7   /* ms, out-of-range accumulator level that fails a sensor */
#define TC_SETUP_DEADBAND_TIMEOUT       8   /* ms, time outside the band before a relay turns on      */
#define TC_SETUP_AT_SETPT_TIMEOUT       9   /* ms, time at the setpoint before a running relay drops  */
#define TC_SETUP_TEMP2_ENABLE          10   /* bool   second sensor fitted                            */
#define TC_SETUP_TEMP2_OFFSET          11   /* deg, added to temp2 before every use                   */
#define TC_SETUP_TEMP2_TOLERANCE       12   /* deg >= 0, largest allowed |Temp1Avg - Temp2Avg|        */
#define TC_SETUP_TEMP_COMPARE_TIMEOUT  13   /* ms, disagreement time before a fault (warning at 1/10) */
#define TC_SETUP_FILTER_POINTS         14   /* 1..64 samples in each average; anything else -> 4      */
#define TC_SETUP_FEEDBACK_ENABLE       15   /* bool   compare DO feedback with the commands           */
#define TC_SETUP_RELAY_FEEDBACK_TIMEOUT 16  /* ms, feedback mismatch time before a fault              */

/* ------------------------------------------------------------------------ */
/* diagArray indexes (TcGetDiag)                                             */
/* ------------------------------------------------------------------------ */
#define TC_DIAG_CONTROL_TEMP            0   /* raw value control acts on (sensor 2 offset-corrected); NaN before the first CheckTemp */
#define TC_DIAG_ACTIVE_SENSOR           1   /* 1 or 2                                                  */
#define TC_DIAG_TEMP1_RAW               2   /* last temp1 as supplied                                  */
#define TC_DIAG_TEMP2_RAW               3   /* last temp2 as supplied (NaN when Temp2Enable = 0)       */
#define TC_DIAG_TEMP2_CORRECTED         4   /* temp2 + Temp2Offset                                     */
#define TC_DIAG_TEMP1_AVG               5   /* moving average of in-range temp1 samples (comparison only); NaN until one exists */
#define TC_DIAG_TEMP2_AVG               6   /* moving average of in-range corrected temp2 samples     */
#define TC_DIAG_HI_BAND                 7   /* Setpoint + DeadbandHi                                   */
#define TC_DIAG_LO_BAND                 8   /* Setpoint - DeadbandLo                                   */
#define TC_DIAG_INITIAL_HC_FLAG         9   /* 0/1, first heat-up or cool-down complete (R7.5)         */
#define TC_DIAG_DEADBAND_REMAIN_MS     10   /* ms before a relay engages; 0 when not counting          */
#define TC_DIAG_AT_SETPT_REMAIN_MS     11   /* ms before the running relay drops; 0 when not counting  */
#define TC_DIAG_COMPARE_REMAIN_MS      12   /* ms to the disagreement fault; 0 when not counting       */
#define TC_DIAG_HEATER_FB_REMAIN_MS    13   /* ms to the heater feedback fault; 0 when not counting    */
#define TC_DIAG_COOLER_FB_REMAIN_MS    14   /* ms to the cooler feedback fault; 0 when not counting    */
#define TC_DIAG_TEMP1_OOR_ACCUM_MS     15   /* sensor 1 leaky accumulator (R5.4); fails at ErrorTimeout */
#define TC_DIAG_TEMP2_OOR_ACCUM_MS     16   /* sensor 2 leaky accumulator                              */
#define TC_DIAG_TEMP1_OOR_EVENTS_PER_HOUR 17 /* in-range -> out-of-range transitions in the last 60 min */
#define TC_DIAG_TEMP2_OOR_EVENTS_PER_HOUR 18
#define TC_DIAG_STATUS_MIRROR          19   /* status from the last CheckTemp (or Init/Reset)          */
#define TC_DIAG_WARNING_MIRROR         20   /* warning from the last CheckTemp (or Init/Reset)         */
#define TC_DIAG_DO_HEATER_MIRROR       21   /* last heater command                                     */
#define TC_DIAG_DO_COOLER_MIRROR       22   /* last cooler command                                     */
#define TC_DIAG_APPLIED_FILTER_POINTS  23   /* FilterPoints actually in use                            */
#define TC_DIAG_ZONE_INITIALIZED       24   /* 1 once a setup has been loaded                          */

/* ------------------------------------------------------------------------ */
/* status codes: 0..5 are states, >= 10 are faults (stand shutdown)          */
/* ------------------------------------------------------------------------ */
#define TC_ST_TEMP_CTRL_DISABLED   0   /* TempCtrlEnable = 0 or no setup loaded; relays 0, nothing evaluated */
#define TC_ST_TEMP_AT_SETPT        1   /* enabled, relays off, ControlTemp inside the deadband        */
#define TC_ST_HEATER_ON            2   /* heating commanded (held through the at-setpoint countdown)  */
#define TC_ST_COOLER_ON            3   /* cooling commanded                                           */
#define TC_ST_HEAT_PENDING         4   /* relays off, below LoBand, deadband countdown running        */
#define TC_ST_COOL_PENDING         5   /* relays off, above HiBand, deadband countdown running        */
#define TC_ST_FAULT_FIRST         10   /* every code >= 10 is a fault                                 */
#define TC_ST_TEMP1_FAIL_HIGH     10   /* single-sensor mode: sensor 1 failed high (incl. NaN/Inf)    */
#define TC_ST_TEMP1_FAIL_LOW      11   /* single-sensor mode: sensor 1 failed low                     */
#define TC_ST_BOTH_SENSORS_FAILED 12   /* two-sensor mode: no healthy sensor left                     */
#define TC_ST_TEMP_DISAGREE_FAULT 13   /* sensors disagreed for TempCompareTimeout                    */
#define TC_ST_CONFIG_FAULT        14   /* Init config check failed with Enable = 1; only a passing Init clears it */
#define TC_ST_HEATER_FB_FAULT     15   /* heater DO feedback mismatched for RelayFeedbackTimeout      */
#define TC_ST_COOLER_FB_FAULT     16   /* cooler DO feedback mismatched for RelayFeedbackTimeout      */

/* ------------------------------------------------------------------------ */
/* warning codes: one value, the lowest active code wins                     */
/* ------------------------------------------------------------------------ */
#define TC_WN_NONE                 0
#define TC_WN_TEMP1_OUT_OF_RANGE   1   /* sensor 1 raw value out of range now                         */
#define TC_WN_TEMP2_OUT_OF_RANGE   2   /* corrected sensor 2 out of range now (Temp2Enable = 1)       */
#define TC_WN_HEATER_FB_MISMATCH   3   /* diHeaterFB != previous heater command                       */
#define TC_WN_COOLER_FB_MISMATCH   4   /* diCoolerFB != previous cooler command                       */
#define TC_WN_TEMP_DISAGREE        5   /* disagreement held for TempCompareTimeout / 10               */
#define TC_WN_RUNNING_ON_TEMP2     6   /* sensor 1 failed, control moved to sensor 2 (until Reset/Init) */
#define TC_WN_CONFIG_INVALID       7   /* Init with Enable = 0 failed the config check                */

/* ------------------------------------------------------------------------ */
/* Functions                                                                 */
/* ------------------------------------------------------------------------ */

/* Library version: (major << 16) | (minor << 8) | patch; 0x030000 for 3.0.0. */
TC_API int32_t TcVersion(void);
/* Required setupArray length (17). */
TC_API int32_t TcSetupCount(void);
/* Required diagArray length (25). */
TC_API int32_t TcDiagCount(void);

/*
 * TcInit - load or replace a zone's setup (also the way to change any
 * parameter at run time). setupArray has TC_SETUP_COUNT elements in the
 * TC_SETUP_* order: 0 TempCtrlEnable, 1 TempUnits, 2 Setpoint, 3 DeadbandHi,
 * 4 DeadbandLo, 5 HiLimit, 6 LoLimit, 7 ErrorTimeout, 8 DeadbandTimeout,
 * 9 AtSetPtTimeout, 10 Temp2Enable, 11 Temp2Offset, 12 Temp2Tolerance,
 * 13 TempCompareTimeout, 14 FilterPoints, 15 FeedbackEnable,
 * 16 RelayFeedbackTimeout.
 *
 * Validates and stores the setup, sets the time reference and clears faults,
 * warnings, averages, accumulators, hourly counts, countdowns and
 * Initial_HC_Flag; ActiveSensor becomes 1 (R9.1). Relay commands are kept
 * only when the zone was running, the new setup has Enable = 1 and passes
 * the config check; otherwise both start at 0 (R9.3). A failed config
 * check with Enable = 1 latches TC_ST_CONFIG_FAULT (only a passing Init
 * clears it); with Enable = 0 it reports TC_WN_CONFIG_INVALID (R9.5).
 * Allowed at any time, including on a stopped zone.
 *
 *   nowMs    LabVIEW Tick Count (ms); free-running, only differences are used
 *   status   TC_ST_* after the call
 *   warning  TC_WN_* after the call
 */
TC_API int32_t TcInit(int32_t zone, uint32_t nowMs,
                      const double* setupArray, int32_t setupLen,
                      int32_t* status, int32_t* warning);

/*
 * TcCheckTemp - one control tick for one zone (nominally every 100 ms).
 *   temp1, temp2   raw scaled temperatures in the configured unit
 *                  (temp2 ignored when Temp2Enable = 0; NaN/Inf count as
 *                  out of range high)
 *   diHeaterFB,    read-back of the heater / cooler digital output, 0/1
 *   diCoolerFB     (ignored when FeedbackEnable = 0)
 *   doHeater,      relay commands for this tick, 0/1, never both 1
 *   doCooler
 *   status         TC_ST_* (>= 10 is a fault: relays 0, latched until Reset/Init)
 *   warning        TC_WN_*
 * A zone with no setup, or with Enable = 0, returns TC_OK with status 0,
 * relays 0 and takes no action (R2). No relay changes state because of one
 * sample: every transition is time-qualified (R7.4).
 */
TC_API int32_t TcCheckTemp(int32_t zone, uint32_t nowMs,
                           double temp1, double temp2,
                           int32_t diHeaterFB, int32_t diCoolerFB,
                           int32_t* doHeater, int32_t* doCooler,
                           int32_t* status, int32_t* warning);

/*
 * TcReset - clear faults, warnings, averages, accumulators, hourly counts,
 * countdowns and Initial_HC_Flag; keep the stored setup; relays 0;
 * ActiveSensor 1; new time reference (R9.2). Does not clear
 * TC_ST_CONFIG_FAULT. Harmless on a zone with no setup.
 */
TC_API int32_t TcReset(int32_t zone, uint32_t nowMs,
                       int32_t* status, int32_t* warning);

/*
 * TcGetDiag - read-only diagnostics for display, logging and CAN packing
 * by the host. diagArray receives TC_DIAG_COUNT values in the TC_DIAG_*
 * order: 0 ControlTemp, 1 ActiveSensor, 2 Temp1Raw, 3 Temp2Raw,
 * 4 Temp2Corrected, 5 Temp1Avg, 6 Temp2Avg, 7 HiBand, 8 LoBand,
 * 9 Initial_HC_Flag, 10 DeadbandRemainMs, 11 AtSetPtRemainMs,
 * 12 CompareRemainMs, 13 HeaterFbRemainMs, 14 CoolerFbRemainMs,
 * 15 Temp1OorAccumMs, 16 Temp2OorAccumMs, 17 Temp1OorEventsPerHour,
 * 18 Temp2OorEventsPerHour, 19 StatusMirror, 20 WarningMirror,
 * 21 doHeaterMirror, 22 doCoolerMirror, 23 AppliedFilterPoints,
 * 24 ZoneInitialized. diagLen must be >= TC_DIAG_COUNT. Never advances
 * state; may be called at any rate.
 */
TC_API int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);

#ifdef __cplusplus
}
#endif
#endif /* TEMPCTL_H */
