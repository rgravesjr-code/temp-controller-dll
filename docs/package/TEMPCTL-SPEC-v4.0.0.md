# TempCtl v4.0.0 - Controller Requirements (as implemented)

Status: describes the controller as shipped in TempCtl v4.0.0 (2026-09-22).
v4.0.0 implements the requirements-change handoff of 2026-09-22 (explicit
Start / Stop lifecycle and a live run permissive) on top of v3.0.0 with
Amendment A. The rule numbers are the handoffs': R1..R9 are the v3 rules,
kept unchanged except where the lifecycle gating requires (marked
*amended*), R10 is the v4 lifecycle. Every statement is implemented and
covered by the unit tests (`test_tempctl`, 2186 checks) and the simulator
scenarios (TempSim 3.0.0, 29 scenarios). Words in `code` are the names used
in `tempctl.h` and the simulator. Section 12 lists the decisions the
implementation took where the handoffs left room, for markup.

## 1. Scope and system context

TempCtl is signals-in / signals-out deadband temperature control with sensor
validation and an explicit run lifecycle. It owns no hardware, no CAN, no
clock and no thread.

- Host: a LabVIEW real-time application on a cRIO (NI Linux RT, x86_64) or
  a myRIO-1900 (NI Linux RT, 32-bit ARM). A Windows `.dll` is built from the
  same source for desktop test; an aarch64 `.so` for the Raspberry Pi bench.
- Caller: a single main state-machine loop, nominally 100 ms (10 Hz). Per
  pass the host reads and scales inputs, computes its own operating
  conditions into one **run permissive**, calls TempCtl once per zone,
  processes errors and hands the outputs to parallel loops that drive
  AO/DO/TCP/CAN. Every call for one zone, including `TcGetDiag`, is
  serialized by the host; zones are independent.
- Relay outputs reach the physical DO through a parallel loop, so DO
  feedback lags the command slightly.
- Faults are shutdown events: the host treats any TempCtl fault as a stand
  shutdown. Warnings are informational. The non-fault lifecycle states
  (stopped, blocked, pending, tripped) are neither.
- The host remains the primary decision-maker for operating conditions.
  TempCtl knows nothing plant-specific (no pump names, flows, doors,
  modes): it accepts one combined permissive and provides a backup inhibit,
  clear feedback and a delayed escalation to a fault.
- CAN packing is not part of TempCtl. The host packs the diagnostics array
  with CanTp; this package defines the message (section 4.1).
- Setup values are sanity-checked by the host before download; TempCtl's
  own config check is a backstop, but authoritative at run time.

### 1.1 Key design rule (REQ-7): no relay chatter, with one safety exception

No relay may **energize**, reverse direction or make a normal
process-control transition because of one sample: every such transition is
time-qualified by a timeout. A countdown starts on the tick its condition
is first observed and may expire only on a later tick (`elapsed >=
timeout`), so every positive timeout yields at least one tick of grace.

v4 narrows the v3 wording deliberately (R10.8): a false run permissive
**de-energizes** both outputs on the first sample that observes it. This
cannot chatter, because recovery never re-energizes anything; only a new
explicit Start does. `OperatingConditionTimeout` qualifies the promotion to
a latched fault, never the initial safe shutdown.

## 2. API

All functions are `cdecl`, C linkage, no dependencies beyond libc. No 64-bit
integers, structs, enums or typedefs in any exported signature (LabVIEW
Import Shared Library wizard). Booleans are `int32_t`, 0 = false, any other
value = true.

```c
int32_t TcVersion(void);      /* (major<<16)|(minor<<8)|patch = 0x040000 */
int32_t TcSetupCount(void);   /* 18 */
int32_t TcDiagCount(void);    /* 28 */
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

| Argument | Type | Meaning |
|---|---|---|
| `zone` | int32 | Zone index 0..15. Zones are fully independent and share nothing. |
| `nowMs` | uint32 | LabVIEW *Tick Count (ms)*. Free-running; only differences are used; 2^32 wrap is handled. No wall-clock time is captured (deferred, section 11). |
| `setupArray`, `setupLen` | const double*, int32 | `TcSetupCount()` values, section 3; `setupLen` must equal 18. |
| `runPermissive` | int32 | The host's combined operating-condition input, 0 = false. Evaluated by `TcStart` (once) and by every `TcCheckTemp` (live). |
| `temp1`, `temp2` | double | Raw scaled temperatures in the configured unit. `temp2` is ignored when `Temp2Enable = 0`. |
| `diHeaterFB`, `diCoolerFB` | int32 | Read-back of the heater / cooler digital output, 0/1. Ignored when `FeedbackEnable = 0`. |
| `doHeater`, `doCooler` | int32* | Relay commands, 0/1, never both 1. `TcStop` always returns 0, 0. |
| `status` | int32* | Status code, section 6. |
| `warning` | int32* | Warning code, section 7. |
| `diagArray`, `diagLen` | double*, int32 | Diagnostics buffer; `diagLen >= 28`; elements beyond 28 are not written. |

Return codes (the call result, not the controller state):

| Value | Name | Meaning |
|---|---|---|
| 0 | `TC_OK` | Call executed. A refused (blocked) Start is `TC_OK`; status and warning explain it. |
| -1 | `TC_ERR_ARG` | Null pointer, or `setupLen != 18`, or `diagLen < 28`. |
| -2 | `TC_ERR_ZONE` | `zone` outside 0..15. |

On any negative return nothing runs and no output is written. There is no
"not initialised" error (R9.4).

Normal call pattern:

```
startup / configuration:  TcInit(... setup[18] ...)        -> configured, IdleStopped
start request:            TcStart(... runPermissive ...)   -> accepted, or refused (IdleStartBlocked)
each control tick:        TcCheckTemp(... runPermissive ...)
normal stop:              TcStop(...)                      -> doHeater = doCooler = 0 returned
fault reset:              TcReset(...)                     -> fault cleared, IdleStopped
display / logging / CAN:  TcGetDiag(... diag[28] ...)      -> read-only

active reconfiguration:   TcStop -> apply the zero DOs -> TcInit  -> TcStart
active non-fault reset:   TcStop -> apply the zero DOs -> TcReset -> TcStart
```

`TcInit` and `TcReset` return no relay commands: they clear the library's
internal commands but cannot retract a command the host already delivered
to the physical output. Whenever a zone may be active the host therefore
calls `TcStop` first and applies its zeros. A faulted zone already returned
zeros on its fault tick, so `TcReset` may follow directly.

## 3. Setup array - `TcSetupCount() = 18`

One 1D DBL array. Booleans: `value > 0.1 -> 1`, else 0 (NaN -> 0). Timeouts:
whole milliseconds, fractions truncated, must be >= 1 ms after truncation.
Temperatures are used as supplied. Indexes 0..16 are the v3 array
unchanged; 17 is appended.

| # | Name | Type | Unit | Valid range | Validation (R9.5) |
|---|---|---|---|---|---|
| 0 | `TempCtrlEnable` | bool | - | 0 or 1 | `> 0.1` -> 1; NaN fails the check. The master enable: permits Start, does not start. |
| 1 | `TempUnits` | enum | - | 0 = degF, 1 = degC | must be exactly 0 or 1 |
| 2 | `Setpoint` | double | deg | within limits | band check |
| 3 | `DeadbandHi` | double | deg | >= 0 | not negative; not both deadbands 0 |
| 4 | `DeadbandLo` | double | deg | >= 0 | not negative; not both deadbands 0 |
| 5 | `HiLimit` | double | deg | > HiBand | band check |
| 6 | `LoLimit` | double | deg | < LoBand | band check |
| 7 | `ErrorTimeout` | uint32 | ms | >= 1; host rule: at least two loop periods | < 1 or NaN -> config failure |
| 8 | `DeadbandTimeout` | uint32 | ms | >= 1 | < 1 or NaN -> config failure |
| 9 | `AtSetPtTimeout` | uint32 | ms | >= 1 | < 1 or NaN -> config failure |
| 10 | `Temp2Enable` | bool | - | 0 or 1 | `> 0.1` -> 1; NaN fails |
| 11 | `Temp2Offset` | double | deg | any finite | NaN/Inf -> failure (only when `Temp2Enable = 1`) |
| 12 | `Temp2Tolerance` | double | deg | >= 0 | negative or NaN -> failure (only when `Temp2Enable = 1`) |
| 13 | `TempCompareTimeout` | uint32 | ms | >= 1 | < 1 or NaN -> failure (only when `Temp2Enable = 1`) |
| 14 | `FilterPoints` | int32 | samples | 1..64 | never fails; see R4.2 |
| 15 | `FeedbackEnable` | bool | - | 0 or 1 | `> 0.1` -> 1; NaN fails |
| 16 | `RelayFeedbackTimeout` | uint32 | ms | >= 1 | < 1 or NaN -> failure (only when `FeedbackEnable = 1`) |
| 17 | `OperatingConditionTimeout` | uint32 | ms | >= 1; guidance: longer than the longest expected transient and at least two loop periods | < 1 or NaN -> config failure (checked like the other always-applicable timeouts) |

Derived: `HiBand = Setpoint + DeadbandHi`, `LoBand = Setpoint - DeadbandLo`.
A new parameter is only ever appended; `TcSetupCount()` and `TcVersion()`
change together. `OperatingConditionTimeout` delays the fault
classification of a permissive loss only; it never delays de-energizing.

## 4. Diagnostics array - `TcDiagCount() = 28`

`TcGetDiag` is read-only: it never advances state (no countdown moves), never
changes relays, warnings or a latched status, and may be called at any rate
(or not at all). Indexes 0..24 are the v3 array unchanged; 25..27 are
appended.

| # | Name | Meaning |
|---|---|---|
| 0 | `ControlTemp` | The raw value control acts on (sensor 2 already offset-corrected). NaN before the first active CheckTemp since Init / Reset, and NaN while the active reading is NaN. Holds while stopped. |
| 1 | `ActiveSensor` | 1 or 2. |
| 2 | `Temp1Raw` | Last raw temp1 as supplied by any enabled, non-faulted CheckTemp, active or stopped (NaN before the first). |
| 3 | `Temp2Raw` | Last raw temp2 as supplied (NaN when `Temp2Enable = 0`). |
| 4 | `Temp2Corrected` | `Temp2Raw + Temp2Offset` (NaN when `Temp2Enable = 0`). |
| 5 | `Temp1Avg` | Moving average of in-range temp1 samples, comparison use only. NaN until the first in-range sample; cleared by Init, Reset and Start. |
| 6 | `Temp2Avg` | Moving average of in-range corrected temp2 samples. NaN when disabled or no sample. |
| 7 | `HiBand` | `Setpoint + DeadbandHi`. |
| 8 | `LoBand` | `Setpoint - DeadbandLo`. |
| 9 | `Initial_HC_Flag` | 0/1, R7.5; cleared by Init, Reset and Start. |
| 10 | `DeadbandRemainMs` | Remaining ms before a relay engages; 0 when not counting. |
| 11 | `AtSetPtRemainMs` | Remaining ms before the running relay drops; 0 when not counting. |
| 12 | `CompareRemainMs` | Remaining ms to the disagreement fault; 0 when not counting. |
| 13 | `HeaterFbRemainMs` | Remaining ms to the heater feedback fault; 0 when not counting. |
| 14 | `CoolerFbRemainMs` | Remaining ms to the cooler feedback fault; 0 when not counting. |
| 15 | `Temp1OorAccumMs` | Sensor 1 leaky accumulator (R5.4). Fails the sensor at `ErrorTimeout`; frozen once failed and while stopped. |
| 16 | `Temp2OorAccumMs` | Sensor 2 leaky accumulator. |
| 17 | `Temp1OorEventsPerHour` | Rolling 60-minute count of in-range -> out-of-range transitions (R5.6); ages in wall time while stopped. |
| 18 | `Temp2OorEventsPerHour` | Same for sensor 2. |
| 19 | `StatusMirror` | The status code of the most recent successful stateful call: Init, Start, Stop, CheckTemp or Reset. |
| 20 | `WarningMirror` | The warning code of that call. |
| 21 | `doHeaterMirror` | The internal heater command after that call (0 after Init / Reset / Stop / a trip, although Init and Reset return no DO value). |
| 22 | `doCoolerMirror` | The internal cooler command after that call. |
| 23 | `AppliedFilterPoints` | The value actually in use after R4.2. |
| 24 | `ZoneInitialized` | 1 once a setup has been loaded, else 0. |
| 25 | `RunPermissive` | The most recent permissive actually evaluated by an enabled, non-faulted `TcStart` or `TcCheckTemp`, 0.0 / 1.0. NaN after Init / Reset until such an evaluation. A disabled or faulted call and an idempotent Start while already started do not update it; Stop leaves it unchanged. |
| 26 | `OperatingConditionRemainMs` | Remaining ms before a pending permissive loss becomes `OperatingConditionFault`; 0 when not pending. |
| 27 | `ControllerStarted` | 1 while a Start has been accepted and control is eligible to run; 0 while disabled, stopped, blocked, pending, tripped or faulted. |

A zone with no setup reports `ZoneInitialized = 0`, `ActiveSensor = 1`, NaN
for the temperatures, averages, bands and `RunPermissive`, 0 for everything
else. `TcGetDiag` only reads the mirrors; it never becomes the call they
represent.

### 4.1 CAN wire representation (`tempctl.dbc`, `tempctl.ecd`, CanTp tables)

The C API and the controller's timing are full width (DBL values, `uint32_t`
timers). The following applies to the serialized message only, which the
host produces with CanTp from the 28-value array without reordering:

- One J1939 parameter group, PGN 65280 (`0xFF00`), CAN identifier
  `0x18FF00FE` (priority 6, source-address placeholder 0xFE), J1939 BAM,
  **44 bytes** (7 TP.DT frames plus TP.CM). Signals in `TC_DIAG_*` order,
  Intel byte order, sequential byte-aligned layout.
- Temperatures: 16-bit, 0.03125 deg/bit, -273 offset.
- Every millisecond diagnostic (`DeadbandRemainMs`, `AtSetPtRemainMs`,
  `CompareRemainMs`, `HeaterFbRemainMs`, `CoolerFbRemainMs`,
  `Temp1OorAccumMs`, `Temp2OorAccumMs`, `OperatingConditionRemainMs`):
  **unsigned 16-bit, 1 ms/bit, offset 0**, valid 0..64255 (`0xFAFF`),
  `0xFFFF` = J1939 not available. A finite value above 64255 saturates to
  64255 on the wire; the controller's timer and the DBL returned by
  `TcGetDiag` are not altered.
- Flags 2-bit (0/1, 3 = not available), `Status` and `Warning` 8-bit with
  value tables, counts 16-bit, `ActiveSensor` 4-bit, `AppliedFilterPoints`
  8-bit.
- NaN (Temp2 fields while disabled, `ControlTemp` during an open sensor,
  `RunPermissive` before its first evaluation) packs as all ones and
  decodes as the signal's maximum.
- The v4 layout is **not** wire-compatible with v3 (55 bytes): the U16
  timers move every later signal. v3 DBC / ECD / tables must not decode a
  v4 payload.
- `tempctl.dbc`, the CanTp tables and `tempctl.ecd` are generated from one
  signal model (`tools\make_tempctl_dbc.py`); the ECD's channel order is the
  diagnostic index order, so `CanTp_DefineFlat` on the ECD cluster and
  `CanTp_Define` on the tables produce the same slot and no reorder table is
  needed. The generator asserts the 44-byte length and reads its ECD back
  for comparison; the oracle proves both definitions pack identical bytes.

## 5. Behaviour rules (normative)

### R1 - Interface

- **R1.1** One call per control tick per zone: `TcCheckTemp`. TempCtl holds all state internally; the host holds none.
- **R1.2** 16 independent zones (0..15), sharing nothing; different zones may be interleaved in one loop with independent lifecycle and permissive states.
- **R1.3** Setup is loaded only by `TcInit`. `TcCheckTemp` takes only live signals (temperatures, feedback, run permissive).
- **R1.4** Every stateful call takes `nowMs`. Only differences are used; 2^32 wrap is handled.
- **R1.5** A backwards time step counts as 0 ms elapsed: if `(uint32)(nowMs - lastMs) >= 0x80000000`, elapsed is 0. The same timestamp twice adds 0 ms.
- **R1.6** Single caller thread per zone; no locking. Every call for one zone, including `TcGetDiag`, is serialized by the host. No allocation, no file or console I/O, no blocking calls, no callbacks, no hardware access.
- **R1.7** Builds from one source: Windows x64 and x86 `.dll`, NI Linux RT x86_64 `.so` (cRIO), 32-bit ARM hard-float `.so` (myRIO-1900), aarch64 `.so` (Raspberry Pi bench). The same `tempctl.h` ships with every binary.

### R2 - Enable (*amended*: the enable permits Start, it does not start)

- **R2.1** `TempCtrlEnable` (setup 0) is the master enable for a zone. When false (or no setup exists) the zone is inert and cannot be started.
- **R2.2** While disabled TempCtl takes no action: `doHeater = doCooler = 0`, no sensor, permissive or control checks, no countdowns, no averaging, no faults; `status = 0 (TempCtrlDisabled)`. The only warning a disabled zone reports is `ConfigInvalid` (R9.5). A Start attempt returns `TC_OK` with `TempCtrlDisabled` (and `ConfigInvalid` when that warning is active); it is not an argument error and records nothing. A Stop returns zeros, `TempCtrlDisabled` and the same warning rule; on a zone with no setup it returns `NoWarning`.
- **R2.3** Power-on state of every zone is disabled, because no setup is loaded.
- **R2.4** `TcCheckTemp`, `TcStart`, `TcStop` or `TcReset` on a zone with no setup is a harmless no-op returning `TC_OK` with `status = TempCtrlDisabled`.

### R3 - Units

- **R3.1** `TempUnits` is a label only (0 = degF, 1 = degC). TempCtl performs no conversion.
- **R3.2** All temperature quantities are supplied in that unit by the host.

### R4 - Signal handling and averaging

- **R4.1** Limit checks and control act on raw values. Averaging is used only for the Temp1-vs-Temp2 comparison (R6).
- **R4.2** `FilterPoints` selects the average length. A value in 1..64 is used as given (fractions truncated). Any other value (0, negative, > 64, NaN, Inf) becomes 4, with no warning and no fault.
- **R4.3** Only in-range raw samples enter an average. Sensor 2's average uses the corrected value. Samples arrive only on active ticks (R10.6).
- **R4.4** NaN or Inf (either sign) on a temperature input is treated as out of range high for the limit check. It never enters an average and never becomes a numeric `ControlTemp`.
- **R4.5** Init, Reset and an accepted Start clear the averages (*amended*: Start added, R10.3).

### R5 - Sensor range checking

- **R5.1** A sensor is out of range when its value (sensor 2: corrected) is `> HiLimit` or `< LoLimit`, including R4.4. Both limits apply to both sensors.
- **R5.2** Sensor 2 is checked only when `Temp2Enable = 1`.
- **R5.3** A sensor that has already failed is not re-evaluated for failure. Its range is still evaluated, but only to drive the warning code; its accumulator is frozen at the failure value.
- **R5.4 (leaky accumulator, Amendment A)** Each sensor holds an out-of-range accumulator in ms with `DRAIN = 0.5`: out of range this active tick: `accum += elapsed`; in range: `accum = max(0, accum - 0.5 x elapsed)`; `accum >= ErrorTimeout` -> the sensor has failed (R5.5). Every out-of-range active tick charges its elapsed time, including the first one (the one-tick grace of section 1.1 applies to the countdowns only). For a sensor out of range a fraction `d` of the time the accumulator grows at `d - 0.5(1 - d)` per ms, so with `E = ErrorTimeout`: 100 % duty fails at 1 x E, 75 % at 1.6 x E, 50 % at 4 x E, 33 % or less never. Kept in half-millisecond units internally. **Host rule:** set `ErrorTimeout` to at least two loop periods. The accumulator neither charges nor drains while the zone is not started (R10.6); Stop and Start preserve it.
- **R5.5 (effect of a failure)** `Temp2Enable = 0`: sensor 1 failed -> fault `Temp1FailHigh` or `Temp1FailLow` by the condition at the moment of failure. `Temp2Enable = 1`, one sensor failed, the other healthy -> no fault; control uses the healthy sensor; warning `RunningOnTemp2` when the switch was away from sensor 1; TempCtl never switches back on its own, and normal Stop / Start never heals a failed or degrading sensor. `Temp2Enable = 1`, both failed -> fault `BothSensorsFailed`.
- **R5.6 (health metric)** Each sensor keeps a rolling 60-minute count of in-range -> out-of-range transitions (`TempxOorEventsPerHour`): 60 one-minute buckets in a static ring buffer, advanced by elapsed wall time on every enabled non-faulted tick (active or stopped) and by the stopped gap at an accepted Start, so events expire 60 minutes after they happened regardless of Stop / Start. Init and Reset clear it; Stop and Start preserve it. A sensor that is out of range on its first active tick after Init, Reset or Start counts one transition.
- **R5.7 (control while out of range)** While the active sensor's value is currently out of range, the deadband and at-setpoint checks pause: relay commands and the status code hold their present state and both countdowns freeze (not reset). Control resumes when the value returns in range; the failure action happens when the accumulator reaches `ErrorTimeout`. An out-of-range condition on the non-active sensor does not affect control.

### R6 - Two-sensor comparison

- **R6.1** `Temp2Corrected = temp2 + Temp2Offset`, used for the limit check, the comparison, control after a switch and the reported Temp2 values.
- **R6.2** The comparison runs only on active ticks when: `Temp2Enable = 1`, neither sensor has failed, `Initial_HC_Flag = 1`, both averages hold `FilterPoints` samples, neither sensor is currently out of range, and no fault is latched. It is evaluated before the control step of a tick, so it first runs on the tick after `Initial_HC_Flag` was set.
- **R6.3** Disagreement condition: `|Temp1Avg - Temp2Avg| > Temp2Tolerance` (exactly the tolerance agrees).
- **R6.4 (two stages)** Held for `TempCompareTimeout / 10` (integer division) -> warning `TempDisagree`, control continues. Held for the full `TempCompareTimeout` -> fault `TempDisagreeFault`. Agreement at any point clears the warning immediately and resets the countdown. `TempCompareTimeout / 10 == 0` -> the warning appears on the first qualifying tick.
- **R6.5** If either sensor goes out of range (or any other R6.2 condition stops holding, including Stop and a permissive trip), the comparison pauses; when it qualifies again it restarts from zero (countdown and warning reset).
- **R6.6** Neither sensor is voted correct. A sustained disagreement always ends in a fault.

### R7 - Control (deadband)

All decisions use `ControlTemp`: the raw active-sensor value, offset-corrected when the active sensor is 2. Control runs only while started (R10.6).

- **R7.1 (engage)** Idle and `ControlTemp > HiBand` continuously for `DeadbandTimeout` -> cooling on. Idle and `ControlTemp < LoBand` continuously for `DeadbandTimeout` -> heating on. Re-entering the band clears the countdown; crossing to the other side restarts it. The band edges are inside the band.
- **R7.2 (release)** A running relay drops only after the at-setpoint condition has held for `AtSetPtTimeout`: `ControlTemp >= Setpoint` while heating, `ControlTemp <= Setpoint` while cooling. Breaking the condition resets that countdown. On the tick the relay drops, the idle logic runs at once.
- **R7.3** Heating and cooling are mutually exclusive. While a relay is on, the deadband countdown is idle (`DeadbandRemainMs = 0`).
- **R7.4 (*amended*)** No relay may energize, reverse direction or make a normal process-control transition on a single sample, in any state (REQ-7). The only single-sample transition is the de-energization on a false run permissive (R10.8) and the de-energization by Stop.
- **R7.5 (`Initial_HC_Flag`)** Cleared by Init, Reset and an accepted Start. Set when either the at-setpoint condition completes its `AtSetPtTimeout` (the relay drops), or the zone is active and idle with `LoBand <= ControlTemp <= HiBand` on any tick, including the first active tick after a Start. It gates the comparison (R6.2).

### R8 - Relay (DO) feedback

- **R8.1** `diHeaterFB` / `diCoolerFB` are read-backs of the digital output, not the physical relay contact (documented gap, section 11).
- **R8.2 (*amended*)** The check runs only when `FeedbackEnable = 1` and only on active ticks: never while the zone is disabled, stopped, blocked, pending, tripped or faulted (R10.9).
- **R8.3 (*amended*)** Each feedback is compared with that relay's command from the previous `TcCheckTemp` call for this zone. After Init, Reset, Stop, an accepted Start and a permissive trip the previous command is 0, so the deliberate command-to-zero transition of those events is never a mismatch, while a relay that is physically still closed when control restarts is caught on the first active tick.
- **R8.4** Per relay: a mismatch raises its warning (`HeaterFBMismatch` / `CoolerFBMismatch`) immediately and starts the `RelayFeedbackTimeout` countdown. A match clears the warning and resets the countdown. Reaching the timeout raises that relay's fault.
- **R8.5** `RelayFeedbackTimeout` must exceed the worst-case host DO-loop latency (see `LABVIEW_INTEGRATION.md`).

### R9 - Init, Reset and faults (*amended* for the lifecycle; see also R10.2, R10.5)

- **R9.1 (Init)** Validates and stores the setup (R9.5), sets the time reference to `nowMs`, clears faults, warnings, averages, accumulators, hourly counts, countdowns, `Initial_HC_Flag` and the lifecycle states, sets `ActiveSensor = 1`, clears Started and sets both internal relay commands and previous commands to 0. Allowed at any time. A passing enabled Init returns `IdleStopped`; a passing disabled Init `TempCtrlDisabled`; a failed check `ConfigFault` (enabled) or `TempCtrlDisabled` with `ConfigInvalid` (disabled). Init never keeps a relay and never starts control.
- **R9.2 (Reset)** Keeps the stored setup. Clears faults, warnings, averages, accumulators, hourly counts, countdowns, `Initial_HC_Flag` and the non-fault lifecycle states, sets `ActiveSensor = 1`, Started = 0 and both relay commands to 0, and sets the time reference. It does not clear `ConfigFault`. A valid enabled zone is left at `IdleStopped`; control does not resume without Start.
- **R9.3 (removed)** The v3 relay keeping across a re-Init is gone. Every Init starts with both relays at 0 (R10.2).
- **R9.4** No "not initialised" error: a call on a zone with no setup is a no-op (R2.4).
- **R9.5 (config check, at Init)** Checks: `TempUnits` is 0 or 1; `DeadbandHi >= 0`, `DeadbandLo >= 0`, not both zero; `LoLimit < LoBand <= Setpoint <= HiBand < HiLimit`; every applicable timeout >= 1 ms, `OperatingConditionTimeout` included; no NaN in any applicable setup value; when `Temp2Enable = 1`: `Temp2Tolerance >= 0`, `Temp2Offset` finite, `TempCompareTimeout >= 1`; when `FeedbackEnable = 1`: `RelayFeedbackTimeout >= 1`. `FilterPoints` is never checked. Parameters of a disabled feature are not checked. `Enable = 1` and a check fails -> `ConfigFault`: relays 0, latched; only a subsequent Init that passes clears it (Reset, Start and Stop do not). `Enable = 0` and a check fails -> warning `ConfigInvalid`; the zone stays disabled.
- **R9.6 (fault behaviour)** On any fault: `doHeater = doCooler = 0`, Started = 0; the fault code latches into `status`; the warning code freezes at the value computed on the fault tick; every check, countdown, accumulator and average stops (the diagnostics freeze, `RunPermissive` included). Only Reset or Init resumes the zone; Start and Stop on a faulted zone return the fault and its frozen warning and change nothing. The first fault wins. If two conditions mature on the same tick the order is: config -> sensor range -> disagreement -> heater feedback -> cooler feedback -> operating condition.
- **R9.7** Faults never self-clear, and failed sensors are never re-admitted.

### R10 - Lifecycle and run permissive (new in v4)

- **R10.1 (master enable)** `TempCtrlEnable` remains the configuration-level permission. When false or no setup exists, Start cannot succeed (R2.2) and CheckTemp is inert.
- **R10.2 (Init)** As R9.1: a passing enabled Init leaves the zone configured but stopped, `IdleStopped`, relays 0, Started 0, `RunPermissive` NaN. Because Init returns no DO values, the host sequence for a zone that may be active is `TcStop` -> apply the returned zeros -> `TcInit`. A direct Init is fine for an uninitialised, disabled, faulted or known-idle zone.
- **R10.3 (Start)** `TcStart` evaluates, in this order: (1) zone and pointers; (2) no setup or disabled -> `TC_OK`, `TempCtrlDisabled`; (3) a latched fault -> preserved with its frozen warning; (4) already started -> unconditional idempotent no-op returning the current status and warning: the permissive is neither evaluated nor recorded, no countdown, average, command or time reference changes; (5) `OperatingConditionPending` and `runPermissive = 0` -> pending preserved with its accumulated countdown and time reference (only CheckTemp advances the countdown); (6) `IdleOperatingConditionTripped` and `runPermissive = 0` -> the trip status is kept (not replaced by `IdleStartBlocked`) with warning `OperatingConditionNotMet` subject to priority; (7) any other stopped non-fault state and `runPermissive = 0` -> commands 0, Started 0, `IdleStartBlocked`, warning 8 subject to priority; (8) otherwise the Start is accepted. In cases 5-8 the evaluated permissive is recorded in `RunPermissive`. An accepted Start sets Started = 1, records the true permissive, keeps both commands 0 until the next CheckTemp decides, sets the time reference to `nowMs` (stopped time never feeds a countdown), clears the deadband, at-setpoint, comparison, relay-feedback and operating-condition countdowns, clears the moving averages and `Initial_HC_Flag`, preserves the setup, the latched sensor failure / failover, the accumulators and the hourly history, clears a blocked or tripped status, clears warning 8 and recomputes the warning from any persistent condition (`RunningOnTemp2` survives), and returns `TempAtSetPt` provisionally until the first CheckTemp. There is no automatic retry: a blocked or tripped zone starts only from a new successful `TcStart`. Start-while-started is checked before the permissive on purpose: `TcCheckTemp`, which returns the DO commands, is the only call that acts on a newly false live permissive.
- **R10.4 (Stop)** `TcStop` validates the zone and all four output pointers, commands and returns `doHeater = doCooler = 0` at once, sets Started = 0 and the previous commands to 0, cancels the deadband, at-setpoint, comparison and relay-feedback countdowns, sets the time reference, preserves the setup, sensor failures, accumulators, hourly history and any latched fault. Status: an uninitialised zone -> `TempCtrlDisabled` / `NoWarning`; a disabled zone -> `TempCtrlDisabled` / `ConfigInvalid` only when active; a faulted zone -> the fault and frozen warning unchanged; `IdleStopped` stays; active operation -> `IdleStopped`; `OperatingConditionPending` or `IdleOperatingConditionTripped` -> the permissive countdown is cancelled and `IdleOperatingConditionTripped` set or retained so the reason is not erased; `IdleStartBlocked` -> Stop is the acknowledgment: `IdleStopped`, warning 8 cleared. Warnings: a faulted zone keeps its frozen warning; Stop from pending / tripped keeps warning 8 while the last evaluated permissive was false (a later tripped-state CheckTemp with a true permissive, or a successful Start, clears it); an ordinary Stop clears the transient warnings 1-5 and 8; `RunningOnTemp2` persists; `ConfigInvalid` applies to a disabled zone only. `RunPermissive` is unchanged (Stop has no permissive input). Stop never creates or clears a fault.
- **R10.5 (Reset)** As R9.2: Started = 0, both commands 0, non-fault lifecycle states cleared, `RunPermissive` NaN, a valid enabled zone at `IdleStopped`. Reset does not claim Start is blocked; a later Start evaluates the current permissive. Because Reset returns no DO values, a non-faulted zone that may be active is stopped first (`TcStop` -> apply zeros -> `TcReset`); a faulted zone already returned zeros.
- **R10.6 (CheckTemp while not started)** When the zone is valid and enabled but Started = 0: both commands are returned as 0; sensor, averaging, comparison, control and relay-feedback logic do not advance (accumulators frozen, no warnings 1-5); the supplied permissive is stored in `RunPermissive`; the raw inputs are mirrored into `Temp1Raw` / `Temp2Raw` / `Temp2Corrected` (display only); the hourly rings age by the elapsed time; `IdleStopped`, `IdleStartBlocked` or `IdleOperatingConditionTripped` is preserved; warning 8 follows the current permissive for the blocked and tripped states only (never for `IdleStopped`); no operating-condition countdown starts because a stopped zone sees a false permissive. `OperatingConditionPending` is the one exception: its countdown advances through `TcCheckTemp` even though Started is 0 (R10.7).
- **R10.7 (loss of the permissive during active control)** On an active tick: (1) the existing sensor, comparison and relay-feedback evaluation runs first, so an existing fault that matures on this tick wins (R9.6 order); (2) if no fault won and `runPermissive = 0`, both outputs are commanded 0 immediately; (3) Started = 0; (4) `OperatingConditionPending`; (5) warning 8 subject to priority; (6) the `OperatingConditionTimeout` countdown is observed with the full remaining value (this tick's elapsed time is not charged); (7) the control and feedback countdowns and the transient warning conditions 1-5 are cleared, so the intentional de-energization cannot create a relay-feedback fault. While pending: sensor, averaging, comparison, control and feedback processing are skipped; a false permissive advances only the operating-condition countdown; a true permissive cancels it and moves to `IdleOperatingConditionTripped`; recovery never restarts control; expiry (elapsed >= timeout on a later false tick) latches `OperatingConditionFault` and freezes the warning by the normal fault rule. The countdown uses the same wrap-safe and backwards-time handling as every other countdown.
- **R10.8 (deliberate safety exception to the one-sample rule)** See section 1.1: a false run permissive may de-energize both outputs on its first observed sample; no relay may energize, reverse or make a process transition on one sample; recovery re-energizes nothing; the timeout qualifies the fault promotion only.
- **R10.9 (feedback after an intentional stop)** The command-to-zero transition of Init, Stop, Reset or a permissive loss is never a feedback mismatch: the previous-command mirrors are set to 0 consistently and feedback evaluation is suppressed while disabled, stopped, pending, tripped or faulted (R8.2, R8.3).

Status precedence, highest first: invalid call (negative return, nothing
written); no setup or disabled (`TempCtrlDisabled`); a previously latched
fault; an existing controller fault maturing on the current active tick;
`OperatingConditionFault`; the pending / tripped / blocked states;
`IdleStopped`; the normal active-control state.

## 6. Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `TempCtrlDisabled` | `TempCtrlEnable = 0`, or no setup loaded. Relays 0; nothing evaluated; Start refused. |
| 1 | `TempAtSetPt` | Started, relays off, `ControlTemp` inside the deadband (also the provisional status right after an accepted Start). |
| 2 | `HeaterON` | Heating commanded (held through the at-setpoint countdown). |
| 3 | `CoolerON` | Cooling commanded. |
| 4 | `HeatPending` | Relays off, below `LoBand`, deadband countdown running. |
| 5 | `CoolPending` | Relays off, above `HiBand`, deadband countdown running. |
| 6 | `IdleStopped` | Valid enabled setup, not started: after Init, Reset, a normal Stop, or Stop acknowledging a blocked Start. Relays 0. |
| 7 | `IdleStartBlocked` | A Start was refused because `runPermissive = 0`. Relays 0, no countdown; a new Start is required. |
| 8 | `OperatingConditionPending` | The permissive was lost during active control: relays 0, the `OperatingConditionTimeout` countdown is running. |
| 9 | `IdleOperatingConditionTripped` | A permissive loss stopped control and then recovered, or was stopped, before the timeout. The cause stays latched; a new Start is required. |
| 10 | `Temp1FailHigh` | Fault. Single-sensor mode: sensor 1 failed high (includes NaN/Inf). |
| 11 | `Temp1FailLow` | Fault. Single-sensor mode: sensor 1 failed low. |
| 12 | `BothSensorsFailed` | Fault. Two-sensor mode: no healthy sensor remains. |
| 13 | `TempDisagreeFault` | Fault. Sensors disagreed for `TempCompareTimeout`. |
| 14 | `ConfigFault` | Fault. Init config check failed with `Enable = 1`. Cleared only by a passing Init. |
| 15 | `HeaterFBFault` | Fault. Heater DO feedback mismatched for `RelayFeedbackTimeout`. |
| 16 | `CoolerFBFault` | Fault. Cooler DO feedback mismatched for `RelayFeedbackTimeout`. |
| 17 | `OperatingConditionFault` | Fault. The permissive stayed false for `OperatingConditionTimeout` after being lost during active control. |
| 18+ | reserved | |

Any code >= 10 (`TC_ST_FAULT_FIRST`) is a fault, and the host treats it as a
stand shutdown. Codes 0-9 are not faults. Status is the authoritative
indication of the controller's state and of the retained cause of a stop;
the warning is secondary. While paused (R5.7) the code holds its pre-pause
value.

## 7. Warning codes

One value; the lowest active code wins.

| Code | Name | Sets | Clears |
|---|---|---|---|
| 0 | `NoWarning` | - | - |
| 1 | `Temp1OutOfRange` | Sensor 1's raw value is out of range now (failed or not), on an active tick | Sensor 1 back in range; Stop or a permissive trip |
| 2 | `Temp2OutOfRange` | Same for sensor 2's corrected value (`Temp2Enable = 1`) | Sensor 2 back in range; Stop or a trip |
| 3 | `HeaterFBMismatch` | `diHeaterFB` != previous heater command | They match; Stop or a trip |
| 4 | `CoolerFBMismatch` | `diCoolerFB` != previous cooler command | They match; Stop or a trip |
| 5 | `TempDisagree` | Disagreement held for `TempCompareTimeout / 10` | Agreement, or the comparison pauses (incl. Stop, trip) |
| 6 | `RunningOnTemp2` | Sensor 1 failed and control switched to sensor 2 | Reset or Init only; persists through Stop and Start; masked by codes 1-5 while they are active |
| 7 | `ConfigInvalid` | Init with `Enable = 0` failed the config check | The next Init that passes |
| 8 | `OperatingConditionNotMet` | A Start is refused by a false permissive, or the permissive is false while the zone is pending or tripped | The live permissive becomes true (a tripped-state CheckTemp, or a successful Start); Stop from a blocked Start clears it; a latched `OperatingConditionFault` freezes it |

Warnings never change control. Warning 8 is the lowest priority and may be
masked by 1-7 (in practice by `RunningOnTemp2`, since 1-5 are cleared when
evaluation stops); the statuses 7, 8, 9 and 17 always carry the
stop / fault meaning. Warning 8 is never latched on its own: the status
holds the cause. While a zone is stopped on a fault, the warning freezes.

## 8. Change list vs v3.0.0 (handoff section 18)

| # | Change | Intent | v3.0.0 | v4.0.0 |
|---|---|---|---|---|
| V4-1 | Explicit Start | Separate run lifecycle from enable / configuration without bypassing a pending trip | control ran as soon as an enabled setup existed | `TcStart`, R10.3 |
| V4-2 | Explicit Stop | Safe shutdown without a fault or re-Init | none (disable by Init) | `TcStop`, R10.4 |
| V4-3 | Live permissive | Generic host-supplied backup operating-condition check | none | `runPermissive` on Start and CheckTemp |
| V4-4 | Blocked Start | Prevent activation under invalid operating conditions | none | `IdleStartBlocked`, warning 8, no countdown |
| V4-5 | Immediate trip | Conservative de-energization | none | R10.7 step 2, R10.8 |
| V4-6 | Delayed condition fault | Ignore a momentary loss as a fault while retaining the cause | none | `OperatingConditionTimeout`, `OperatingConditionPending`, `OperatingConditionFault` |
| V4-7 | Latched non-fault cause | Preserve why control stopped | none | `IdleOperatingConditionTripped` until Start / Reset / Init |
| V4-8 | Fault precedence | Existing faults are never hidden or overwritten | 5-way order | 6-way order ending in operating condition |
| V4-9 | Revised Init / Reset | Never run without Start; the host applies zero DOs before an active reconfigure / reset | Init kept relays (R9.3), Reset resumed on the next tick | R9.1, R9.2, R10.2, R10.5; R9.3 removed |
| V4-10 | Diagnostics | Make the lifecycle observable and transportable | 25 values | 28 values, section 4 |
| V4-11 | Header per package | Self-contained LabVIEW import | one header at the root | the same `tempctl.h` in every target folder, byte-identical, in the manifest |
| V4-12 | myRIO support | 32-bit ARM NI Linux RT | x86_64 and aarch64 only | `linux-armhf` build (Cortex-A9, hard float) |
| V4-13 | Updated TempSim | Independent scenario validation | TempSim 2.x, 16 scenarios | TempSim 3.0.0, 29 scenarios |
| V4-14 | User reference | Prevent integration rediscovery | guides split | `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md` |
| V4-15 | Compact ECD / DBC | Smaller CAN payload, full-width controller timing | 55-byte message, U32 timers | 44-byte message, U16 timers with saturation, generated `tempctl.ecd` |

## 9. Build and packaging

1. LabVIEW-importable header: only `int32_t`, `uint32_t`, `double`; arrays as pointer + `int32_t` length; no structs, enums, typedefs or 64-bit integers; a `#define` name for every index and code; a doc comment per function listing the array indexes and the direction of every argument. The same file ships next to every binary (root, `x86\`, `linux-x64\`, `linux-armhf\`, `linux-arm64\`, `src\`), byte-identical, hashed in the manifest. The wizard run itself (32-bit LabVIEW 2026 against the x86 DLL) is the LabVIEW side's acceptance step.
2. Targets from one source: `tempctl.dll` x64 and x86 (MSVC, static CRT, imports `KERNEL32.dll` only); `libtempctl.so` linux-x64 (cRIO, glibc 2.2.5+), linux-armhf (myRIO-1900, EABI v5 hard float, Cortex-A9, no libc import), linux-arm64 (Raspberry Pi, glibc 2.17+); a `test_tempctl` for each. `DEPENDENCIES.txt` records format, architecture, exports, imports and SONAME.
3. TempSim 3.0.0 drives the v4 API and runs every scenario of section 10 with built-in expectations (`SIMULATOR.md` in the TempSim package).
4. Unit tests cover every rule of section 5 and every row of section 8 (`TESTING.md`).
5. Determinism: no heap allocation, I/O, blocking or callbacks in any call; all state static (16 zones x 2 sensors x 60 ring entries + 64-sample averages).
6. `TcVersion()` returns `0x040000`; `TcSetupCount()` 18; `TcDiagCount()` 28.
7. The DBC, the CanTp tables and the ECD are generated from one signal model and cross-checked (section 4.1); the oracle proves the wire format.
8. Shipped documentation: this file, `TEMPCTL-CAPABILITY-v4.0.0.md`, `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`, `TEMPCTL_PACKAGE_GUIDE.md`, `LABVIEW_INTEGRATION.md`, `TESTING.md`, `DISTRIBUTION_README.md`, `CHANGELOG.md`.

## 10. Test scenarios (unit tests and TempSim)

v3 scenarios, under the v4 lifecycle (Start at t = 0): S1 heat-up; S2
setpoint change, now the Stop -> Init -> Start form (`reconfigure-stop-init`);
S3 cool-down; S4 chatter; S5 flicker (and `flicker-25`); S6 control pause; S7
failover; S8 disagreement; S9 gating; S10 relay feedback; S11 config
faults; S12 enable / disable; S13 reset; S14 two zones; S15 time.

v4 scenarios (handoff section 14): V4-1 `idle-before-start`; V4-2
`start-heat-cool`; V4-3 `stop-from-active` (HeatPending, HeaterON,
CoolPending, CoolerON); V4-4 `blocked-start`; V4-5 `permissive-trip`; V4-6
`permissive-recover`; V4-7 `permissive-fault`; V4-8 `stop-while-pending`;
V4-9 `permissive-fault-priority`; V4-10 `reset-stays-idle`; V4-11
`reconfigure-stop-init`; V4-12 `reset-stop-first`; V4-13
`start-while-pending`; V4-14 `two-zones-lifecycle`.

## 11. Known coverage gaps and deferred items (documented, not implemented)

1. Physically stuck relay: feedback reads the DO state, so a welded contact with a correct DO is not detected.
2. No heating or cooling progress: a heater that never reaches the setpoint stays on indefinitely; no max-on-time fault.
3. Wrong-but-in-range sensor: after one sensor has failed the survivor is no longer cross-checked.
4. Single warning output: only the lowest-numbered warning is visible; no warning bitmask.
5. No process over-temperature independent of sensor validity.
6. No wall-clock / calendar timestamp for faults or stop events (`nowMs` is the wrapping tick count; the host adds its own timestamp when it observes a status transition).
7. No individual operating-condition names or reason bits; v4 accepts the combined generic permissive only.
8. No automatic restart after permissive recovery.
9. Sensor health is not evaluated while a zone is stopped (V4-D1): a probe that fails while `IdleStopped` is discovered after the next Start.
10. The host and the physical I/O layer remain responsible for watchdogs and fail-safe outputs if the application stops calling the library.

## 12. Implementation decisions (for markup)

Carried from v3.0.0 (I1-I9 apply unchanged except where noted):

- **I1 / I2** Amendment A: `DRAIN = 0.5`; every out-of-range active tick charges, the first included; the one-tick grace applies to the countdowns only.
- **I3** Status holds while paused (R5.7).
- **I4** The comparison first qualifies on the tick after `Initial_HC_Flag` is set.
- **I5 (*changed*)** Before the first CheckTemp a valid enabled zone reports `IdleStopped` after Init or Reset and `TempAtSetPt` after an accepted Start; the first active CheckTemp corrects it. Relays are never kept, so `HeaterON` / `CoolerON` never appear before a CheckTemp.
- **I6** A timeout that truncates to 0 fails the config check; 1 ms is the smallest accepted value.
- **I7** A failed sensor's accumulator is frozen at the failure value.
- **I8** A sensor out of range on its first active tick after Init, Reset or Start counts one transition.
- **I9** The frozen warning is the one computed on the fault tick.

New in v4.0.0 (decided with the owner on 2026-09-22, plan V4-D1..D8, and during implementation, V4-D9..D12):

- **V4-D1 (stopped ticks)** While Started = 0 nothing is evaluated, as R10.6 says, but the raw inputs are still mirrored into `Temp1Raw` / `Temp2Raw` / `Temp2Corrected` so displays stay live. `ControlTemp`, the averages, the accumulators and warnings 1-5 freeze. A sensor that fails while stopped is therefore detected only after the next Start.
- **V4-D2 (RunPermissive on a refused Start)** Start steps 5-7 do evaluate the permissive and record it; only the idempotent already-started Start and disabled / faulted calls do not. R10.4's "last evaluated permissive" rule for warning 8 after a Stop relies on this.
- **V4-D3 (ECD)** `tempctl.ecd` is generated here from the same signal model as the DBC, channels in `TC_DIAG_*` order, so there is no reorder table; the generator reads it back with CanTp's own reader and compares every channel with the DBC and the tables.
- **V4-D4 (array lengths)** `setupLen` must equal 18 (exact, as in v3); `diagLen` must be >= 28 and elements beyond 28 are not written.
- **V4-D5 (target execution)** The cRIO and myRIO are stretch targets in this release: the x86-64 and 32-bit ARM binaries are built and ELF-inspected here; the package records whether each was executed on its target (a dated log in `docs\testlogs\`) or only inspected. This relaxes the handoff's section 17 items 4, 7 and 13 by owner decision.
- **V4-D6 (LabVIEW gates)** Wrapper generation and the LabVIEW / LabVIEW RT tests are owner-executed; the package ships the header, the parser settings and a checklist.
- **V4-D7 (ARM glibc floor)** 2.24 for the zig target (`arm-linux-gnueabihf.2.24`, `-mcpu=cortex_a9`); the resulting library imports nothing from libc and the test executable needs glibc 2.4.
- **V4-D8 (TempSim)** 3.0.0, a major release; the simulator issues `TcStart` after `TcInit` by default so every earlier scenario still controls from t = 0.
- **V4-D9 (warnings on a trip)** The permissive trip clears the transient warning conditions 1-5 the way Stop does, because evaluation stops on that tick; the trip tick and the pending ticks therefore report warning 8 unless `RunningOnTemp2` masks it.
- **V4-D10 (history while stopped)** The hourly rings age in wall time while stopped (stopped ticks advance them; an accepted Start advances them by the gap since the last call), so `TempxOorEventsPerHour` keeps its 60-minute meaning across a Stop. The leaky accumulators neither charge nor drain while stopped.
- **V4-D11 (Start / Stop on a disabled zone)** Return `TempCtrlDisabled` with `ConfigInvalid` when that warning is active, `NoWarning` otherwise; an uninitialised zone always `NoWarning`.
- **V4-D12 (the fault tick and the permissive)** An active tick that ends in an existing fault has already recorded the permissive it evaluated; that value then freezes with the other diagnostics. The pending countdown's expiry tick freezes warning 8 (or `RunningOnTemp2` when it masks 8).
