# TempCtl v3.0.0 - Capability and Operating Rules

Temperature controller library (.dll for Windows test, .so for the cRIO).
This document describes what the controller does and the rules it follows;
`TEMPCTL-SPEC-v3.0.0.md` has the numbered requirements and
`TEMPCTL_PACKAGE_GUIDE.md` the API tables.

## 1. What it does

TempCtl is signals in, signals out. It owns no hardware and no timing. The
host application reads and scales the temperatures, calls the controller once
per zone per loop pass (nominally every 100 ms), and drives the relays from
the commands it returns.

Per zone it provides:

- Deadband heating and cooling control of one heater and one cooler relay.
- Validity checking of one or two temperature sensors against a high and low limit.
- Fallback to the second sensor, and a cross-check between the two.
- A check that the digital outputs followed the commands.
- A status code, a warning code and a diagnostics array.

Sixteen independent zones (0 to 15) share nothing. Faults are shutdown
events: the host shuts the stand down on any controller fault. Warnings are
information only and never change control.

## 2. Calls and setup

Initialize loads the whole setup. CheckTemp then passes only the live
signals: the temperatures and the relay feedback. Changing any parameter
means calling Initialize again with the full setup.

| Call | Arguments | Use |
|---|---|---|
| `TcInit` | zone, nowMs, setupArray[17], setupLen, status, warning | Loads the setup. Call once per zone at startup, and again for any parameter change. |
| `TcCheckTemp` | zone, nowMs, temp1, temp2, diHeaterFB, diCoolerFB, doHeater, doCooler, status, warning | One control step. Call once per zone per loop pass. |
| `TcReset` | zone, nowMs, status, warning | Clears faults, warnings, history and timers; keeps the setup; relays off. |
| `TcGetDiag` | zone, diagArray[25], diagLen | Reads the diagnostics. Read-only; changes nothing. |
| `TcVersion` | - | Library version, 0x030000 for 3.0.0. |
| `TcSetupCount` | - | Setup array length (17). |
| `TcDiagCount` | - | Diagnostics array length (25). |

Every call takes the current millisecond tick, which the controller uses for
all its timers. A call returns 0 when it ran, or a negative code for a bad
argument (wrong zone, null pointer, wrong array length), in which case
nothing runs and no output changes.

## 3. Operating rules

No relay ever changes state because of a single reading. Every relay
transition waits out a timeout. This protects the relays and the heating and
cooling equipment, and is the reason for the timeouts throughout these rules.

### 3.1 Enable, Initialize and Reset

- A zone is disabled until a setup with `TempCtrlEnable = 1` is loaded. While disabled the controller takes no action at all: relays off, no checks, no timers, no faults.
- Initialize is allowed at any time, including on a faulted zone, and clears faults, history and timers. If the zone was already running and the new setup is valid, the relays keep their current state and the new settings decide what happens next. Otherwise both relays start off.
- Reset keeps the setup but clears faults, warnings, history, timers and the averages, and turns both relays off. It does not clear a configuration fault.
- Initialize checks the setup: the units, the deadbands (not negative and not both zero), the setpoint and both band edges inside the sensor limits, and every timeout above zero with no NaN. While enabled, a failed check is a configuration fault; while disabled it is only a warning.

### 3.2 Signal high and low checks

- `HiLimit` and `LoLimit` define a valid sensor reading. A reading outside them, including NaN or infinity, is out of range.
- Out-of-range time accumulates; in-range time drains it back at half the rate. When the accumulated time reaches `ErrorTimeout` the sensor has failed. A sensor that is out of range more than a third of the time therefore still fails (half the time: after about four `ErrorTimeout`s), instead of holding the system in a permanent pending state, while an isolated glitch drains away and a cleanly recovered sensor returns to zero.
- `ErrorTimeout` must be at least two loop periods (200 ms at a 100 ms loop). A shorter value lets a single out-of-range reading fail the sensor, and drop both relays, from one sample; the controller cannot check this because it does not know the loop rate.
- While the sensor in control is out of range, the deadband and at-setpoint checks pause: the relays hold and their timers freeze until the reading returns or the sensor fails.
- A failed sensor is never returned to service. Only Reset or Initialize restores it.
- With one sensor fitted, a failure is a fault. With two fitted, a failure with a healthy partner is only a warning, and the fault comes when no healthy sensor is left.

### 3.3 Deadband checks

- The deadbands set the switching points: `HiBand` above the setpoint, `LoBand` below it.
- Below `LoBand` continuously for `DeadbandTimeout` turns heating on; above `HiBand` for the same time turns cooling on. Returning inside the band clears the timer, and crossing to the other side restarts it.
- Heating and cooling are mutually exclusive, and the deadband timer is idle while a relay is on.

### 3.4 At-setpoint check

- A running relay turns off only after the temperature has held at the setpoint for `AtSetPtTimeout`: at or above it while heating, at or below it while cooling.
- Falling back across the setpoint restarts the timer, so the relay keeps running.
- Completing this timer also sets `Initial_HC_Flag`, which marks the end of the first heat-up or cool-down. A zone that starts inside the deadband sets the flag immediately.

### 3.5 Two sensors

- Sensor 1 is the primary. Sensor 2 is optional and may sit in a different location, so `Temp2Offset` is added to its reading before every use.
- If the sensor in control fails and the other is healthy, control moves to it and the stand keeps running, with a warning. Control never moves back on its own.
- Both sensors are averaged over `FilterPoints` readings, and the averages are compared. Averaging is used only for this comparison; the limit checks and control use the raw readings.
- A difference larger than `Temp2Tolerance` raises a warning after a tenth of `TempCompareTimeout`, and a fault after the full time. Agreement clears the warning at once and restarts the timer.
- The comparison is inactive until `Initial_HC_Flag` is set, while either sensor is out of range, and once either sensor has failed. Nothing decides which sensor is right, so a lasting disagreement always faults.

### 3.6 Relay output feedback

- The feedback inputs read back the digital output state, not the physical relay contact.
- Each feedback is compared with that relay command from the previous step. A mismatch raises a warning at once and starts the timer; a match clears it. A mismatch lasting `RelayFeedbackTimeout` faults that relay.
- The heater and cooler are checked independently. `FeedbackEnable` turns both checks off. Set `RelayFeedbackTimeout` longer than the delay through the output loop.

### 3.7 What happens on a fault

Both relay commands go to 0, the first fault is latched in the status, the
warning code freezes, and every check and timer stops. The zone stays there
until Reset or Initialize. Freezing keeps the conditions at the moment of the
fault visible for troubleshooting.

## 4. Status codes

| Code | Name | Type | Meaning |
|---|---|---|---|
| 0 | TempCtrlDisabled | State | Not enabled, or no setup loaded. Relays off, nothing evaluated. |
| 1 | TempAtSetPt | State | Relays off and the temperature is inside the deadband. |
| 2 | HeaterON | State | Heating commanded. |
| 3 | CoolerON | State | Cooling commanded. |
| 4 | HeatPending | State | Below the band, waiting out the deadband timeout. |
| 5 | CoolPending | State | Above the band, waiting out the deadband timeout. |
| 6-9 | reserved | - | Future states. |
| 10 | Temp1FailHigh | Fault | One sensor fitted: sensor 1 above HiLimit past the timeout. |
| 11 | Temp1FailLow | Fault | One sensor fitted: sensor 1 below LoLimit past the timeout. |
| 12 | BothSensorsFailed | Fault | Two sensors fitted: no healthy sensor is left. |
| 13 | TempDisagreeFault | Fault | The sensors disagreed for TempCompareTimeout. |
| 14 | ConfigFault | Fault | The setup check failed while enabled. Only a valid Initialize clears it. |
| 15 | HeaterFBFault | Fault | Heater DO feedback mismatched past the timeout. |
| 16 | CoolerFBFault | Fault | Cooler DO feedback mismatched past the timeout. |
| 17+ | reserved | - | Future faults. |

## 5. Warning codes

One warning is reported at a time, the lowest code active. Warnings do not change control.

| Code | Name | Sets when | Clears when |
|---|---|---|---|
| 0 | NoWarning | - | - |
| 1 | Temp1OutOfRange | Sensor 1 is outside its limits now. | It returns in range. |
| 2 | Temp2OutOfRange | Corrected sensor 2 is outside its limits now. | It returns in range. |
| 3 | HeaterFBMismatch | Heater feedback does not match the command. | They match. |
| 4 | CoolerFBMismatch | Cooler feedback does not match the command. | They match. |
| 5 | TempDisagree | The sensors have disagreed for a tenth of TempCompareTimeout. | They agree, or the check pauses. |
| 6 | RunningOnTemp2 | Sensor 1 failed and control moved to sensor 2. | Reset or Initialize. |
| 7 | ConfigInvalid | The setup check failed while the zone was disabled. | The next valid Initialize. |

## 6. Setup parameters

One array of doubles, in this order. Each value is read as its own type:
timeouts are whole milliseconds above zero, switches are 0 or 1,
temperatures are in the unit chosen by `TempUnits`.

| # | Parameter | Unit | Meaning |
|---|---|---|---|
| 0 | TempCtrlEnable | 0 / 1 | Master switch for the zone. 0 = the controller takes no action. |
| 1 | TempUnits | 0 / 1 | 0 = degF, 1 = degC. Label only; no conversion is performed. |
| 2 | Setpoint | deg | Target temperature. |
| 3 | DeadbandHi | deg | HiBand = Setpoint + DeadbandHi. |
| 4 | DeadbandLo | deg | LoBand = Setpoint - DeadbandLo. |
| 5 | HiLimit | deg | Sensor valid range, upper end. Must be above HiBand. |
| 6 | LoLimit | deg | Sensor valid range, lower end. Must be below LoBand. |
| 7 | ErrorTimeout | ms | Accumulated out-of-range time before a sensor is declared failed. At least two loop periods. |
| 8 | DeadbandTimeout | ms | Time outside the band before a relay turns on. |
| 9 | AtSetPtTimeout | ms | Time at the setpoint before the running relay turns off. |
| 10 | Temp2Enable | 0 / 1 | Second sensor fitted. |
| 11 | Temp2Offset | deg | Added to sensor 2 to correct for its location. |
| 12 | Temp2Tolerance | deg | Largest allowed difference between the two sensors. |
| 13 | TempCompareTimeout | ms | Disagreement time before a fault. Warning at one tenth of it. |
| 14 | FilterPoints | 1..64 | Averaging length for the sensor comparison. Any other value becomes 4. |
| 15 | FeedbackEnable | 0 / 1 | Turns the DO feedback check on. |
| 16 | RelayFeedbackTimeout | ms | Feedback mismatch time before a fault. |

## 7. Diagnostics

One array of doubles, read with `TcGetDiag` for display, logging or CAN.
Reading it changes nothing.

| # | Value | Meaning |
|---|---|---|
| 0 | ControlTemp | The value control acts on. |
| 1 | ActiveSensor | 1 or 2. |
| 2 | Temp1Raw | Last raw sensor 1 reading. |
| 3 | Temp2Raw | Last raw sensor 2 reading. |
| 4 | Temp2Corrected | Sensor 2 plus its offset. |
| 5 | Temp1Avg | Sensor 1 average (comparison only). |
| 6 | Temp2Avg | Corrected sensor 2 average. |
| 7 | HiBand | Upper switching point. |
| 8 | LoBand | Lower switching point. |
| 9 | Initial_HC_Flag | 1 once the first heat or cool cycle has reached the setpoint. |
| 10 | DeadbandRemainMs | Time left before a relay turns on. |
| 11 | AtSetPtRemainMs | Time left before the running relay turns off. |
| 12 | CompareRemainMs | Time left to the disagreement fault. |
| 13 | HeaterFbRemainMs | Time left to the heater feedback fault. |
| 14 | CoolerFbRemainMs | Time left to the cooler feedback fault. |
| 15 | Temp1OorAccumMs | Sensor 1 out-of-range accumulator; it fails at ErrorTimeout. |
| 16 | Temp2OorAccumMs | Sensor 2 accumulator. |
| 17 | Temp1OorEventsPerHour | Sensor 1 out-of-range events in the last 60 minutes. |
| 18 | Temp2OorEventsPerHour | Sensor 2 events in the last 60 minutes. |
| 19 | StatusMirror | Status from the last CheckTemp. |
| 20 | WarningMirror | Warning from the last CheckTemp. |
| 21 | doHeaterMirror | Last heater command. |
| 22 | doCoolerMirror | Last cooler command. |
| 23 | AppliedFilterPoints | Averaging length actually in use. |
| 24 | ZoneInitialized | 1 once a setup has been loaded. |

## 8. Known limits

- A relay that is physically stuck is not detected, because the feedback reads the output state, not the contact.
- A heater or cooler that runs without reaching the setpoint runs indefinitely; there is no time limit on a relay.
- After one sensor has failed, a wrong reading from the survivor that stays inside the limits is not detected.
- Only the highest-priority warning is visible at a time.
- The sensor limits serve as both the valid-signal range and the trip limits; there is no separate process over-temperature limit.
