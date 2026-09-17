# TempCtl v3.0.0 — Implementation Handoff

**Baseline:** TempCtl v2.0.1 (spec `TEMPCTL-SPEC-v2.0.1.docx`, dated 2026-09-12).
**Target:** v3.0.0. The API and several behaviour rules change, so this is a major version.
**Audience:** the Claude project that builds the TempCtl `.dll` / `.so`.
**Revision:** Amendment A applied 2026-09-18 — R5.4 drain rate, C11 acceptance test, the `ErrorTimeout` floor note, and header packaging (§9.1). See `TEMPCTL-v3.0.0-AMENDMENT-A.md` for the reasons and the work required.
**Status of this document:** every rule below was decided with the system owner (Scott). This document is standalone — implement from it, using the v2.0.1 spec only to locate existing code (its R-numbers are cited as `v2:Rx.y`).

---

## 1. Scope and system context

TempCtl is signals-in / signals-out deadband temperature control with sensor validation. It owns no hardware, no CAN and no timing.

- **Host:** a LabVIEW real-time application on a cRIO (NI Linux RT, x86_64). A Windows `.dll` is built from the same source for desktop test.
- **Caller:** a single main state-machine loop, nominally **100 ms (10 Hz)**. Per pass the host reads and scales inputs (AI/DI/CAN), runs custom logic, calls TempCtl once per zone, processes errors, then hands outputs to parallel loops that drive AO/DO/TCP/CAN. There is one calling thread.
- **Relay outputs** reach the physical DO through a parallel loop, so DO feedback lags the command slightly.
- **Faults are shutdown events.** The host treats *any* TempCtl fault as a stand shutdown. Warnings are informational; the host may act on them.
- **CAN packing is NOT part of TempCtl.** Remove the v2.0.1 input echo (`v2:R3.1`) entirely; other host code collects and packs signals.
- **Setup values are sanity-checked by the host/UI before download.** TempCtl's own config check is a backstop, but it is authoritative at run time.

### 1.1 Key design rule (REQ-7): no relay chatter

**A relay command must never change because of one sample.** Every relay transition is time-qualified by a timeout. Relay chatter damages relays, heaters and cooling equipment. This rule overrides convenience everywhere; when in doubt, add a timeout rather than react to a sample.

Corollary for the implementer: a countdown starts on the tick its condition is first observed and may expire only on a **later** tick (`elapsed >= timeout`), so every positive timeout yields at least one tick of grace.

---

## 2. API (v3.0.0)

All functions are `cdecl`, C linkage, no dependencies beyond libc. **No 64-bit integer types, no structs, no enums, no typedefs in any exported signature** — see §9 (LabVIEW import constraints).

```c
/* Load/replace a zone's setup. Also used to change any parameter at run time. */
int32_t TcInit(int32_t zone, uint32_t nowMs,
               const double *setupArray, int32_t setupLen,
               int32_t *status, int32_t *warning);

/* One control tick for one zone. */
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs,
                    double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB,
                    int32_t *doHeater, int32_t *doCooler,
                    int32_t *status, int32_t *warning);

/* Clear faults, warnings, history and countdowns; keep the stored setup. */
int32_t TcReset(int32_t zone, uint32_t nowMs,
                int32_t *status, int32_t *warning);

/* Read-only diagnostics for display, logging and CAN packing by the host. */
int32_t TcGetDiag(int32_t zone, double *diagArray, int32_t diagLen);

/* Sizing and version helpers. */
int32_t TcVersion(void);      /* (major<<16)|(minor<<8)|patch -> 0x030000 for 3.0.0 */
int32_t TcSetupCount(void);   /* 17 */
int32_t TcDiagCount(void);    /* 25 */
```

### 2.1 Arguments

| Argument | Type | Meaning |
|---|---|---|
| `zone` | int32 | Zone index 0..15. Zones are fully independent and share nothing. |
| `nowMs` | uint32 | LabVIEW *Tick Count (ms)*. Free-running; only differences are used; 2^32 wrap is handled. Never packed into a double. |
| `setupArray` | const double* | 1D array of `TcSetupCount()` values, §3. |
| `setupLen` | int32 | Element count supplied; must equal `TcSetupCount()`. |
| `temp1`, `temp2` | double | Raw scaled temperatures in the configured unit. `temp2` is ignored when `Temp2Enable = 0`. |
| `diHeaterFB`, `diCoolerFB` | int32 | Read-back of the heater/cooler **digital output** state, 0/1. Ignored when `FeedbackEnable = 0`. |
| `doHeater`, `doCooler` | int32* | Relay commands for this tick, 0/1. Never both 1. |
| `status` | int32* | Status / fault code, §6. |
| `warning` | int32* | Warning code, §7. |
| `diagArray`, `diagLen` | double*, int32 | Diagnostics buffer; `diagLen >= TcDiagCount()`. |

### 2.2 Return codes (the call result, not the controller state)

| Value | Name | Meaning |
|---|---|---|
| 0 | `TC_OK` | Call executed. |
| −1 | `TC_ERR_ARG` | Null pointer, or `setupLen != TcSetupCount()`, or `diagLen < TcDiagCount()`. |
| −2 | `TC_ERR_ZONE` | `zone` outside 0..15. |

On any negative return **nothing runs and no output is written** — `status`, `warning`, `doHeater` and `doCooler` keep their previous values. There is no `TC_ERR_NOT_INIT` in v3.0.0 (see R9.4).

### 2.3 Call pattern

```
startup:            TcInit(zone, now, setup, 17, &st, &wn)   once per zone
every 100 ms:       TcCheckTemp(zone, now, t1, t2, hfb, cfb, &dh, &dc, &st, &wn)  per zone
parameter change:   TcInit(...) again with the full setup array
clear a fault:      TcReset(zone, now, &st, &wn)
display/logging:    TcGetDiag(zone, diag, 25)   at any rate, read-only
```

---

## 3. Setup array (index order) — `TcSetupCount() = 17`

One 1D **DBL** array. Each element is interpreted as its true type. Conversion rules: booleans are `value > 0.1 -> 1, else 0` (NaN compares false, so NaN → 0); timeouts are whole milliseconds, fractions truncated; temperatures are used as supplied.

| # | Name | Type | Unit | Valid range | Validation (§5.9) |
|---|---|---|---|---|---|
| 0 | `TempCtrlEnable` | bool | — | 0 or 1 | `>0.1` → 1 |
| 1 | `TempUnits` | enum | — | 0 = °F, 1 = °C | must be 0 or 1, else CONFIG fault |
| 2 | `Setpoint` | double | deg | within limits | see band check |
| 3 | `DeadbandHi` | double | deg | >= 0 | not negative; not both deadbands 0 |
| 4 | `DeadbandLo` | double | deg | >= 0 | not negative; not both deadbands 0 |
| 5 | `HiLimit` | double | deg | > HiBand | band check |
| 6 | `LoLimit` | double | deg | < LoBand | band check |
| 7 | `ErrorTimeout` | uint32 | ms | > 0 | <= 0 or NaN → CONFIG fault |
| 8 | `DeadbandTimeout` | uint32 | ms | > 0 | <= 0 or NaN → CONFIG fault |
| 9 | `AtSetPtTimeout` | uint32 | ms | > 0 | <= 0 or NaN → CONFIG fault |
| 10 | `Temp2Enable` | bool | — | 0 or 1 | `>0.1` → 1 |
| 11 | `Temp2Offset` | double | deg | any finite | NaN → CONFIG fault (only when `Temp2Enable = 1`) |
| 12 | `Temp2Tolerance` | double | deg | >= 0 | negative or NaN → CONFIG fault (only when `Temp2Enable = 1`) |
| 13 | `TempCompareTimeout` | uint32 | ms | > 0 | <= 0 or NaN → CONFIG fault (only when `Temp2Enable = 1`) |
| 14 | `FilterPoints` | int32 | samples | 1..64 | never faults; see R4.2 |
| 15 | `FeedbackEnable` | bool | — | 0 or 1 | `>0.1` → 1 |
| 16 | `RelayFeedbackTimeout` | uint32 | ms | > 0 | <= 0 or NaN → CONFIG fault (only when `FeedbackEnable = 1`) |

Derived: `HiBand = Setpoint + DeadbandHi`, `LoBand = Setpoint − DeadbandLo`.

**Never append a new parameter anywhere but the end**, and bump `TcSetupCount()` and `TcVersion()` together when doing so.

---

## 4. Diagnostics array (index order) — `TcDiagCount() = 25`

`TcGetDiag` is read-only: it never advances state, never changes relays and may be called at any rate (or not at all).

| # | Name | Meaning |
|---|---|---|
| 0 | `ControlTemp` | The raw value control acts on (sensor 2 already offset-corrected). NaN before the first CheckTemp. |
| 1 | `ActiveSensor` | 1 or 2. |
| 2 | `Temp1Raw` | Last raw Temp1 as supplied. |
| 3 | `Temp2Raw` | Last raw Temp2 as supplied (NaN when `Temp2Enable = 0`). |
| 4 | `Temp2Corrected` | `Temp2Raw + Temp2Offset`. |
| 5 | `Temp1Avg` | Moving average of Temp1, comparison use only. NaN until the first in-range sample. |
| 6 | `Temp2Avg` | Moving average of `Temp2Corrected`. NaN when disabled or no sample. |
| 7 | `HiBand` | `Setpoint + DeadbandHi`. |
| 8 | `LoBand` | `Setpoint − DeadbandLo`. |
| 9 | `Initial_HC_Flag` | 0/1, see R7.5. |
| 10 | `DeadbandRemainMs` | Remaining ms before a relay engages; 0 when not counting. |
| 11 | `AtSetPtRemainMs` | Remaining ms before the running relay drops; 0 when not counting. |
| 12 | `CompareRemainMs` | Remaining ms to the disagreement fault; 0 when not counting. |
| 13 | `HeaterFbRemainMs` | Remaining ms to the heater feedback fault; 0 when not counting. |
| 14 | `CoolerFbRemainMs` | Remaining ms to the cooler feedback fault; 0 when not counting. |
| 15 | `Temp1OorAccumMs` | Sensor 1 leaky accumulator (R5.4). Fault at `ErrorTimeout`. |
| 16 | `Temp2OorAccumMs` | Sensor 2 leaky accumulator. |
| 17 | `Temp1OorEventsPerHour` | Rolling 60-minute count of in-range → out-of-range transitions (R5.6). |
| 18 | `Temp2OorEventsPerHour` | Same for sensor 2. |
| 19 | `StatusMirror` | The status code from the last CheckTemp. |
| 20 | `WarningMirror` | The warning code from the last CheckTemp. |
| 21 | `doHeaterMirror` | Last heater command. |
| 22 | `doCoolerMirror` | Last cooler command. |
| 23 | `AppliedFilterPoints` | The value actually in use after R4.2. |
| 24 | `ZoneInitialized` | 1 once a setup has been loaded, else 0. |

---

## 5. Behaviour rules (v3.0.0, normative)

### R1 — Interface

- **R1.1** One call per control tick per zone: `TcCheckTemp`. TempCtl holds all state internally; the host holds none.
- **R1.2** 16 independent zones (0..15), sharing nothing. *(v2:R1.2 unchanged.)*
- **R1.3** Setup is loaded **only** by `TcInit`. `TcCheckTemp` takes only live signals. *(Changes v2:R1.6, where every input was re-read each call.)*
- **R1.4** Every call takes `nowMs`. Only differences are used; 2^32 wrap is handled. *(v2:R1.4.)*
- **R1.5** A backwards time step counts as **0 ms elapsed**: if `(uint32)(nowMs − lastMs) >= 0x80000000`, treat elapsed as 0. This prevents a wrong or restarted host timer from expiring every countdown at once.
- **R1.6** Single caller thread assumed; no locking required. The library must be documented as *not* thread-safe per zone, and must contain no allocation, no file or console I/O and no blocking calls, so it is safe in an RT loop.
- **R1.7** Builds: Windows x64 `.dll` (test) and cRIO x86_64 `.so` (target), from one source.

### R2 — Enable and lifecycle

- **R2.1** `TempCtrlEnable` (setup index 0) is the master switch for a zone.
- **R2.2** While disabled (`Enable = 0`, or no setup ever loaded) TempCtl **takes no action**: `doHeater = doCooler = 0`, no checks, no countdowns, no averaging, no warnings, no faults. `status = 0 (TempCtrlDisabled)`.
- **R2.3** Power-on state of every zone is disabled, because no setup is loaded.
- **R2.4** `TcCheckTemp` or `TcReset` on a zone with no setup is a harmless no-op returning `TC_OK` with `status = TempCtrlDisabled`.

### R3 — Units

- **R3.1** `TempUnits` is a **label only** (0 = °F, 1 = °C). TempCtl performs no conversion.
- **R3.2** All temperature quantities — `temp1`, `temp2`, `Setpoint`, deadbands, limits, `Temp2Offset`, `Temp2Tolerance` — are supplied in that unit by the host.

### R4 — Signal handling and averaging

- **R4.1** Limit checks and control act on **raw** values. Averaging is used **only** for the Temp1-vs-Temp2 comparison (R6). *(Changes v2:R5.2 and v2:R7, which used filtered values.)*
- **R4.2** `FilterPoints` selects the average length. A value in 1..64 is used as given (fractions truncated). **Any** other value — 0, negative, > 64, NaN, Inf — becomes **4**, with no warning and no fault. *(Changes v2:R8.3 clamping.)*
- **R4.3** Only **in-range** raw samples enter an average, so bad data is never averaged. Sensor 2's average uses the corrected value (`temp2 + Temp2Offset`).
- **R4.4** `NaN` or `Inf` on a temperature input is treated as **out of range high**: substitute a sentinel far above `HiLimit` for the limit check only. The sentinel never enters an average and never becomes `ControlTemp`. *(Replaces v2 `T1_BAD`/`T2_BAD`; those fault bits are removed.)*
- **R4.5** Init clears averages; Reset clears averages (*changes v2:R9.2, which kept them*).

### R5 — Sensor range checking

- **R5.1** A sensor is *out of range* when its value (sensor 2: corrected) is `> HiLimit` or `< LoLimit`, including the R4.4 sentinel. Both limits apply to both sensors.
- **R5.2** Sensor 2 is checked only when `Temp2Enable = 1`.
- **R5.3** A sensor that has already failed is not re-evaluated for failure. Its range is still evaluated, but only to drive the warning code.
- **R5.4 (leaky accumulator).** *Amended 2026-09-18 — supersedes the original 1:1 rule; see Amendment A.* Each sensor holds an out-of-range accumulator in ms. With `DRAIN = 0.5`:
  - out of range this tick: `accum += elapsed`;
  - in range this tick: `accum = max(0, accum − DRAIN * elapsed)`;
  - `accum >= ErrorTimeout` → **the sensor has failed** (R5.5).
  It replaces a countdown that resets on recovery, so a sensor flickering in and out of range still fails instead of disturbing control indefinitely. *(Changes v2:R5.3.)*
  - **Resulting behaviour.** For a sensor out of range a fraction `d` of the time, the accumulator grows at `d − DRAIN·(1−d)` per ms, so with `DRAIN = 0.5` any sensor out of range more than **one third** of the time eventually fails, while a cleanly recovered sensor drains to zero:

    | Duty out of range | Time to failure (`E` = `ErrorTimeout`) |
    |---|---|
    | 100 % | 1 × E |
    | 75 % | 1.6 × E |
    | 50 % | 4 × E |
    | 33 % or less | never |

  - **The accumulator charges from its first out-of-range tick.** The one-tick grace described in §1.1 applies to the relay countdowns (deadband, at-setpoint, disagreement, relay feedback) but **not** to this accumulator: granting it a grace tick would make an alternating one-tick excursion immune forever, which is exactly the failure mode the accumulator exists to catch.
  - **Configuration note (host):** set `ErrorTimeout` to at least **two loop periods** (≥ 200 ms at a 100 ms loop). A shorter value lets a single out-of-range reading reach `ErrorTimeout` on the tick it is first seen, which would fail the sensor — and so drop both relays — from one sample. TempCtl cannot check this, because it does not know the host loop rate.
- **R5.5 (effect of a failure).**
  - `Temp2Enable = 0`: sensor 1 failed → **fault**, `Temp1FailHigh` or `Temp1FailLow` by the condition at the moment of failure.
  - `Temp2Enable = 1`, one sensor failed, the other healthy → **no fault**. Control uses the healthy sensor; warning `RunningOnTemp2` when the switch was away from sensor 1. TempCtl never switches back on its own; only Init or Reset restores sensor 1.
  - `Temp2Enable = 1`, both sensors failed → **fault** `BothSensorsFailed` (no per-sensor detail required).
- **R5.6 (health metric).** Each sensor keeps a rolling 60-minute count of in-range → out-of-range **transitions**, reported as `TempxOorEventsPerHour`. Implement as 60 one-minute buckets in a ring buffer (static memory; no allocation). **Init and Reset both clear it**, so pre- and post-reset history is never mixed.
- **R5.7 (control while out of range).** While the **active** sensor's value is currently out of range, the deadband and at-setpoint checks **pause**: relay commands hold their present state and both countdowns freeze (they are not reset). Control resumes when the value returns in range; the failure action happens when the accumulator reaches `ErrorTimeout`. An out-of-range condition on the non-active sensor does not affect control. *(Replaces v2:R5.9, which dropped relays on a single bad sample.)*

### R6 — Two-sensor comparison

- **R6.1** `Temp2Corrected = temp2 + Temp2Offset`. The offset compensates for sensor 2 sitting in a different location. The corrected value is used for the limit check, the comparison, control after a switch, and the reported Temp2 values.
- **R6.2** The comparison runs only when: `Temp2Enable = 1`, neither sensor has failed, `Initial_HC_Flag = 1`, both averages hold `FilterPoints` samples, neither sensor is currently out of range, and the disagreement fault is not already latched.
- **R6.3** Disagreement condition: `|Temp1Avg − Temp2Avg| > Temp2Tolerance`.
- **R6.4 (two stages).**
  - Held for `TempCompareTimeout / 10` (integer division) → **warning** `TempDisagree`. Control continues.
  - Held for the full `TempCompareTimeout` → **fault** `TempDisagreeFault`.
  - Agreement at any point clears the warning immediately and resets the countdown.
  - `TempCompareTimeout / 10 == 0` → the warning appears on the first qualifying tick.
- **R6.5** If either sensor goes out of range, the comparison **pauses**; when that sensor returns in range the comparison **restarts from zero** (countdown and warning reset). *(Rationale: neither sensor can be trusted as the reference during a range excursion.)*
- **R6.6** Neither sensor is "voted" correct. A sustained disagreement always ends in a fault.

### R7 — Control (deadband)

All decisions use `ControlTemp`: the raw active-sensor value, offset-corrected when the active sensor is 2.

- **R7.1 (engage).** Idle and `ControlTemp > HiBand` continuously for `DeadbandTimeout` → cooling on. Idle and `ControlTemp < LoBand` continuously for `DeadbandTimeout` → heating on. Re-entering the band clears the countdown; crossing to the other side restarts it.
- **R7.2 (release).** A running relay drops only after the at-setpoint condition has held for `AtSetPtTimeout`: `ControlTemp >= Setpoint` while heating, `ControlTemp <= Setpoint` while cooling. Breaking the condition resets that countdown. *(Changes v2:R7.2, which released on one sample.)*
- **R7.3** Heating and cooling are mutually exclusive. While a relay is on, the deadband countdown is idle (`DeadbandRemainMs = 0`).
- **R7.4** No relay may change state on a single sample, in any state (REQ-7).
- **R7.5 (`Initial_HC_Flag`).** Cleared by Init and Reset. Set when either:
  1. the at-setpoint condition completes its `AtSetPtTimeout` (the relay drops), or
  2. the zone is idle with `LoBand <= ControlTemp <= HiBand` on any tick, including the first tick after Init or Reset.
  It gates the comparison (R6.2), standing in for a "warm-up complete" signal: the first heat-up or cool-down after Init or Reset is the warm-up.

### R8 — Relay (DO) feedback

- **R8.1** `diHeaterFB` / `diCoolerFB` are read-backs of the **digital output**, not the physical relay contact. A mechanically stuck relay is **not** detectable (documented gap, §10).
- **R8.2** The check runs only when `FeedbackEnable = 1`, and never while the zone is disabled or stopped on a fault.
- **R8.3** Each feedback is compared with **that relay's command from the previous `TcCheckTemp` call** for this zone. After the first Init or a Reset the previous command is 0; after a re-Init that keeps the relays (R9.3) it is the kept state.
- **R8.4** Per relay: a mismatch raises its warning (`HeaterFBMismatch` / `CoolerFBMismatch`) **immediately** and starts the `RelayFeedbackTimeout` countdown. A match clears the warning and resets the countdown. Reaching the timeout raises that relay's **fault**.
- **R8.5** `RelayFeedbackTimeout` must exceed the worst-case host DO-loop latency; state this in the configuration notes.

### R9 — Init, Reset and faults

- **R9.1 (Init).** Validates and stores the setup (R5.9 checks), sets the time reference, clears faults, warnings, averages, accumulators, hourly counts, countdowns, `Initial_HC_Flag`, and sets `ActiveSensor = 1`. Allowed at any time, including on a stopped zone — Init is a deliberate user action or part of power-up.
- **R9.2 (Reset).** Keeps the stored setup. Clears faults, warnings, averages, accumulators, hourly counts, countdowns and `Initial_HC_Flag`, sets `ActiveSensor = 1`, sets both relay commands to 0, and sets the time reference. It does **not** clear `ConfigFault` (R9.5).
- **R9.3 (relay state across Init).** If the zone is currently enabled and running (not disabled, not stopped) **and** the new setup has `Enable = 1` **and** passes the config check, the internal relay commands are **kept**, and the new setup's normal logic then decides whether each relay stays on or drops. In every other case — first Init after power-up, zone previously disabled or stopped, new `Enable = 0`, failed config check — both relays start at 0. *(Replaces v2:R9.1's `in[15..16]`, which are removed.)*
- **R9.4** No `TC_ERR_NOT_INIT`: a call on a zone with no setup is a no-op (R2.4).
- **R9.5 (config check, at Init).** Checks:
  1. `TempUnits` is 0 or 1;
  2. `DeadbandHi >= 0`, `DeadbandLo >= 0`, and **not both zero**;
  3. `LoLimit < LoBand <= Setpoint <= HiBand < HiLimit` — the setpoint and both band edges lie strictly inside the sensor limits;
  4. every applicable timeout `> 0` (0, negative and NaN all fail);
  5. no NaN in any applicable setup value;
  6. when `Temp2Enable = 1`: `Temp2Tolerance >= 0`, `Temp2Offset` finite, `TempCompareTimeout > 0`;
  7. when `FeedbackEnable = 1`: `RelayFeedbackTimeout > 0`.
  `FilterPoints` is never checked here (R4.2). Parameters belonging to a disabled feature are not checked.
  - **Enable = 1 and a check fails** → `ConfigFault`: zone stopped, relays 0, latched. Only a subsequent Init that passes clears it — Reset does not.
  - **Enable = 0 and a check fails** → warning `ConfigInvalid`; the zone stays disabled and takes no action.
- **R9.6 (fault behaviour).** On any fault: `doHeater = doCooler = 0`; the fault code latches into `status`; the warning code freezes at its last value; every check, countdown and average stops. Only Reset or Init resumes the zone. The first fault wins — no later fault can overwrite it. If two conditions mature on the same tick, evaluate in this order: config → sensor range → disagreement → heater feedback → cooler feedback.
- **R9.7** Faults never self-clear, and failed sensors are never re-admitted.

---

## 6. Status codes (`status`, one value, states and faults share the enum)

| Code | Name | Meaning |
|---|---|---|
| 0 | `TempCtrlDisabled` | `TempCtrlEnable = 0`, or no setup loaded. Relays 0; nothing evaluated. |
| 1 | `TempAtSetPt` | Enabled, relays off, `ControlTemp` inside the deadband. |
| 2 | `HeaterON` | Heating commanded (held through the at-setpoint countdown). |
| 3 | `CoolerON` | Cooling commanded (held through the at-setpoint countdown). |
| 4 | `HeatPending` | Relays off, below `LoBand`, deadband countdown running. |
| 5 | `CoolPending` | Relays off, above `HiBand`, deadband countdown running. |
| 6–9 | *reserved* | Future states. |
| 10 | `Temp1FailHigh` | **Fault.** Single-sensor mode: sensor 1 failed high (includes NaN/Inf). |
| 11 | `Temp1FailLow` | **Fault.** Single-sensor mode: sensor 1 failed low. |
| 12 | `BothSensorsFailed` | **Fault.** Two-sensor mode: no healthy sensor remains. |
| 13 | `TempDisagreeFault` | **Fault.** Sensors disagreed for `TempCompareTimeout`. |
| 14 | `ConfigFault` | **Fault.** Init config check failed with `Enable = 1`. Cleared only by a passing Init. |
| 15 | `HeaterFBFault` | **Fault.** Heater DO feedback mismatched for `RelayFeedbackTimeout`. |
| 16 | `CoolerFBFault` | **Fault.** Cooler DO feedback mismatched for `RelayFeedbackTimeout`. |
| 17+ | *reserved* | Future faults. |

Any code >= 10 is a fault, and the host treats it as a stand shutdown.

---

## 7. Warning codes (`warning`, single value, lowest number wins)

A bit-packed output may replace this later; keep the codes stable if so.

| Code | Name | Sets | Clears |
|---|---|---|---|
| 0 | `NoWarning` | — | — |
| 1 | `Temp1OutOfRange` | Sensor 1's raw value is out of range now (accumulator running or already failed) | Sensor 1 back in range |
| 2 | `Temp2OutOfRange` | Same for sensor 2's corrected value (`Temp2Enable = 1`) | Sensor 2 back in range |
| 3 | `HeaterFBMismatch` | `diHeaterFB` ≠ previous heater command | They match |
| 4 | `CoolerFBMismatch` | `diCoolerFB` ≠ previous cooler command | They match |
| 5 | `TempDisagree` | Disagreement held for `TempCompareTimeout / 10` | Agreement, or the comparison pauses |
| 6 | `RunningOnTemp2` | Sensor 1 failed and control switched to sensor 2 | Reset or Init only; temporarily masked by codes 1–5 |
| 7 | `ConfigInvalid` | Init with `Enable = 0` failed the config check | The next Init that passes |

Warnings never latch (code 6 excepted, which persists until Reset or Init) and never change control. While a zone is stopped on a fault, the warning code freezes.

---

## 8. Change list vs v2.0.1, with acceptance tests

| # | Change | v2.0.1 today | v3.0.0 | Acceptance test |
|---|---|---|---|---|
| C1 | API split | One `TcStep` with `in[17]`/`out[27]` | `TcInit` / `TcCheckTemp` / `TcReset` / `TcGetDiag` + 3 helpers (§2) | Wizard-generated wrappers call each function; `TcSetupCount() = 17`, `TcDiagCount() = 25`, `TcVersion() = 0x030000` |
| C2 | Setup loaded at Init only | Every input re-read each call (R1.6) | R1.3; a parameter change means a new Init | Changing the setup array between CheckTemp calls has no effect until Init |
| C3 | `TempCtrlEnable` | absent | R2.1–R2.4, setup index 0 | With Enable = 0 the relays stay 0 and no warning or fault appears under any stimulus; an uninitialised zone returns `TC_OK` + status 0 |
| C4 | `TempUnits` | absent | R3, label only | Units 0 and 1 give identical control behaviour; units = 2 → `ConfigFault` |
| C5 | `Temp2Offset` | absent | R6.1 | With offset 5 and temp2 = 95, the corrected value 100 is used for the limit check, comparison and post-switch control |
| C6 | Disagreement is a fault | flag only (R6.2) | R6.3–R6.4, two stages | Warning at `T/10`, fault at `T`; agreement between the two clears and restarts |
| C7 | `TempCompareTimeout` | absent | setup 13 | As C6 |
| C8 | `AtSetPtTimeout` | absent, release on one sample (R7.2) | R7.2 | Heating to SP: a single sample at SP does not drop the relay; it drops after the timeout of continuous at-setpoint |
| C9 | Raw control and limits | filtered values (R5.2, R7) | R4.1 | With `FilterPoints = 64`, control still reacts at the raw value; averages affect only the comparison |
| C10 | NaN handling | separate `T*_BAD` bits (R5.2) | R4.4 sentinel; `T*_BAD` removed | A NaN stream fails the sensor high after `ErrorTimeout`; a single NaN never changes a relay or corrupts an average |
| C11 | Leaky accumulator | countdown restarted on recovery (R5.3) | R5.4, `DRAIN = 0.5` | *(corrected, Amendment A)* 100 % duty fails at 1 × `E`; 50 % duty fails at ≈ 4 × `E`; 33 % duty or less never fails; a single-tick glitch never fails; the accumulator charges on its first out-of-range tick |
| C12 | Single-sensor failure is not a fault | latched fault bits + DEGRADED (R5.7) | R5.5, warning `RunningOnTemp2` | Sensor 1 fails with sensor 2 healthy → control continues, status stays a state, warning 6 |
| C13 | Control pause | relays dropped at once (R5.9) | R5.7 | Active sensor goes out of range mid-heating → relays hold and countdowns freeze until it returns or the sensor fails |
| C14 | `Initial_HC_Flag` | `WARMUP` status (R4.3) | R7.5 | In-band start sets it on tick 1; an out-of-band start sets it when the at-setpoint countdown completes; the comparison is inactive until then |
| C15 | DO feedback | 1-tick compare, flag only (R6.3–R6.5) | R8, separate warning and fault per relay, `RelayFeedbackTimeout` | A 1-tick mismatch warns only; a sustained one faults that relay; heater and cooler independent; `FeedbackEnable = 0` disables both |
| C16 | Config check | warn and run (R8.3) | R9.5, `ConfigFault` / `ConfigInvalid` | Reversed band, deadbands both 0, timeout 0 or negative, NaN, bad units each fault with Enable = 1, and warn with Enable = 0; Reset does not clear `ConfigFault` |
| C17 | Reset clears history | kept the filters (R9.2) | R9.2 | After Reset the averages are empty and `Initial_HC_Flag` is 0 |
| C18 | Relay state across Init | from `in[15..16]` (R9.1) | R9.3 | Setpoint change by Init while heating keeps the heater on; Init after a fault starts both relays off |
| C19 | Status and warning outputs | bit mask + priority status (R7.5, R8.1) | §6, §7 | Every state and fault is reachable in test; a fault never hides which relay was commanded (`TcGetDiag` mirrors) |
| C20 | Input echo / CAN | `out[0..14]` echoed (R3.1) | removed | No echo anywhere in the API |
| C21 | Time robustness | wrap only (R1.4) | R1.5 | A backwards `nowMs` step expires nothing |
| C22 | Diagnostics | none | §4, `TcGetDiag` | Each index reports the documented value; calling it repeatedly changes no state |

---

## 9. Build and packaging requirements

1. **LabVIEW-importable header.** Ship `.h` files intended for the LabVIEW **Import Shared Library** wizard, which generates the wrapper VIs:
   - **no `int64_t` / `uint64_t` anywhere in the exported API** — these produce wizard errors requiring manual correction;
   - only `int32_t`, `uint32_t`, `double` in signatures;
   - arrays as a pointer plus an `int32_t` length;
   - no structs, enums or typedef'd parameter types (codes are plain `int32_t`);
   - no complex macros in the header; `cdecl` calling convention;
   - self-describing parameter names (`setupArray`, `diagArray`, `doHeater`, `diHeaterFB`, …) and a doc comment per function listing the array indexes.
   - **Acceptance:** the wizard runs on the header with no manual corrections beyond marking `setupArray` / `diagArray` as arrays.
   - **Ship the header inside every build package** — the Windows x64 `.dll` package and the cRIO x86_64 `.so` package each contain the `.h`, not only the source tree. It is the **same file** for both (only fixed-width integer types and `double`, nothing platform-specific), so one set of wrapper VIs generated from it serves both: test against the `.dll`, deploy the `.so`, regenerate nothing. If the header ever has to differ per target, that is a defect to report rather than to ship.
2. **TempSim.exe must be updated** to the v3.0.0 API and must exercise every scenario in §10. Keep the existing scenario-runner style, print one line per tick with `status`, `warning`, relay commands and key diagnostics, and support a scripted temperature profile so the new timing rules can be verified.
3. **Unit tests** cover every rule in §5 and every row in §8.
4. **Determinism:** no heap allocation, I/O or blocking anywhere in `TcInit`, `TcCheckTemp`, `TcReset` or `TcGetDiag`; all state static. Memory for the hourly ring buffers is static (16 zones × 2 sensors × 60 entries).
5. **Version:** `TcVersion()` returns `0x030000`. Bump it and `TcSetupCount()` together on any future array change.
6. **Documentation to ship with the build:** the setup and diagnostics index tables, the status and warning tables, the recommended Call Library Function Node settings (cdecl, "run in any thread", array pass-by-pointer), and a note that `RelayFeedbackTimeout` must exceed the host DO-loop latency, plus an explanation of the leaky accumulator behaviour (a flickering sensor eventually fails).

---

## 10. Required test scenarios (TempSim and unit tests)

1. **Single-sensor heat-up:** cold start out of band → HeatPending → HeaterON → at-setpoint countdown → TempAtSetPt → in-band idle.
2. **Setpoint change by re-Init while heating:** relays kept, control continues to the new setpoint.
3. **Cool-down:** setpoint moved below the temperature → CoolPending → CoolerON → release at the setpoint.
4. **Chatter immunity:** single spike and single NaN samples in every state — no relay transition, no average corruption.
5. **Flickering sensor:** 50 % duty in/out of range → failure at ≈ 2 × `ErrorTimeout`; sparse glitches → no failure; `OorEventsPerHour` counts the transitions.
6. **Control pause:** the active sensor leaves range mid-heating → relays hold, countdowns freeze, then resume on recovery.
7. **Failover:** sensor 1 fails with sensor 2 healthy (offset applied) → control continues on sensor 2, warning `RunningOnTemp2`, no fault; sensor 2 then fails → `BothSensorsFailed`.
8. **Disagreement:** sensor 2 drifts beyond tolerance → warning at `T/10`, fault at `T`; recovery in between clears and restarts.
9. **Comparison gating:** no comparison before `Initial_HC_Flag`, during a range excursion, or before the averages are full.
10. **Relay feedback:** 1-tick mismatch (warning only), sustained mismatch (fault), heater and cooler independent, `FeedbackEnable = 0` silent, no checking while stopped.
11. **Config faults:** reversed band, both deadbands 0, band edge outside the limits, zero/negative/NaN timeouts, bad units — each with `Enable = 1` (fault) and `Enable = 0` (warning); Reset does not clear `ConfigFault`.
12. **Enable/disable:** disabled zone is inert under every stimulus; an uninitialised zone returns `TC_OK` with status 0.
13. **Reset:** clears faults, averages, accumulators, hourly counts and `Initial_HC_Flag`, with relays off; Reset with the fault condition still present faults again after the timeout.
14. **Two zones:** different setups, stepped alternately in the same tick, with no interaction.
15. **Time:** 2^32 wrap during a countdown; a backwards `nowMs` step; a long gap between calls.

---

## 11. Known coverage gaps (document; do not implement in v3.0.0)

1. **Physically stuck relay.** Feedback reads the DO state, so a welded or failed contact with a correct DO is not detected.
2. **No heating or cooling progress.** A heater that runs without reaching the setpoint stays on indefinitely; there is no max-on-time or no-progress fault. The operator sees it on the display.
3. **Wrong-but-in-range sensor.** After one sensor has failed, the survivor's plausibility is no longer cross-checked; an in-range but incorrect reading is undetected.
4. **Single warning output.** Only the highest-priority warning is visible; lower ones are hidden until it clears.
5. **No process over-temperature independent of sensor validity.** `HiLimit`/`LoLimit` serve both as sensor-range and as trip limits.
