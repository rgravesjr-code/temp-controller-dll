# TempCtl v4 - Package Guide (API reference)

`tempctl` is a pure C library with flat `extern "C"` exports for LabVIEW's
Call Library Function Node (CLFN) and any other caller that can load a native
library: C/C++, Python (ctypes), .NET P/Invoke, MATLAB. It ships as
`tempctl.dll` (Windows x64 and x86) and `libtempctl.so` (Linux x86_64 for the
cRIO-904x/905x, 32-bit ARM for the myRIO-1900, aarch64 for the
Raspberry Pi). One header, one ABI, every build.

**Calling convention:** C (cdecl) on all exports.
**Numeric types:** `int32_t`, `uint32_t`, `double` only; no structs, enums,
typedefs, strings, callbacks or 64-bit integers in any signature (written for
the LabVIEW *Import Shared Library* wizard). Booleans are `int32_t`, 0 = false.
**Arrays:** caller-allocated, passed as a pointer plus an `int32_t` length.
**Return value:** `int32_t`; 0 = the call ran, negative = argument error
(nothing ran, no output written).
**Threading:** no locks. Every call for one zone, including `TcGetDiag`,
serialized by the caller; zones independent.
**Determinism:** no allocation, no file or console I/O, no blocking calls,
all state static. The DLL links the CRT statically (imports only
`KERNEL32.dll`); the `.so` files import at most `memset` / `memcpy` from libc.

The behaviour rules are in `TEMPCTL-SPEC-v4.0.0.md` (R-numbered) and, in
prose, `TEMPCTL-CAPABILITY-v4.0.0.md`; the integration reference is
`TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`; this guide is the table reference.

## 1. Exports

```c
int32_t TcVersion(void);       /* (major << 16) | (minor << 8) | patch; 0x040000 = 4.0.0 */
int32_t TcSetupCount(void);    /* 18 = TC_SETUP_COUNT */
int32_t TcDiagCount(void);     /* 28 = TC_DIAG_COUNT  */
int32_t TcInit(int32_t zone, uint32_t nowMs, const double* setupArray, int32_t setupLen,
               int32_t* status, int32_t* warning);
int32_t TcStart(int32_t zone, uint32_t nowMs, int32_t runPermissive,
                int32_t* status, int32_t* warning);
int32_t TcStop(int32_t zone, uint32_t nowMs, int32_t* doHeater, int32_t* doCooler,
               int32_t* status, int32_t* warning);
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs, double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB, int32_t runPermissive,
                    int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning);
int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning);
int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);
```

## 2. Return codes

| Code | Value | Meaning |
|---|---|---|
| `TC_OK` | 0 | The call executed. The controller state is in `status` / `warning`, not here. A refused Start is `TC_OK`. |
| `TC_ERR_ARG` | -1 | Null pointer, `setupLen != 18`, or `diagLen < 28`. Nothing ran; outputs untouched. |
| `TC_ERR_ZONE` | -2 | `zone` outside 0..15. Nothing ran; outputs untouched. |

## 3. Functions

### 3.1 `TcInit` - load or replace the setup

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0 or negative |
| 1 | zone | Numeric I32 | Value | 0..15 |
| 2 | nowMs | Numeric U32 | Value | `Tick Count (ms)` |
| 3 | setupArray | Array, 8-byte Double, 1-D | Array Data Pointer | exactly 18 elements, order in 3.7 |
| 4 | setupLen | Numeric I32 | Value | 18 |
| 5 | status | Numeric I32 | Pointer to Value | status after the call (section 4) |
| 6 | warning | Numeric I32 | Pointer to Value | warning after the call (section 5) |

Call once per zone at start-up and again for any parameter change (the
whole array each time). Validates the setup (R9.5), stores it, sets the time
reference and clears faults, warnings, averages, accumulators, hourly counts,
countdowns, `Initial_HC_Flag` and the lifecycle states; sensor 1 becomes
active; both relay commands are 0 and the zone is **stopped**: a passing
enabled Init returns `IdleStopped` (6) and control runs only after
`TcStart`. A failed check with `Enable = 1` latches `ConfigFault` (14); with
`Enable = 0` it reports warning `ConfigInvalid` (7). Init returns no relay
commands, so on a zone that may be active call `TcStop` first and write its
zeros to the outputs.

### 3.2 `TcStart` - request control

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0 or negative |
| 1 | zone | Numeric I32 | Value | 0..15 |
| 2 | nowMs | Numeric U32 | Value | `Tick Count (ms)` |
| 3 | runPermissive | Numeric I32 | Value | 0 = false, else true |
| 4 | status | Numeric I32 | Pointer to Value | section 4 |
| 5 | warning | Numeric I32 | Pointer to Value | section 5 |

Order of evaluation: disabled or no setup -> `TempCtrlDisabled`; a latched
fault -> kept; already started -> no-op (the permissive is not evaluated);
`runPermissive = 0` -> refused: `IdleStartBlocked` (7) with warning
`OperatingConditionNotMet` (8), or the existing `OperatingConditionPending`
/ `IdleOperatingConditionTripped` state kept; otherwise accepted: Started 1,
countdowns, averages and `Initial_HC_Flag` cleared, time reference set to
`nowMs`, sensor failures / accumulators / hourly counts kept, relays 0
until the next `TcCheckTemp` decides, status `TempAtSetPt` (1)
provisionally. Nothing else ever restarts control.

### 3.3 `TcStop` - normal stop

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0 or negative |
| 1 | zone | Numeric I32 | Value | 0..15 |
| 2 | nowMs | Numeric U32 | Value | `Tick Count (ms)` |
| 3 | doHeater | Numeric I32 | Pointer to Value | always 0: write it to the DO |
| 4 | doCooler | Numeric I32 | Pointer to Value | always 0: write it to the DO |
| 5 | status | Numeric I32 | Pointer to Value | section 4 |
| 6 | warning | Numeric I32 | Pointer to Value | section 5 |

Commands both relays off at once, clears Started and the control /
feedback countdowns, sets the time reference, keeps the setup, sensor
failures, accumulators, hourly counts and any fault. Status `IdleStopped`
from active, idle or blocked; `IdleOperatingConditionTripped` kept from
pending / tripped (the fault countdown is cancelled); a fault and its frozen
warning kept; `TempCtrlDisabled` from a disabled or uninitialised zone. An
ordinary Stop clears the transient warnings 1-5 and 8; `RunningOnTemp2`
persists. Never a fault.

### 3.4 `TcCheckTemp` - one control tick

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0 or negative |
| 1 | zone | Numeric I32 | Value | 0..15 |
| 2 | nowMs | Numeric U32 | Value | `Tick Count (ms)` |
| 3 | temp1 | Numeric DBL | Value | sensor 1, raw, configured unit; NaN = open |
| 4 | temp2 | Numeric DBL | Value | sensor 2 (ignored unless `Temp2Enable`) |
| 5 | diHeaterFB | Numeric I32 | Value | heater DO read-back 0/1 (ignored unless `FeedbackEnable`) |
| 6 | diCoolerFB | Numeric I32 | Value | cooler DO read-back 0/1 |
| 7 | runPermissive | Numeric I32 | Value | the live operating-condition input, 0 = false |
| 8 | doHeater | Numeric I32 | Pointer to Value | heater command 0/1 |
| 9 | doCooler | Numeric I32 | Pointer to Value | cooler command 0/1 (never both 1) |
| 10 | status | Numeric I32 | Pointer to Value | section 4; >= 10 is a fault |
| 11 | warning | Numeric I32 | Pointer to Value | section 5 |

A zone with no setup or with `TempCtrlEnable = 0` returns `TC_OK`, status
0, relays 0 and does nothing. A zone that is not started returns relays 0,
evaluates nothing except a pending permissive countdown, stores the
permissive and mirrors the raw temperatures. A started zone evaluates the
sensor, comparison and feedback faults first (an existing fault maturing on
this tick wins), then a false `runPermissive` de-energizes both relays on
this very sample and enters `OperatingConditionPending` (8); the pending
zone becomes `IdleOperatingConditionTripped` (9) when the permissive
returns before `OperatingConditionTimeout` (no fault, no restart) and
`OperatingConditionFault` (17) when it does not. A faulted zone returns its
fault code with relays 0 until `TcReset` or `TcInit`.

### 3.5 `TcReset` - operator reset

| # | Parameter | CLFN type | Pass |
|---|---|---|---|
| 1 | zone | Numeric I32 | Value |
| 2 | nowMs | Numeric U32 | Value |
| 3 | status | Numeric I32 | Pointer to Value |
| 4 | warning | Numeric I32 | Pointer to Value |

Keeps the setup; clears faults, warnings, averages, accumulators, hourly
counts, countdowns, `Initial_HC_Flag` and the blocked / pending / tripped
states; relays off; sensor 1 active; new time reference; leaves the zone
`IdleStopped`: control resumes only after `TcStart`. Does not clear
`ConfigFault` (only a passing `TcInit` does). Returns no relay commands: on a
non-faulted zone that may be active call `TcStop` first.

### 3.6 `TcGetDiag` - diagnostics

| # | Parameter | CLFN type | Pass |
|---|---|---|---|
| 1 | zone | Numeric I32 | Value |
| 2 | diagArray | Array, 8-byte Double, 1-D | Array Data Pointer (pre-sized to 28) |
| 3 | diagLen | Numeric I32 | Value (28) |

Read-only, any rate, but serialized with the zone's other calls. The 28
values (3.8) are also the CAN message this package defines (section 6).

### 3.7 The setup array (`TC_SETUP_*`)

Booleans: `> 0.1` is 1. Timeouts: whole ms, fractions truncated, minimum 1.

| # | Name | Unit | Meaning | Config check |
|---|---|---|---|---|
| 0 | TempCtrlEnable | 0/1 | master enable (permits Start) | NaN fails |
| 1 | TempUnits | 0/1 | 0 degF, 1 degC, label only | must be 0 or 1 |
| 2 | Setpoint | deg | heating releases at >=, cooling at <= | `LoLimit < LoBand <= Setpoint <= HiBand < HiLimit` |
| 3 | DeadbandHi | deg >= 0 | `HiBand = Setpoint + DeadbandHi` | not negative; not both 0 |
| 4 | DeadbandLo | deg >= 0 | `LoBand = Setpoint - DeadbandLo` | not negative; not both 0 |
| 5 | HiLimit | deg | sensor valid range, top | > HiBand |
| 6 | LoLimit | deg | sensor valid range, bottom | < LoBand |
| 7 | ErrorTimeout | ms | accumulated out-of-range time that fails a sensor; at least two loop periods | >= 1 |
| 8 | DeadbandTimeout | ms | time outside the band before a relay turns on | >= 1 |
| 9 | AtSetPtTimeout | ms | time at the setpoint before the running relay turns off | >= 1 |
| 10 | Temp2Enable | 0/1 | second sensor fitted | NaN fails |
| 11 | Temp2Offset | deg | added to temp2 before every use | finite (only if 10) |
| 12 | Temp2Tolerance | deg >= 0 | largest allowed `|Temp1Avg - Temp2Avg|` | >= 0 (only if 10) |
| 13 | TempCompareTimeout | ms | disagreement time before the fault; warning at 1/10 | >= 1 (only if 10) |
| 14 | FilterPoints | 1..64 | average length for the comparison; anything else -> 4 | never |
| 15 | FeedbackEnable | 0/1 | compare the DO read-backs with the commands | NaN fails |
| 16 | RelayFeedbackTimeout | ms | mismatch time before a relay fault | >= 1 (only if 15) |
| 17 | OperatingConditionTimeout | ms | how long a lost permissive may stay false, after control stopped, before `OperatingConditionFault`; never delays the shutdown | >= 1 |

### 3.8 The diagnostics array (`TC_DIAG_*`)

| # | Name | Meaning |
|---|---|---|
| 0 | ControlTemp | raw value control acts on (sensor 2 corrected); NaN before the first active tick or while the reading is NaN |
| 1 | ActiveSensor | 1 or 2 |
| 2 | Temp1Raw | last temp1 (also while stopped) |
| 3 | Temp2Raw | last temp2 (NaN when Temp2 disabled) |
| 4 | Temp2Corrected | temp2 + Temp2Offset |
| 5 | Temp1Avg | average of in-range temp1 samples (comparison only); NaN until one exists; cleared by Init / Reset / Start |
| 6 | Temp2Avg | average of in-range corrected temp2 samples |
| 7 | HiBand | Setpoint + DeadbandHi |
| 8 | LoBand | Setpoint - DeadbandLo |
| 9 | Initial_HC_Flag | 1 once the first heat-up / cool-down of this run completed (or the zone started in band) |
| 10 | DeadbandRemainMs | ms before a relay engages; 0 when not counting |
| 11 | AtSetPtRemainMs | ms before the running relay drops |
| 12 | CompareRemainMs | ms to the disagreement fault |
| 13 | HeaterFbRemainMs | ms to the heater feedback fault |
| 14 | CoolerFbRemainMs | ms to the cooler feedback fault |
| 15 | Temp1OorAccumMs | sensor 1 leaky accumulator; fails at ErrorTimeout; frozen once failed and while stopped |
| 16 | Temp2OorAccumMs | sensor 2 leaky accumulator |
| 17 | Temp1OorEventsPerHour | in-range -> out-of-range transitions in the last 60 min |
| 18 | Temp2OorEventsPerHour | same for sensor 2 |
| 19 | StatusMirror | status of the last Init / Start / Stop / CheckTemp / Reset |
| 20 | WarningMirror | warning of that call |
| 21 | doHeaterMirror | internal heater command after that call |
| 22 | doCoolerMirror | internal cooler command after that call |
| 23 | AppliedFilterPoints | FilterPoints in use |
| 24 | ZoneInitialized | 1 once a setup has been loaded |
| 25 | RunPermissive | last permissive evaluated by an enabled, non-faulted Start or CheckTemp (0/1); NaN after Init / Reset until then |
| 26 | OperatingConditionRemainMs | ms before a pending permissive loss becomes OperatingConditionFault; 0 when not pending |
| 27 | ControllerStarted | 1 while a Start is accepted and control may run |

## 4. Status codes (`TC_ST_*`)

| Code | Name | Meaning |
|---|---|---|
| 0 | TempCtrlDisabled | disabled or no setup; relays 0, nothing evaluated, Start refused |
| 1 | TempAtSetPt | started, relays off, ControlTemp inside the deadband (provisional right after Start) |
| 2 | HeaterON | heating commanded (held through the at-setpoint countdown) |
| 3 | CoolerON | cooling commanded |
| 4 | HeatPending | relays off, below LoBand, deadband countdown running |
| 5 | CoolPending | relays off, above HiBand, deadband countdown running |
| 6 | IdleStopped | configured and enabled, not started (after Init / Reset / Stop) |
| 7 | IdleStartBlocked | Start refused: permissive false; no countdown |
| 8 | OperatingConditionPending | permissive lost while running; relays off; fault countdown running |
| 9 | IdleOperatingConditionTripped | the loss recovered or was stopped before the timeout; cause latched until Start / Reset / Init |
| 10 | Temp1FailHigh | fault: single-sensor mode, sensor 1 failed high (incl. NaN/Inf) |
| 11 | Temp1FailLow | fault: single-sensor mode, sensor 1 failed low |
| 12 | BothSensorsFailed | fault: two-sensor mode, no healthy sensor left |
| 13 | TempDisagreeFault | fault: sensors disagreed for TempCompareTimeout |
| 14 | ConfigFault | fault: config check failed with Enable = 1; only a passing Init clears it |
| 15 | HeaterFBFault | fault: heater DO feedback mismatched for RelayFeedbackTimeout |
| 16 | CoolerFBFault | fault: cooler DO feedback mismatched for RelayFeedbackTimeout |
| 17 | OperatingConditionFault | fault: permissive stayed false for OperatingConditionTimeout after being lost while running |

Codes 18+ are reserved. Any code >= 10 (`TC_ST_FAULT_FIRST`) is a fault:
relays 0, latched, diagnostics frozen, until `TcReset` or `TcInit`. The
first fault wins; same-tick order: config, sensor range, disagreement,
heater feedback, cooler feedback, operating condition.

## 5. Warning codes (`TC_WN_*`)

One value, the lowest active code wins.

| Code | Name | Set while | Cleared when |
|---|---|---|---|
| 0 | NoWarning | | |
| 1 | Temp1OutOfRange | sensor 1 raw value is out of range now (running) | back in range; Stop / trip |
| 2 | Temp2OutOfRange | corrected sensor 2 is out of range now | back in range; Stop / trip |
| 3 | HeaterFBMismatch | diHeaterFB != previous heater command | they match; Stop / trip |
| 4 | CoolerFBMismatch | diCoolerFB != previous cooler command | they match; Stop / trip |
| 5 | TempDisagree | disagreement held for TempCompareTimeout / 10 | agreement, or the comparison pauses |
| 6 | RunningOnTemp2 | sensor 1 failed, control on sensor 2 | Reset or Init only (masked by 1-5; survives Stop / Start) |
| 7 | ConfigInvalid | Init with Enable = 0 failed the config check | the next passing Init |
| 8 | OperatingConditionNotMet | Start refused by a false permissive, or permissive false while pending / tripped | permissive true again; Stop from blocked; a successful Start; frozen by OperatingConditionFault |

Warnings never change control; while a zone is faulted the warning freezes.
Warning 8 is the lowest priority; the statuses 7, 8, 9 and 17 always carry
the stop / fault meaning.

## 6. Sending the diagnostics on CAN (CanTp)

TempCtl does no CAN. The 28-value diagnostics array is, however, exactly
the message `dbc\tempctl.dbc` and `dbc\tempctl.ecd` define (PGN 65280, id
`0x18FF00FE`, J1939 BAM, **44 bytes**, 8 frames), so the host packs it with
CanTp:

```c
/* once: the message definition from dbc\tables (CanTp_Define) or from the ECD cluster (CanTp_DefineFlat) */
CanTp_Define(0, TempCtl_msgdef, 8, &TempCtl_sigdefs[0][0], TempCtl_NSIG);   /* cantp_tables.h, 28 rows */
int32_t need = CanTp_OutputSize(0);                                           /* 8 frames x 24 = 192 bytes */

/* every tick (or every N ticks) */
TcCheckTemp(0, tick, t1, t2, hfb, cfb, perm, &dh, &dc, &st, &wn);
TcGetDiag(0, diag, 28);
CanTp_Pack(0, diag, 28, timestamp100ns, spacing100ns, frames, need, &written);
/* -> XNET Write (Frame Output Stream, raw), or append to a .ncl after CanTp_NclHeader() */
```

The eight millisecond diagnostics are U16 on the wire (1 ms/bit, 0..64255,
`0xFFFF` = not available); a larger value saturates to 64255 in the frame
while `TcGetDiag` keeps the full value. NaN diagnostics (Temp2 fields while
disabled, ControlTemp during an open sensor, RunPermissive before its
first evaluation) are packed as the J1939 "not available" pattern and decode
as the signal's maximum. Status and warning carry value tables. On the
receiving side `CanTp_Unpack` / `CanTp_RxFeed` return the 28 values in the
same order. In LabVIEW: `Read Delimited Spreadsheet` on
`dbc\tables\TempCtl.msg.csv` (1x8) and `TempCtl.sig.csv` (28x8) ->
`Reshape Array` to 1-D DBL -> `CanTp_Define`; or load `dbc\tempctl.ecd`
with your ECD reader and pass the flattened `TempCtl` cluster to
`CanTp_DefineFlat` (the channels are in `TC_DIAG_*` order, so the array
needs no reordering). **The v3 layout (55 bytes, U32 timers) does not
decode v4 frames.**

## 7. Timing

The library differences successive `nowMs` values (`Tick Count (ms)`), so
a loop period of 10 ms or 1 s gives the same countdown behaviour; countdown
resolution is one loop period. A countdown starts on the tick that first
observes its condition and expires on a later tick when the elapsed time
reaches the timeout, so every timeout gives at least one tick of grace; the
operating-condition countdown follows the same rule. The leaky accumulators
(`TempxOorAccumMs`) charge every out-of-range active tick's elapsed time,
the first one included, and drain half of every in-range tick's elapsed
time: a sensor out of range all the time fails at `ErrorTimeout`, 75 % of
the time at 1.6 x, half the time at about 4 x, a third or less never;
`ErrorTimeout` must be at least two loop periods. Stopped time never feeds
a countdown: `TcStart` re-bases the time reference. A backwards `nowMs`
step counts as 0 ms; 2^32 wrap is handled.
