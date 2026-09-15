# TempCtl v2.0.1 - Controller Requirements (for markup)

Status: describes the controller as shipped in TempCtl v2.0.1 (2026-09-12), the
version behind the current TempSim.exe. Every statement below is implemented and
covered by the unit tests; the normative source is `src/tempctl.h`. Numbered
items are there so a markup can say "change R3.4" or "add R6.5". Words in
`code` are the names used in the header and the simulator.

## 1. Scope and interface

- **R1.1** The controller is pure signals in, signals out. It owns no hardware
  and no CAN. One call per control tick:
  `TcStep(zone, action, nowMs, in[17], out[27])`.
- **R1.2** `zone` selects one of 16 independent controllers (0..15). Zones share
  nothing.
- **R1.3** `action` is `Init` (0), `Step` (1) or `Reset` (2). See section 9.
- **R1.4** `nowMs` is a free-running millisecond tick (LabVIEW *Tick Count
  (ms)*). The controller differences successive values; wrap-around at 2^32 is
  handled. The absolute value has no meaning.
- **R1.5** `in` and `out` are SGL arrays. `in` needs at least 17 elements, `out`
  at least 27. They may be the same array.
- **R1.6** Configuration is live: every input is read on every call, a change
  takes effect on the next `Step`, no re-Init.
- **R1.7** The DLL (x64, x86) and the .so (x86_64 cRIO, aarch64 and armhf Pi)
  are built from the same source with a plain C interface (cdecl), no
  dependencies beyond libc.
- **R1.8** `TcVersion()` returns 2.0.1 as `(major<<16)|(minor<<8)|patch`;
  `TcInputCount()` = 17 and `TcSignalCount()` = 27 let the caller size arrays.

## 2. Inputs (`in[0..16]`)

| # | Name | Unit | Meaning |
|---|---|---|---|
| 0 | `Setpoint` | deg | Target. Heating runs until ControlTemp >= Setpoint, cooling until <= Setpoint. |
| 1 | `DeadbandHi` | deg, >= 0 | `HiBand = Setpoint + DeadbandHi`. |
| 2 | `DeadbandLo` | deg, >= 0 | `LoBand = Setpoint - DeadbandLo`. |
| 3 | `HiLimit` | deg | A sensor above this for `ErrorTimeout` has failed high. |
| 4 | `LoLimit` | deg | A sensor below this for `ErrorTimeout` has failed low. |
| 5 | `ErrorTimeout` | ms, >= 0 | Countdown length for every fault condition (limits, bad reading, disagreement, relay feedback). |
| 6 | `DeadbandTimeout` | ms, >= 0 | Time ControlTemp must sit outside the band before a relay engages. |
| 7 | `FilterPoints` | 1..64 | Moving-average length per sensor. 0 means 4. |
| 8 | `Temp2Enable` | 0/1 | Second sensor present: rationality check, disagreement check and failover. |
| 9 | `Temp2Tolerance` | deg, >= 0 | Allowed difference between the two filtered sensors. |
| 10 | `FeedbackEnable` | 0/1 | Compare the relay feedback inputs with the commands. |
| 11 | `Temp1` | deg | Sensor 1, the primary. NaN/Inf = bad reading (open thermocouple). |
| 12 | `Temp2` | deg | Sensor 2. Ignored unless `Temp2Enable`. |
| 13 | `HeaterFeedback` | 0/1 | Measured heater relay state. Ignored unless `FeedbackEnable`. |
| 14 | `CoolerFeedback` | 0/1 | Measured cooler relay state. |
| 15 | `HeatingCmd` | 0/1 | Read on `Init` only: relay state the controller starts from. |
| 16 | `CoolingCmd` | 0/1 | Read on `Init` only. If both are 1, heating wins. |

Boolean inputs are true when > 0.5.

## 3. Outputs (`out[0..26]`)

- **R3.1** `out[0..14]` echo the inputs unchanged, so the whole array can go to
  CAN (the 27-signal layout is the TempCtl DBC message, PGN 65280).
- **R3.2** `out[15] HeatingCmd` and `out[16] CoolingCmd` are the relay commands
  for this tick, never both 1.

| # | Name | Meaning |
|---|---|---|
| 17 | `ErrorStatus` | Bit mask, section 8. |
| 18 | `TempStatus` | State code 0..8, section 7. |
| 19 | `ControlTemp` | Filtered value of the active sensor; the value control acts on. |
| 20 | `Temp1Filtered` | Moving average of sensor 1. NaN until the first valid sample. |
| 21 | `Temp2Filtered` | Moving average of sensor 2. NaN when `Temp2Enable` = 0 or no valid sample yet. |
| 22 | `HiBand` | `Setpoint + DeadbandHi`. |
| 23 | `LoBand` | `Setpoint - DeadbandLo`. |
| 24 | `ErrorRemainMs` | Smallest error countdown still running; 0 when none (or stopped). |
| 25 | `DbRemainMs` | Remaining deadband countdown; 0 when idle in band or while a relay is on. |
| 26 | `ActiveSensor` | 1 or 2: the sensor whose filtered value is `ControlTemp`. |

## 4. Filtering and warm-up

- **R4.1** Each sensor has its own moving average of `FilterPoints` samples.
  Only valid samples (not NaN, not Inf) enter it; a bad sample is skipped, it
  does not corrupt the average.
- **R4.2** The filtered value is NaN until the first valid sample, then the
  average of the samples present (fewer than `FilterPoints` during warm-up).
- **R4.3** `TempStatus` reports `WARMUP` (8) while the active sensor's filter
  holds fewer than `FilterPoints` samples. Control still runs on the partial
  average during warm-up.
- **R4.4** Filters are also fed on `Init` and `Reset` (the sample passed with
  that call counts). `Init` clears the filter history; `Reset` keeps it.
- **R4.5** Changing `FilterPoints` at run time takes effect immediately on the
  next average (the 64-sample history is always kept).

## 5. Sensor limits, bad readings, failover

- **R5.1** Sensor 1 is always checked. Sensor 2 is checked only when
  `Temp2Enable` = 1. A sensor that has already failed is not re-checked.
- **R5.2** Per sensor and per tick one condition is derived, first match wins:
  *bad* (raw NaN/Inf, or no filtered value yet), else *high* (filtered >
  `HiLimit`), else *low* (filtered < `LoLimit`), else none.
- **R5.3** A condition starts a countdown of `ErrorTimeout` ms at the first
  `Step` that observes it. If the condition changes (for example high -> none,
  or high -> bad) the countdown restarts from full or clears. When the
  countdown reaches 0 the sensor has **failed** and the matching bit latches
  (`T1_HI`, `T1_LO`, `T1_BAD`, `T2_HI`, `T2_LO`, `T2_BAD`).
- **R5.4** `ErrorTimeout` = 0 latches on the first observation.
- **R5.5** A failed sensor stays failed until `Reset` or `Init`, even if its
  reading returns to normal (no automatic recovery).
- **R5.6** `Temp2Enable` = 0: the active sensor is always 1. Sensor 1 failed ->
  `STOPPED`.
- **R5.7** `Temp2Enable` = 1: if the active sensor fails and the other has not,
  `ActiveSensor` switches to the other one and `TempStatus` reports `DEGRADED`
  (7) for as long as exactly one sensor is failed. Both failed -> `STOPPED`.
- **R5.8** After a failover the controller does not switch back on its own;
  `Reset` returns the active sensor to 1.
- **R5.9** *Bad active reading, before the countdown expires:* while the active
  sensor's raw value is NaN/Inf (or its filtered value is NaN) both relays are
  off immediately and the deadband countdown is cleared. The error countdown
  runs meanwhile (`ERROR_PENDING`). This is the v1 behaviour, kept.

## 6. Disagreement and relay feedback (operation continues)

- **R6.1** Disagreement is evaluated only when `Temp2Enable` = 1, neither sensor
  has failed, both filtered values exist and the bit is not yet latched.
- **R6.2** |`Temp1Filtered` - `Temp2Filtered`| > `Temp2Tolerance` for
  `ErrorTimeout` ms latches `DISAGREE` (bit 6). Control continues on the active
  sensor; the active sensor does not change.
- **R6.3** With `FeedbackEnable` = 1 each relay's feedback input is compared
  with the command the controller issued on the **previous** call (one tick of
  lag is allowed by construction, more is a mismatch).
- **R6.4** A mismatch held for `ErrorTimeout` ms latches `HEATER_FB` (bit 7) or
  `COOLER_FB` (bit 8). The controller keeps commanding the relays as before; it
  does not stop and does not change the command.
- **R6.5** Once a disagreement or feedback bit is latched that check is no
  longer evaluated until `Reset`.

## 7. Heating and cooling (deadband control)

All decisions use `ControlTemp` = filtered value of the active sensor.

- **R7.1** Idle, `ControlTemp` > `HiBand` for `DeadbandTimeout` ms -> cooling
  on. Idle, `ControlTemp` < `LoBand` for `DeadbandTimeout` ms -> heating on.
  Inside the band the countdown is cleared; crossing from one side to the
  other restarts it.
- **R7.2** Heating stays on until `ControlTemp` >= `Setpoint`, cooling until
  `ControlTemp` <= `Setpoint`. Then the relay drops and the controller is idle
  again (so the plant is driven to the setpoint, not to the band edge).
- **R7.3** Heating and cooling are mutually exclusive; while a relay is on the
  deadband countdown is not in play (`DbRemainMs` = 0).
- **R7.4** `DeadbandTimeout` = 0 engages the relay on the first tick outside
  the band.
- **R7.5** `TempStatus` when several apply, highest first:

| Code | Name | When |
|---|---|---|
| 6 | `STOPPED` | Latched fault, relays off, until `Reset`. |
| 5 | `ERROR_PENDING` | Any error countdown is running (`ErrorRemainMs` > 0). |
| 8 | `WARMUP` | Active filter not yet full. |
| 7 | `DEGRADED` | `Temp2Enable` and exactly one sensor failed. |
| 2 | `HEATING` | Heating relay on. |
| 4 | `COOLING` | Cooling relay on. |
| 3 | `COOL_PENDING` | Above `HiBand`, deadband countdown running. |
| 1 | `HEAT_PENDING` | Below `LoBand`, deadband countdown running. |
| 0 | `IN_BAND` | Idle inside the band. |

Note: `ERROR_PENDING`, `WARMUP` and `DEGRADED` mask the control states in this
code, but the relay commands in `out[15..16]` are always the true commands.
Read the relay state from the commands, not from `TempStatus`.

## 8. Faults, `ErrorStatus` bits and return codes

- **R8.1** `ErrorStatus` (out[17]) is a bit mask. Bits 0..8 latch until `Reset`
  or `Init`. Bit 9 is not latched; it follows the configuration of the current
  call.

| Bit | Name | Latches when | Effect on control |
|---|---|---|---|
| 0 | `T1_HI` | Temp1 filtered > `HiLimit` for `ErrorTimeout` | Sensor 1 failed: failover or stop. |
| 1 | `T1_LO` | Temp1 filtered < `LoLimit` for `ErrorTimeout` | Sensor 1 failed. |
| 2 | `T1_BAD` | Temp1 NaN/Inf for `ErrorTimeout` | Sensor 1 failed (relays already off, R5.9). |
| 3 | `T2_HI` | as bit 0, sensor 2 | Sensor 2 failed: failover or stop. |
| 4 | `T2_LO` | as bit 1, sensor 2 | Sensor 2 failed. |
| 5 | `T2_BAD` | as bit 2, sensor 2 | Sensor 2 failed. |
| 6 | `DISAGREE` | filtered sensors differ by more than `Temp2Tolerance` for `ErrorTimeout` | None, flag only. |
| 7 | `HEATER_FB` | heater feedback != heating command for `ErrorTimeout` | None, flag only. |
| 8 | `COOLER_FB` | cooler feedback != cooling command for `ErrorTimeout` | None, flag only. |
| 9 | `CONFIG` | configuration inconsistent (R8.3) | None, controller runs with the values as given. |

- **R8.2** `STOPPED` (fault stop): relays off, all countdowns cleared, latched
  until `Reset`. While stopped, `Step` only keeps the filters running; nothing
  else is evaluated and `ErrorRemainMs` = `DbRemainMs` = 0. The latched bits
  stay visible so the cause can be read.
- **R8.3** Configuration check, every call: `DeadbandHi`, `DeadbandLo` >= 0;
  `LoLimit` < `LoBand` and `HiBand` < `HiLimit`; both timeouts >= 0;
  `Temp2Tolerance` >= 0; `FilterPoints` in 0..64. A NaN in any of these fails
  the check. Failure sets bit 9 and returns `TC_WARN_CONFIG` (+1); the call
  still runs. `FilterPoints` outside 1..64 is clamped (negative or NaN -> 4,
  above 64 -> 64, 0 -> 4 without a warning).
- **R8.4** Return value of `TcStep`:

| Value | Name | Meaning |
|---|---|---|
| 0 | `TC_OK` | Ran. |
| +1 | `TC_WARN_CONFIG` | Ran, configuration inconsistent (bit 9 set). |
| -1 | `TC_ERR_ARG` | Null pointer, or `in` < 17 / `out` < 27 elements. Nothing ran. |
| -2 | `TC_ERR_ZONE` | `zone` outside 0..15. |
| -3 | `TC_ERR_ACTION` | `action` not 0, 1 or 2. |
| -4 | `TC_ERR_NOT_INIT` | `Step` on a zone that has had no `Init` (or `Reset`). |

## 9. Init and Reset

- **R9.1** `Init` (0): clears everything in the zone, including filter history
  and latched bits. Active sensor = 1. Relay state is taken from `in[15..16]`
  (so a controller can be started with a relay already on, heating wins if
  both). The sample in the call is the first filter sample. `nowMs` becomes
  the time reference.
- **R9.2** `Reset` (2): clears the fault stop, all latched bits (0..8) and all
  countdowns. Relays off, active sensor = 1. Filter history is **kept**, so
  control resumes without a new warm-up. `nowMs` becomes the time reference.
  `Reset` on a zone that was never initialised behaves as an `Init` with
  empty filters.
- **R9.3** Neither action needs the fault to be gone: a `Reset` with a sensor
  still bad simply starts the countdown again and will stop again after
  `ErrorTimeout`.

## 10. TempSim scenarios (what the current build exercises)

| Scenario | Shows |
|---|---|
| `warmup` | Cold start: warm-up, heat pending, heating to setpoint, in-band cycling. |
| `setpoint-step` | Setpoint 50 -> 30 (cooler engages) -> 70. |
| `sensor-failover` | Sensor 1 opens: relays drop, `T1_BAD` after `ErrorTimeout`, failover to sensor 2 (degraded), sensor 1 returns but stays failed until reset. |
| `both-sensors-fail` | Sensor 2 sticks high (degraded), then sensor 1 opens: stopped; reset with both healthy. |
| `disagree` | Sensor 2 drifts +8 deg: `DISAGREE` bit, control continues on sensor 1. |
| `feedback-fault` | Heater sticks open (`HEATER_FB`), cooler sticks closed (`COOLER_FB`) and drives the plant below `LoLimit` until both sensors fail low and the controller stops. |
| `single-sensor` | One sensor, no feedback: sensor opens -> stopped; reset. |

## 11. Points I would like your view on

These are the rules most likely to need "extra rules around when faults
occur". Mark any of them up or add your own.

1. **Sensor recovery (R5.5, R5.8).** A failed sensor never recovers by itself
   and the controller never fails back to sensor 1. Should a sensor that reads
   normally for some time be re-admitted, and should control return to sensor
   1 when it does?
2. **Feedback and disagreement bits (R6.4, R6.5).** They are flags only and
   stay latched. Should a relay feedback mismatch stop the controller (a stuck
   cooler can drive the plant out of limits, see `feedback-fault`), and should
   the bits self-clear when the condition goes away?
3. **Disagreement with no failure (R6.2).** When the two sensors disagree
   nothing decides which one is right; control stays on the active sensor.
   Should disagreement stop, or switch to the more plausible sensor?
4. **`TempStatus` priority (R7.5).** `ERROR_PENDING` and `WARMUP` hide
   `HEATING`/`COOLING`. If the operator display should show the relay state
   during a countdown, the priority order or a separate field is the change.
5. **Bad active reading (R5.9).** Relays drop at once on a NaN, before the
   sensor is declared failed. With `Temp2Enable` this means a short sensor
   glitch interrupts heating even though sensor 2 is healthy. Failover only
   happens after `ErrorTimeout`.
6. **Configuration warning (R8.3).** An inconsistent configuration is reported
   but not refused. Should certain errors (for example `HiLimit` <= `HiBand`)
   force the relays off?
7. **Deadband return to setpoint (R7.2).** A relay stays on until the setpoint
   is reached, not until the band edge. Confirm this is the intended overshoot
   behaviour for the stand.
