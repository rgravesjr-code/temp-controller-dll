# TempCtl v3.0.0 - Controller Requirements (as implemented)

Status: describes the controller as shipped in TempCtl v3.0.0 (2026-09-18,
with Amendment A of the handoff applied: R5.4 drain rate 0.5, corrected C11
row, `ErrorTimeout` floor, header packaging). The rules were decided with the
system owner and handed over as the v3.0.0 implementation handoff; the rule
numbers below are the handoff's, so a markup can say "change R5.4" or "add
R8.6". Every statement is implemented and covered by the unit tests
(`test_tempctl`, 1516 checks) and the simulator scenarios. Words in `code`
are the names used in `tempctl.h` and the simulator. Section 12 lists the
decisions the implementation took where the handoff left room.

## 1. Scope and system context

TempCtl is signals-in / signals-out deadband temperature control with sensor
validation. It owns no hardware, no CAN and no timing.

- Host: a LabVIEW real-time application on a cRIO (NI Linux RT, x86_64). A
  Windows `.dll` is built from the same source for desktop test.
- Caller: a single main state-machine loop, nominally 100 ms (10 Hz). Per
  pass the host reads and scales inputs, runs its own logic, calls TempCtl
  once per zone, processes errors and hands the outputs to parallel loops
  that drive AO/DO/TCP/CAN. One calling thread.
- Relay outputs reach the physical DO through a parallel loop, so DO
  feedback lags the command slightly.
- Faults are shutdown events: the host treats any TempCtl fault as a stand
  shutdown. Warnings are informational.
- CAN packing is not part of TempCtl. The host collects and packs signals
  (this package defines a DBC over the diagnostics array as a convenience,
  section 10).
- Setup values are sanity-checked by the host before download; TempCtl's
  own config check is a backstop, but authoritative at run time.

### 1.1 Key design rule (REQ-7): no relay chatter

A relay command must never change because of one sample. Every relay
transition is time-qualified by a timeout. Corollary: a countdown starts on
the tick its condition is first observed and may expire only on a later tick
(`elapsed >= timeout`), so every positive timeout yields at least one tick of
grace.

## 2. API

All functions are `cdecl`, C linkage, no dependencies beyond libc. No 64-bit
integers, structs, enums or typedefs in any exported signature (LabVIEW
Import Shared Library wizard).

```c
int32_t TcInit(int32_t zone, uint32_t nowMs, const double* setupArray, int32_t setupLen,
               int32_t* status, int32_t* warning);
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs, double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB,
                    int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning);
int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning);
int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);
int32_t TcVersion(void);      /* (major<<16)|(minor<<8)|patch = 0x030000 */
int32_t TcSetupCount(void);   /* 17 */
int32_t TcDiagCount(void);    /* 25 */
```

| Argument | Type | Meaning |
|---|---|---|
| `zone` | int32 | Zone index 0..15. Zones are fully independent and share nothing. |
| `nowMs` | uint32 | LabVIEW *Tick Count (ms)*. Free-running; only differences are used; 2^32 wrap is handled. |
| `setupArray`, `setupLen` | const double*, int32 | `TcSetupCount()` values, section 3; `setupLen` must equal 17. |
| `temp1`, `temp2` | double | Raw scaled temperatures in the configured unit. `temp2` is ignored when `Temp2Enable = 0`. |
| `diHeaterFB`, `diCoolerFB` | int32 | Read-back of the heater / cooler digital output, 0/1 (any non-zero = 1). Ignored when `FeedbackEnable = 0`. |
| `doHeater`, `doCooler` | int32* | Relay commands for this tick, 0/1. Never both 1. |
| `status` | int32* | Status / fault code, section 6. |
| `warning` | int32* | Warning code, section 7. |
| `diagArray`, `diagLen` | double*, int32 | Diagnostics buffer; `diagLen >= 25`. |

Return codes (the call result, not the controller state):

| Value | Name | Meaning |
|---|---|---|
| 0 | `TC_OK` | Call executed. |
| -1 | `TC_ERR_ARG` | Null pointer, or `setupLen != 17`, or `diagLen < 25`. |
| -2 | `TC_ERR_ZONE` | `zone` outside 0..15. |

On any negative return nothing runs and no output is written. There is no
"not initialised" error (R9.4).

Call pattern: `TcInit` once per zone at startup; `TcCheckTemp` per zone
every pass; `TcInit` again with the full array for any parameter change;
`TcReset` to clear a fault; `TcGetDiag` at any rate for display, logging and
CAN.

## 3. Setup array - `TcSetupCount() = 17`

One 1D DBL array. Booleans: `value > 0.1 -> 1`, else 0 (NaN -> 0). Timeouts:
whole milliseconds, fractions truncated, must be >= 1 ms after truncation.
Temperatures are used as supplied.

| # | Name | Type | Unit | Valid range | Validation (R9.5) |
|---|---|---|---|---|---|
| 0 | `TempCtrlEnable` | bool | - | 0 or 1 | `> 0.1` -> 1; NaN fails the check |
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

Derived: `HiBand = Setpoint + DeadbandHi`, `LoBand = Setpoint - DeadbandLo`.
A new parameter is only ever appended; `TcSetupCount()` and `TcVersion()`
change together.

## 4. Diagnostics array - `TcDiagCount() = 25`

`TcGetDiag` is read-only: it never advances state, never changes relays and
may be called at any rate (or not at all).

| # | Name | Meaning |
|---|---|---|
| 0 | `ControlTemp` | The raw value control acts on (sensor 2 already offset-corrected). NaN before the first CheckTemp, and NaN while the active reading is NaN. |
| 1 | `ActiveSensor` | 1 or 2. |
| 2 | `Temp1Raw` | Last raw temp1 as supplied (NaN before the first CheckTemp). |
| 3 | `Temp2Raw` | Last raw temp2 as supplied (NaN when `Temp2Enable = 0`). |
| 4 | `Temp2Corrected` | `Temp2Raw + Temp2Offset` (NaN when `Temp2Enable = 0`). |
| 5 | `Temp1Avg` | Moving average of in-range temp1 samples, comparison use only. NaN until the first in-range sample. |
| 6 | `Temp2Avg` | Moving average of in-range corrected temp2 samples. NaN when disabled or no sample. |
| 7 | `HiBand` | `Setpoint + DeadbandHi`. |
| 8 | `LoBand` | `Setpoint - DeadbandLo`. |
| 9 | `Initial_HC_Flag` | 0/1, R7.5. |
| 10 | `DeadbandRemainMs` | Remaining ms before a relay engages; 0 when not counting. |
| 11 | `AtSetPtRemainMs` | Remaining ms before the running relay drops; 0 when not counting. |
| 12 | `CompareRemainMs` | Remaining ms to the disagreement fault; 0 when not counting. |
| 13 | `HeaterFbRemainMs` | Remaining ms to the heater feedback fault; 0 when not counting. |
| 14 | `CoolerFbRemainMs` | Remaining ms to the cooler feedback fault; 0 when not counting. |
| 15 | `Temp1OorAccumMs` | Sensor 1 leaky accumulator (R5.4). The sensor fails at `ErrorTimeout`; frozen once failed. |
| 16 | `Temp2OorAccumMs` | Sensor 2 leaky accumulator. |
| 17 | `Temp1OorEventsPerHour` | Rolling 60-minute count of in-range -> out-of-range transitions (R5.6). |
| 18 | `Temp2OorEventsPerHour` | Same for sensor 2. |
| 19 | `StatusMirror` | The status code from the last call (CheckTemp, Init or Reset). |
| 20 | `WarningMirror` | The warning code from the last call. |
| 21 | `doHeaterMirror` | Last heater command. |
| 22 | `doCoolerMirror` | Last cooler command. |
| 23 | `AppliedFilterPoints` | The value actually in use after R4.2. |
| 24 | `ZoneInitialized` | 1 once a setup has been loaded, else 0. |

A zone with no setup reports `ZoneInitialized = 0`, `ActiveSensor = 1`, NaN
for the temperatures, averages and bands, 0 for everything else.

## 5. Behaviour rules (normative)

### R1 - Interface

- **R1.1** One call per control tick per zone: `TcCheckTemp`. TempCtl holds all state internally; the host holds none.
- **R1.2** 16 independent zones (0..15), sharing nothing.
- **R1.3** Setup is loaded only by `TcInit`. `TcCheckTemp` takes only live signals.
- **R1.4** Every call takes `nowMs`. Only differences are used; 2^32 wrap is handled.
- **R1.5** A backwards time step counts as 0 ms elapsed: if `(uint32)(nowMs - lastMs) >= 0x80000000`, elapsed is 0.
- **R1.6** Single caller thread; no locking. Not thread-safe per zone. No allocation, no file or console I/O, no blocking calls (the `.so` imports only `memset` from libc).
- **R1.7** Builds: Windows x64 (and x86) `.dll`, cRIO x86_64 `.so` (and aarch64 for the Raspberry Pi bench), from one source.

### R2 - Enable and lifecycle

- **R2.1** `TempCtrlEnable` (setup 0) is the master switch for a zone.
- **R2.2** While disabled (`Enable = 0`, or no setup ever loaded) TempCtl takes no action: `doHeater = doCooler = 0`, no checks, no countdowns, no averaging, no faults; `status = 0 (TempCtrlDisabled)`. The only warning a disabled zone reports is `ConfigInvalid` (R9.5).
- **R2.3** Power-on state of every zone is disabled, because no setup is loaded.
- **R2.4** `TcCheckTemp` or `TcReset` on a zone with no setup is a harmless no-op returning `TC_OK` with `status = TempCtrlDisabled`.

### R3 - Units

- **R3.1** `TempUnits` is a label only (0 = degF, 1 = degC). TempCtl performs no conversion.
- **R3.2** All temperature quantities are supplied in that unit by the host.

### R4 - Signal handling and averaging

- **R4.1** Limit checks and control act on raw values. Averaging is used only for the Temp1-vs-Temp2 comparison (R6).
- **R4.2** `FilterPoints` selects the average length. A value in 1..64 is used as given (fractions truncated). Any other value (0, negative, > 64, NaN, Inf) becomes 4, with no warning and no fault.
- **R4.3** Only in-range raw samples enter an average. Sensor 2's average uses the corrected value.
- **R4.4** NaN or Inf (either sign) on a temperature input is treated as out of range high for the limit check. It never enters an average and never becomes a numeric `ControlTemp`.
- **R4.5** Init clears averages; Reset clears averages.

### R5 - Sensor range checking

- **R5.1** A sensor is out of range when its value (sensor 2: corrected) is `> HiLimit` or `< LoLimit`, including R4.4. Both limits apply to both sensors.
- **R5.2** Sensor 2 is checked only when `Temp2Enable = 1`.
- **R5.3** A sensor that has already failed is not re-evaluated for failure. Its range is still evaluated, but only to drive the warning code; its accumulator is frozen at the failure value.
- **R5.4 (leaky accumulator)** *Amended 2026-09-18 (Amendment A).* Each sensor holds an out-of-range accumulator in ms with `DRAIN = 0.5`: out of range this tick: `accum += elapsed`; in range this tick: `accum = max(0, accum - 0.5 x elapsed)`; `accum >= ErrorTimeout` -> the sensor has failed (R5.5). Every out-of-range tick charges its elapsed time, including the first one (the one-tick grace of section 1.1 applies to the deadband, at-setpoint, disagreement and relay-feedback countdowns only). For a sensor out of range a fraction `d` of the time the accumulator grows at `d - 0.5(1 - d)` per ms, so with `E = ErrorTimeout`: 100 % duty fails at 1 x E, 75 % at 1.6 x E, 50 % at 4 x E, 33 % or less never. The accumulator is kept in half-millisecond units internally, so odd loop periods stay exact; `TempxOorAccumMs` may therefore show a .5. **Host rule:** set `ErrorTimeout` to at least two loop periods (>= 200 ms at a 100 ms loop); a shorter value lets one out-of-range reading reach `ErrorTimeout` on the tick it is first seen and fail the sensor from a single sample, which TempCtl cannot detect because it does not know the loop rate.
- **R5.5 (effect of a failure)** `Temp2Enable = 0`: sensor 1 failed -> fault `Temp1FailHigh` or `Temp1FailLow` by the condition at the moment of failure. `Temp2Enable = 1`, one sensor failed, the other healthy -> no fault; control uses the healthy sensor; warning `RunningOnTemp2` when the switch was away from sensor 1; TempCtl never switches back on its own. `Temp2Enable = 1`, both failed -> fault `BothSensorsFailed`.
- **R5.6 (health metric)** Each sensor keeps a rolling 60-minute count of in-range -> out-of-range transitions (`TempxOorEventsPerHour`): 60 one-minute buckets in a static ring buffer, advanced by elapsed time. Init and Reset clear it. A sensor that is out of range on its first tick after Init or Reset counts one transition.
- **R5.7 (control while out of range)** While the active sensor's value is currently out of range, the deadband and at-setpoint checks pause: relay commands and the status code hold their present state and both countdowns freeze (not reset). Control resumes when the value returns in range; the failure action happens when the accumulator reaches `ErrorTimeout`. An out-of-range condition on the non-active sensor does not affect control.

### R6 - Two-sensor comparison

- **R6.1** `Temp2Corrected = temp2 + Temp2Offset`, used for the limit check, the comparison, control after a switch and the reported Temp2 values.
- **R6.2** The comparison runs only when: `Temp2Enable = 1`, neither sensor has failed, `Initial_HC_Flag = 1`, both averages hold `FilterPoints` samples, neither sensor is currently out of range, and no fault is latched. The check is evaluated before the control step of a tick, so it first runs on the tick after `Initial_HC_Flag` was set.
- **R6.3** Disagreement condition: `|Temp1Avg - Temp2Avg| > Temp2Tolerance` (exactly the tolerance agrees).
- **R6.4 (two stages)** Held for `TempCompareTimeout / 10` (integer division) -> warning `TempDisagree`, control continues. Held for the full `TempCompareTimeout` -> fault `TempDisagreeFault`. Agreement at any point clears the warning immediately and resets the countdown. `TempCompareTimeout / 10 == 0` -> the warning appears on the first qualifying tick.
- **R6.5** If either sensor goes out of range (or any other R6.2 condition stops holding), the comparison pauses; when it qualifies again it restarts from zero (countdown and warning reset).
- **R6.6** Neither sensor is voted correct. A sustained disagreement always ends in a fault.

### R7 - Control (deadband)

All decisions use `ControlTemp`: the raw active-sensor value, offset-corrected when the active sensor is 2.

- **R7.1 (engage)** Idle and `ControlTemp > HiBand` continuously for `DeadbandTimeout` -> cooling on. Idle and `ControlTemp < LoBand` continuously for `DeadbandTimeout` -> heating on. Re-entering the band clears the countdown; crossing to the other side restarts it. The band edges are inside the band.
- **R7.2 (release)** A running relay drops only after the at-setpoint condition has held for `AtSetPtTimeout`: `ControlTemp >= Setpoint` while heating, `ControlTemp <= Setpoint` while cooling. Breaking the condition resets that countdown. On the tick the relay drops, the idle logic runs at once (so an overshoot past the far band starts the opposite countdown on that tick).
- **R7.3** Heating and cooling are mutually exclusive. While a relay is on, the deadband countdown is idle (`DeadbandRemainMs = 0`).
- **R7.4** No relay may change state on a single sample, in any state (REQ-7).
- **R7.5 (`Initial_HC_Flag`)** Cleared by Init and Reset. Set when either the at-setpoint condition completes its `AtSetPtTimeout` (the relay drops), or the zone is idle with `LoBand <= ControlTemp <= HiBand` on any tick, including the first tick after Init or Reset. It gates the comparison (R6.2).

### R8 - Relay (DO) feedback

- **R8.1** `diHeaterFB` / `diCoolerFB` are read-backs of the digital output, not the physical relay contact (documented gap, section 11).
- **R8.2** The check runs only when `FeedbackEnable = 1`, and never while the zone is disabled or stopped on a fault.
- **R8.3** Each feedback is compared with that relay's command from the previous `TcCheckTemp` call for this zone. After the first Init or a Reset the previous command is 0; after a re-Init that keeps the relays (R9.3) it is the kept state.
- **R8.4** Per relay: a mismatch raises its warning (`HeaterFBMismatch` / `CoolerFBMismatch`) immediately and starts the `RelayFeedbackTimeout` countdown. A match clears the warning and resets the countdown. Reaching the timeout raises that relay's fault.
- **R8.5** `RelayFeedbackTimeout` must exceed the worst-case host DO-loop latency (see `LABVIEW_INTEGRATION.md`).

### R9 - Init, Reset and faults

- **R9.1 (Init)** Validates and stores the setup (R9.5), sets the time reference, clears faults, warnings, averages, accumulators, hourly counts, countdowns and `Initial_HC_Flag`, sets `ActiveSensor = 1`. Allowed at any time, including on a stopped zone. Until the first CheckTemp the status is `TempAtSetPt` (or `HeaterON` / `CoolerON` when a relay was kept, `TempCtrlDisabled` when disabled, `ConfigFault` when the check failed).
- **R9.2 (Reset)** Keeps the stored setup. Clears faults, warnings, averages, accumulators, hourly counts, countdowns and `Initial_HC_Flag`, sets `ActiveSensor = 1`, sets both relay commands to 0, and sets the time reference. It does not clear `ConfigFault`.
- **R9.3 (relay state across Init)** If the zone is currently enabled and running (not disabled, not stopped) and the new setup has `Enable = 1` and passes the config check, the internal relay commands are kept and the new setup's normal logic then decides whether each relay stays on or drops (the at-setpoint countdown starts afresh). In every other case both relays start at 0.
- **R9.4** No "not initialised" error: a call on a zone with no setup is a no-op (R2.4).
- **R9.5 (config check, at Init)** Checks: `TempUnits` is 0 or 1; `DeadbandHi >= 0`, `DeadbandLo >= 0`, not both zero; `LoLimit < LoBand <= Setpoint <= HiBand < HiLimit`; every applicable timeout >= 1 ms; no NaN in any applicable setup value; when `Temp2Enable = 1`: `Temp2Tolerance >= 0`, `Temp2Offset` finite, `TempCompareTimeout >= 1`; when `FeedbackEnable = 1`: `RelayFeedbackTimeout >= 1`. `FilterPoints` is never checked. Parameters of a disabled feature are not checked. `Enable = 1` and a check fails -> `ConfigFault`: zone stopped, relays 0, latched; only a subsequent Init that passes clears it (Reset does not). `Enable = 0` and a check fails -> warning `ConfigInvalid`; the zone stays disabled.
- **R9.6 (fault behaviour)** On any fault: `doHeater = doCooler = 0`; the fault code latches into `status`; the warning code freezes at the value computed on the fault tick; every check, countdown, accumulator and average stops (the diagnostics freeze). Only Reset or Init resumes the zone. The first fault wins. If two conditions mature on the same tick the order is: config -> sensor range -> disagreement -> heater feedback -> cooler feedback.
- **R9.7** Faults never self-clear, and failed sensors are never re-admitted.

## 6. Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `TempCtrlDisabled` | `TempCtrlEnable = 0`, or no setup loaded. Relays 0; nothing evaluated. |
| 1 | `TempAtSetPt` | Enabled, relays off, `ControlTemp` inside the deadband. |
| 2 | `HeaterON` | Heating commanded (held through the at-setpoint countdown). |
| 3 | `CoolerON` | Cooling commanded. |
| 4 | `HeatPending` | Relays off, below `LoBand`, deadband countdown running. |
| 5 | `CoolPending` | Relays off, above `HiBand`, deadband countdown running. |
| 6-9 | reserved | |
| 10 | `Temp1FailHigh` | Fault. Single-sensor mode: sensor 1 failed high (includes NaN/Inf). |
| 11 | `Temp1FailLow` | Fault. Single-sensor mode: sensor 1 failed low. |
| 12 | `BothSensorsFailed` | Fault. Two-sensor mode: no healthy sensor remains. |
| 13 | `TempDisagreeFault` | Fault. Sensors disagreed for `TempCompareTimeout`. |
| 14 | `ConfigFault` | Fault. Init config check failed with `Enable = 1`. Cleared only by a passing Init. |
| 15 | `HeaterFBFault` | Fault. Heater DO feedback mismatched for `RelayFeedbackTimeout`. |
| 16 | `CoolerFBFault` | Fault. Cooler DO feedback mismatched for `RelayFeedbackTimeout`. |
| 17+ | reserved | |

Any code >= 10 is a fault, and the host treats it as a stand shutdown. While
paused (R5.7) the code holds its pre-pause value.

## 7. Warning codes

One value; the lowest active code wins.

| Code | Name | Sets | Clears |
|---|---|---|---|
| 0 | `NoWarning` | - | - |
| 1 | `Temp1OutOfRange` | Sensor 1's raw value is out of range now (failed or not) | Sensor 1 back in range |
| 2 | `Temp2OutOfRange` | Same for sensor 2's corrected value (`Temp2Enable = 1`) | Sensor 2 back in range |
| 3 | `HeaterFBMismatch` | `diHeaterFB` != previous heater command | They match |
| 4 | `CoolerFBMismatch` | `diCoolerFB` != previous cooler command | They match |
| 5 | `TempDisagree` | Disagreement held for `TempCompareTimeout / 10` | Agreement, or the comparison pauses |
| 6 | `RunningOnTemp2` | Sensor 1 failed and control switched to sensor 2 | Reset or Init only; masked by codes 1-5 while they are active |
| 7 | `ConfigInvalid` | Init with `Enable = 0` failed the config check | The next Init that passes |

Warnings never latch (code 6 excepted) and never change control. While a
zone is stopped on a fault, the warning code freezes.

## 8. Change list vs v2.0.1

| # | Change | v2.0.1 | v3.0.0 |
|---|---|---|---|
| C1 | API split | One `TcStep` with `in[17]`/`out[27]` SGL | `TcInit` / `TcCheckTemp` / `TcReset` / `TcGetDiag` + 3 helpers, doubles |
| C2 | Setup loaded at Init only | every input re-read each call | R1.3 |
| C3 | `TempCtrlEnable` | absent | R2 |
| C4 | `TempUnits` | absent | R3, label only |
| C5 | `Temp2Offset` | absent | R6.1 |
| C6 | Disagreement is a fault | flag only | R6.3-R6.4, two stages |
| C7 | `TempCompareTimeout` | absent | setup 13 |
| C8 | `AtSetPtTimeout` | release on one sample | R7.2 |
| C9 | Raw control and limits | filtered values | R4.1 |
| C10 | NaN handling | separate bad bits | R4.4 |
| C11 | Leaky accumulator | countdown restarted on recovery | R5.4 with `DRAIN = 0.5` (Amendment A): 100 % duty fails at 1 x E, 50 % at about 4 x E, 33 % or less never; a single-tick glitch never fails; the accumulator charges on its first out-of-range tick |
| C12 | Single-sensor failure is not a fault | latched bits + Degraded | R5.5, warning `RunningOnTemp2` |
| C13 | Control pause | relays dropped at once | R5.7 |
| C14 | `Initial_HC_Flag` | warm-up status | R7.5 |
| C15 | DO feedback | 1-tick compare, flag only | R8, separate warning and fault per relay |
| C16 | Config check | warn and run | R9.5, `ConfigFault` / `ConfigInvalid` |
| C17 | Reset clears history | kept the filters | R9.2 |
| C18 | Relay state across Init | from the input array | R9.3 |
| C19 | Status and warning outputs | bit mask + priority status | sections 6, 7 |
| C20 | Input echo / CAN | outputs echoed the inputs | removed |
| C21 | Time robustness | wrap only | R1.5 |
| C22 | Diagnostics | none | section 4, `TcGetDiag` |

## 9. Build and packaging

1. LabVIEW-importable header: only `int32_t`, `uint32_t`, `double`; arrays as pointer + `int32_t` length; no structs, enums, typedefs or 64-bit integers; simple `#define` names for every index and code; a doc comment per function listing the array indexes. The wizard run itself is the LabVIEW side's acceptance step. `tempctl.h` ships inside the package next to the Windows `.dll` and the cRIO `.so` (and again under `src\`); it is the same file for every target, so one set of wrapper VIs serves the desktop DLL and the deployed `.so` with nothing regenerated (Amendment A, A6).
2. TempSim 2.0.0 drives the v3.0.0 API and runs every scenario of section 10 with built-in expectations (`SIMULATOR.md`).
3. Unit tests cover every rule of section 5 and every row of section 8 (`TESTING.md`).
4. Determinism: no heap allocation, I/O or blocking in any call; all state static (16 zones x 2 sensors x 60 ring entries + 64-sample averages).
5. `TcVersion()` returns `0x030000`.
6. Shipped documentation: this file, `TEMPCTL-CAPABILITY-v3.0.0.md`, `TEMPCTL_PACKAGE_GUIDE.md` (index tables, codes), `LABVIEW_INTEGRATION.md` (CLFN settings, DO-loop latency rule, leaky accumulator note).

## 10. Test scenarios (unit tests and TempSim)

S1 single-sensor heat-up; S2 setpoint change by re-Init while heating; S3
cool-down; S4 chatter immunity; S5 flickering sensor; S6 control pause; S7
failover; S8 disagreement; S9 comparison gating; S10 relay feedback; S11
config faults; S12 enable/disable; S13 reset; S14 two zones; S15 time (wrap,
backwards step, long gap). TempSim.Cli scenario names: `heat-up`,
`setpoint-reinit`, `cool-down`, `chatter`, `flicker`, `control-pause`,
`failover`, `disagree`, `compare-gating`, `relay-feedback`, `config-fault`,
`enable-disable`, `reset`, `two-zones`, `time`.

## 11. Known coverage gaps (documented, not implemented)

1. Physically stuck relay: feedback reads the DO state, so a welded contact with a correct DO is not detected.
2. No heating or cooling progress: a heater that never reaches the setpoint stays on indefinitely; no max-on-time fault.
3. Wrong-but-in-range sensor: after one sensor has failed the survivor is no longer cross-checked.
4. Single warning output: only the lowest-numbered warning is visible.
5. No process over-temperature independent of sensor validity: `HiLimit`/`LoLimit` serve as both sensor-range and trip limits.

## 12. Implementation decisions (for markup)

- **I1 (R5.4 vs C11) - resolved by Amendment A, 2026-09-18.** The original C11 row claimed a 50 % in/out duty fails at about 2 x `ErrorTimeout`, which the 1:1 drain of the original R5.4 cannot produce (a 50 % duty nets zero). Scott's amendment set `DRAIN = 0.5`, corrected C11 and added the `ErrorTimeout` floor; both are implemented (R5.4 above). The tests and the `flicker` / `flicker-25` scenarios pin the four duty points of the amended table.
- **I2 (accumulator first tick) - confirmed by Amendment A (A4).** Every out-of-range tick charges its elapsed time, the first one included; the one-tick grace of section 1.1 applies to the deadband, at-setpoint, disagreement and relay-feedback countdowns only (each of those is pinned by a test that shows the observation tick with the full countdown remaining and expiry on a later tick).
- **I3 (status while paused)** R5.7 says relays hold; the status code holds with them (`HeaterON` stays `HeaterON`, `HeatPending` stays `HeatPending`).
- **I4 (comparison vs flag)** The comparison is evaluated before the control step of a tick, so it first qualifies on the tick after `Initial_HC_Flag` is set by a relay release.
- **I5 (Init / Reset status)** Before the first CheckTemp a valid enabled zone reports `TempAtSetPt` (or `HeaterON` / `CoolerON` when relays were kept across a re-Init); the first CheckTemp corrects it.
- **I6 (timeouts below 1 ms)** A timeout that truncates to 0 (0.5) fails the config check; the smallest accepted value is 1 ms, which still needs a later tick to expire.
- **I7 (failed sensor accumulator)** Frozen at the failure value instead of draining, so `TempxOorAccumMs` stays at `ErrorTimeout` while the sensor is failed.
- **I8 (first-tick transition)** A sensor out of range on its first tick after Init or Reset counts one `OorEventsPerHour` transition.
- **I9 (warning on the fault tick)** The frozen warning is the one computed on the fault tick (for example `Temp2OutOfRange` when sensor 2's excursion caused `BothSensorsFailed`).
