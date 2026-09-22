# TempCtl v4.0.0 - Capability and Operating Rules

Temperature controller library (.dll for Windows test, .so for the cRIO,
the myRIO and the Raspberry Pi). This document describes what the controller
does and the rules it follows; `TEMPCTL-SPEC-v4.0.0.md` has the numbered
requirements, `TEMPCTL_PACKAGE_GUIDE.md` the API tables and
`TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md` the integration reference.

## 1. What it does

TempCtl is signals in, signals out. It owns no hardware and no timing. The
host application reads and scales the temperatures, decides whether its own
operating conditions allow control and passes that as one **run
permissive**, calls the controller once per zone per loop pass (nominally
every 100 ms), and drives the relays from the commands it returns.

Per zone it provides:

- Deadband heating and cooling control of one heater and one cooler relay.
- An explicit lifecycle: Start, Stop, and a live run permissive that stops
  control at once when the host's conditions are lost.
- Validity checking of one or two temperature sensors against a high and low limit.
- Fallback to the second sensor, and a cross-check between the two.
- A check that the digital outputs followed the commands.
- A status code, a warning code and a diagnostics array of 28 values.

Sixteen independent zones (0 to 15) share nothing. Faults are shutdown
events: the host shuts the stand down on any controller fault. Warnings are
information only and never change control. The stopped, blocked, pending
and tripped states are neither faults nor warnings: they are the normal
lifecycle.

## 2. Calls and setup

Initialize loads the whole setup and leaves the zone stopped. Start begins
control. CheckTemp passes only the live signals: the temperatures, the
relay feedback and the run permissive. Stop ends control without a fault.
Changing any parameter means Stop, then Initialize with the full setup,
then Start again.

| Call | Arguments | Use |
|---|---|---|
| `TcInit` | zone, nowMs, setupArray[18], setupLen, status, warning | Loads the setup. Leaves the zone `IdleStopped`. Call once per zone at startup, and (after Stop) for any parameter change. |
| `TcStart` | zone, nowMs, runPermissive, status, warning | Starts control when the zone is enabled, not faulted and the permissive is true. Otherwise refused, no error: `IdleStartBlocked`. |
| `TcStop` | zone, nowMs, doHeater, doCooler, status, warning | Stops control: both commands 0 returned at once, no fault. Keeps the setup, sensor history and any fault. |
| `TcCheckTemp` | zone, nowMs, temp1, temp2, diHeaterFB, diCoolerFB, runPermissive, doHeater, doCooler, status, warning | One control step. Call once per zone per loop pass whether started or not. |
| `TcReset` | zone, nowMs, status, warning | Clears faults, warnings, history and timers; keeps the setup; relays off; leaves the zone `IdleStopped`. |
| `TcGetDiag` | zone, diagArray[28], diagLen | Reads the diagnostics. Read-only; changes nothing. |
| `TcVersion` | - | Library version, 0x040000 for 4.0.0. |
| `TcSetupCount` | - | Setup array length (18). |
| `TcDiagCount` | - | Diagnostics array length (28). |

Every stateful call takes the current millisecond tick, which the
controller uses for all its timers. A call returns 0 when it ran, or a
negative code for a bad argument (wrong zone, null pointer, wrong array
length), in which case nothing runs and no output changes. A refused Start
returns 0: the status says why.

Initialize and Reset return no relay commands. They clear the library's
commands, but they cannot take back a command the host already put on a
physical output. So, on a zone that may be running, always call Stop first,
apply the two zeros it returns to the outputs, and then Initialize or Reset.
A faulted zone already returned zeros when it faulted; Reset may follow
directly.

## 3. Operating rules

No relay ever turns on, or switches from heating to cooling, because of a
single reading. Every such transition waits out a timeout. This protects
the relays and the heating and cooling equipment. The one deliberate
exception is turning relays **off**: a Stop, and a lost run permissive, turn
both relays off on the very reading that observes them. Turning off early
cannot chatter, because nothing turns back on by itself; only a new Start
does.

### 3.1 Enable, Initialize, Start, Stop and Reset

- A zone is disabled until a setup with `TempCtrlEnable = 1` is loaded. While disabled the controller takes no action at all: relays off, no checks, no timers, no faults, and a Start is refused. The enable permits a Start; it does not start anything.
- Initialize is allowed at any time, including on a faulted zone. It checks and stores the setup, clears faults, history and timers, and leaves the zone stopped with both relays off. It never keeps a relay running from before.
- Start runs control when the zone is enabled, not faulted and the run permissive is true. With the permissive false the Start is refused: the zone reports `IdleStartBlocked` with the warning `OperatingConditionNotMet`, nothing counts down, and nothing starts by itself when the permissive returns. A new Start is needed. A Start on a zone that is already running changes nothing.
- Stop turns both relay commands off at once and reports `IdleStopped`. It is not a fault and clears nothing that matters: the setup, a failed sensor, the sensor history and any latched fault stay. If the zone had stopped because the permissive was lost, Stop keeps that reason (`IdleOperatingConditionTripped`) instead of erasing it.
- Reset keeps the setup but clears faults, warnings, history, timers and the averages, turns both relays off and leaves the zone stopped. It does not clear a configuration fault. Control resumes only after Start.
- Initialize checks the setup: the units, the deadbands (not negative and not both zero), the setpoint and both band edges inside the sensor limits, and every timeout above zero with no NaN, the operating-condition timeout included. While enabled, a failed check is a configuration fault; while disabled it is only a warning.

### 3.2 The run permissive

- The host combines its own operating conditions (pumps, flows, doors, modes, whatever the plant needs) into one true/false value and passes it to every CheckTemp. The controller knows nothing about what is behind it.
- While running, the first reading with the permissive false turns both relays off, clears the Started flag and starts the `OperatingConditionTimeout` countdown: the status is `OperatingConditionPending`, the warning `OperatingConditionNotMet`. Nothing else is evaluated while pending.
- If the permissive comes back before the timeout, the countdown is cancelled and the zone waits as `IdleOperatingConditionTripped`: no fault, no restart. The reason stays visible until a new Start, Reset or Initialize. The warning clears as soon as the permissive is true again; the status does not.
- If the permissive stays false for the whole timeout, the zone faults: `OperatingConditionFault`, latched like every other fault.
- A false permissive while the zone is stopped, blocked or tripped never starts a countdown and never faults: it only refuses a Start and shows the warning.
- If a sensor, disagreement or feedback fault matures on the same reading the permissive is lost, that fault wins and is what the status shows.

### 3.3 Signal high and low checks

- `HiLimit` and `LoLimit` define a valid sensor reading. A reading outside them, including NaN or infinity, is out of range.
- While running, out-of-range time accumulates; in-range time drains it back at half the rate. When the accumulated time reaches `ErrorTimeout` the sensor has failed. A sensor that is out of range more than a third of the time therefore still fails (half the time: after about four `ErrorTimeout`s), while an isolated glitch drains away.
- `ErrorTimeout` must be at least two loop periods (200 ms at a 100 ms loop). A shorter value lets a single out-of-range reading fail the sensor from one sample.
- While the sensor in control is out of range, the deadband and at-setpoint checks pause: the relays hold and their timers freeze until the reading returns or the sensor fails.
- A failed sensor is never returned to service. Only Reset or Initialize restores it; Stop and Start do not.
- With one sensor fitted, a failure is a fault. With two fitted, a failure with a healthy partner is only a warning, and the fault comes when no healthy sensor is left.
- While the zone is stopped nothing is checked: the raw readings are still shown in the diagnostics, but out-of-range time neither accumulates nor drains, and a sensor that goes bad while stopped is found after the next Start.

### 3.4 Deadband checks

- The deadbands set the switching points: `HiBand` above the setpoint, `LoBand` below it.
- While started: below `LoBand` continuously for `DeadbandTimeout` turns heating on; above `HiBand` for the same time turns cooling on. Returning inside the band clears the timer, and crossing to the other side restarts it.
- Heating and cooling are mutually exclusive, and the deadband timer is idle while a relay is on.
- Right after a Start both relays stay off until the first CheckTemp has run its countdowns; the status shows `TempAtSetPt` until then.

### 3.5 At-setpoint check

- A running relay turns off only after the temperature has held at the setpoint for `AtSetPtTimeout`: at or above it while heating, at or below it while cooling.
- Falling back across the setpoint restarts the timer, so the relay keeps running.
- Completing this timer also sets `Initial_HC_Flag`, which marks the end of the first heat-up or cool-down. A zone that starts inside the deadband sets the flag on its first reading. Start clears the flag, so every run has its own first cycle.

### 3.6 Two sensors

- Sensor 1 is the primary. Sensor 2 is optional and may sit in a different location, so `Temp2Offset` is added to its reading before every use.
- If the sensor in control fails and the other is healthy, control moves to it and the stand keeps running, with the warning `RunningOnTemp2`. Control never moves back on its own; the warning survives Stop and Start until Reset or Initialize.
- Both sensors are averaged over `FilterPoints` readings, and the averages are compared. Averaging is used only for this comparison; the limit checks and control use the raw readings. Start clears the averages, so stale readings from before a stop are never compared.
- A difference larger than `Temp2Tolerance` raises a warning after a tenth of `TempCompareTimeout`, and a fault after the full time. Agreement clears the warning at once and restarts the timer.
- The comparison is inactive until `Initial_HC_Flag` is set, while either sensor is out of range, once either sensor has failed, and while the zone is not running. Nothing decides which sensor is right, so a lasting disagreement always faults.

### 3.7 Relay output feedback

- The feedback inputs read back the digital output state, not the physical relay contact.
- While running, each feedback is compared with that relay command from the previous step. A mismatch raises a warning at once and starts the timer; a match clears it. A mismatch lasting `RelayFeedbackTimeout` faults that relay.
- The heater and cooler are checked independently. `FeedbackEnable` turns both checks off. Set `RelayFeedbackTimeout` longer than the delay through the output loop.
- Turning the relays off by Stop, by a lost permissive, by Initialize or by Reset is never a mismatch. A relay that is still physically closed when control starts again is caught on the first reading after Start.

### 3.8 What happens on a fault

Both relay commands go to 0, the Started flag clears, the first fault is
latched in the status, the warning code freezes, and every check and timer
stops. Start and Stop on a faulted zone change nothing. The zone stays there
until Reset or Initialize. Freezing keeps the conditions at the moment of
the fault visible for troubleshooting.

## 4. Status codes

| Code | Name | Type | Meaning |
|---|---|---|---|
| 0 | TempCtrlDisabled | State | Not enabled, or no setup loaded. Relays off, nothing evaluated, Start refused. |
| 1 | TempAtSetPt | Running | Relays off and the temperature is inside the deadband (also right after a Start, until the first reading). |
| 2 | HeaterON | Running | Heating commanded. |
| 3 | CoolerON | Running | Cooling commanded. |
| 4 | HeatPending | Running | Below the band, waiting out the deadband timeout. |
| 5 | CoolPending | Running | Above the band, waiting out the deadband timeout. |
| 6 | IdleStopped | Stopped | Configured and enabled, not started: after Initialize, Reset or Stop. Waiting for Start. |
| 7 | IdleStartBlocked | Stopped | The last Start was refused because the run permissive was false. Waiting for a new Start. |
| 8 | OperatingConditionPending | Stopped | The permissive was lost while running: relays off, the fault countdown is running. |
| 9 | IdleOperatingConditionTripped | Stopped | The permissive loss ended (it recovered, or the operator stopped) before the timeout. The reason stays until a new Start. |
| 10 | Temp1FailHigh | Fault | One sensor fitted: sensor 1 above HiLimit past the timeout. |
| 11 | Temp1FailLow | Fault | One sensor fitted: sensor 1 below LoLimit past the timeout. |
| 12 | BothSensorsFailed | Fault | Two sensors fitted: no healthy sensor is left. |
| 13 | TempDisagreeFault | Fault | The sensors disagreed for TempCompareTimeout. |
| 14 | ConfigFault | Fault | The setup check failed while enabled. Only a valid Initialize clears it. |
| 15 | HeaterFBFault | Fault | Heater DO feedback mismatched past the timeout. |
| 16 | CoolerFBFault | Fault | Cooler DO feedback mismatched past the timeout. |
| 17 | OperatingConditionFault | Fault | The permissive stayed false for OperatingConditionTimeout after being lost while running. |
| 18+ | reserved | - | Future codes. |

Every code from 10 up is a fault. The status is the authoritative
indication of the controller's state and of why it stopped.

## 5. Warning codes

One warning is reported at a time, the lowest code active. Warnings do not change control.

| Code | Name | Sets when | Clears when |
|---|---|---|---|
| 0 | NoWarning | - | - |
| 1 | Temp1OutOfRange | Sensor 1 is outside its limits now (while running). | It returns in range, or the zone stops. |
| 2 | Temp2OutOfRange | Corrected sensor 2 is outside its limits now (while running). | It returns in range, or the zone stops. |
| 3 | HeaterFBMismatch | Heater feedback does not match the command. | They match, or the zone stops. |
| 4 | CoolerFBMismatch | Cooler feedback does not match the command. | They match, or the zone stops. |
| 5 | TempDisagree | The sensors have disagreed for a tenth of TempCompareTimeout. | They agree, or the check pauses. |
| 6 | RunningOnTemp2 | Sensor 1 failed and control moved to sensor 2. | Reset or Initialize (not Stop or Start). |
| 7 | ConfigInvalid | The setup check failed while the zone was disabled. | The next valid Initialize. |
| 8 | OperatingConditionNotMet | A Start was refused by a false permissive, or the permissive is false while the zone is pending or tripped. | The permissive is true again, Stop acknowledges a blocked Start, or a Start succeeds. Frozen by OperatingConditionFault. |

## 6. Setup parameters

One array of doubles, in this order. Each value is read as its own type:
timeouts are whole milliseconds above zero, switches are 0 or 1,
temperatures are in the unit chosen by `TempUnits`. Values 0 to 16 are the
v3 setup unchanged; 17 is new.

| # | Parameter | Unit | Meaning |
|---|---|---|---|
| 0 | TempCtrlEnable | 0 / 1 | Master enable for the zone. 0 = the controller takes no action and refuses Start. |
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
| 17 | OperatingConditionTimeout | ms | How long a lost run permissive may stay false, after stopping control, before it becomes a fault. Longer than the longest expected transient and at least two loop periods. It never delays turning the relays off. |

## 7. Diagnostics

One array of doubles, read with `TcGetDiag` for display, logging or CAN.
Reading it changes nothing. Values 0 to 24 are the v3 diagnostics unchanged;
25 to 27 are new.

| # | Value | Meaning |
|---|---|---|
| 0 | ControlTemp | The value control acts on. |
| 1 | ActiveSensor | 1 or 2. |
| 2 | Temp1Raw | Last raw sensor 1 reading (shown while stopped as well). |
| 3 | Temp2Raw | Last raw sensor 2 reading. |
| 4 | Temp2Corrected | Sensor 2 plus its offset. |
| 5 | Temp1Avg | Sensor 1 average (comparison only). |
| 6 | Temp2Avg | Corrected sensor 2 average. |
| 7 | HiBand | Upper switching point. |
| 8 | LoBand | Lower switching point. |
| 9 | Initial_HC_Flag | 1 once the first heat or cool cycle of this run has reached the setpoint. |
| 10 | DeadbandRemainMs | Time left before a relay turns on. |
| 11 | AtSetPtRemainMs | Time left before the running relay turns off. |
| 12 | CompareRemainMs | Time left to the disagreement fault. |
| 13 | HeaterFbRemainMs | Time left to the heater feedback fault. |
| 14 | CoolerFbRemainMs | Time left to the cooler feedback fault. |
| 15 | Temp1OorAccumMs | Sensor 1 out-of-range accumulator; it fails at ErrorTimeout. |
| 16 | Temp2OorAccumMs | Sensor 2 accumulator. |
| 17 | Temp1OorEventsPerHour | Sensor 1 out-of-range events in the last 60 minutes. |
| 18 | Temp2OorEventsPerHour | Sensor 2 events in the last 60 minutes. |
| 19 | StatusMirror | Status of the last Initialize, Start, Stop, CheckTemp or Reset. |
| 20 | WarningMirror | Warning of that call. |
| 21 | doHeaterMirror | The heater command after that call. |
| 22 | doCoolerMirror | The cooler command after that call. |
| 23 | AppliedFilterPoints | Averaging length actually in use. |
| 24 | ZoneInitialized | 1 once a setup has been loaded. |
| 25 | RunPermissive | The last permissive the controller evaluated (0 or 1); not available after Initialize or Reset until a Start or CheckTemp. |
| 26 | OperatingConditionRemainMs | Time left before a lost permissive becomes a fault; 0 when not pending. |
| 27 | ControllerStarted | 1 while a Start is accepted and control may run. |

On the CAN message these 28 values occupy 44 bytes; the millisecond values
are sent as 16-bit numbers that saturate at 64255 on the wire, while the
diagnostics array itself keeps the full value.

## 8. Known limits

- A relay that is physically stuck is not detected, because the feedback reads the output state, not the contact.
- A heater or cooler that runs without reaching the setpoint runs indefinitely; there is no time limit on a relay.
- After one sensor has failed, a wrong reading from the survivor that stays inside the limits is not detected.
- Only the highest-priority warning is visible at a time.
- The sensor limits serve as both the valid-signal range and the trip limits; there is no separate process over-temperature limit.
- Nothing is checked while a zone is stopped, so a sensor that goes bad while stopped is found after the next Start.
- No wall-clock time is recorded for a fault or a stop; the host adds its own timestamp when it sees the status change.
- The controller cannot act if the host stops calling it. Watchdogs and fail-safe outputs remain the host's and the I/O layer's responsibility.
