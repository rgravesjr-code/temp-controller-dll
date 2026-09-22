# TempCtl v4.0.0 — Requirements Change Handoff

**Baseline:** TempCtl v3.0.0, including Amendment A and the behavior documented by `TEMPCTL-SPEC-v3.0.0.md`.

**Target:** TempCtl v4.0.0. This is a major version because it adds exports, changes the `TcCheckTemp` signature, extends the setup and diagnostics arrays, and changes Init/Reset lifecycle behavior.

**Audience:** the implementation project responsible for the TempCtl C source, Windows DLLs, NI Linux Real-Time shared libraries, tests, TempSim, package generation, and release documentation.

**Decision date:** 2026-09-22.

**Status:** approved requirements handoff. The behaviors below were reviewed with the system owner. Implement this document as a coherent release; do not deliver isolated parts of the change under a v3.x version.

---

## 1. Background and intent

TempCtl v3.0.0 uses `TempCtrlEnable` as its only lifecycle control. A valid enabled setup begins controlling as soon as `TcCheckTemp` is called. Reusing `TcInit` to start and stop is undesirable because Init also validates and reloads configuration, resets runtime history, and may preserve running relay commands under the v3 rules.

The host application needs three separate concepts:

1. **Master enable:** `TempCtrlEnable` remains the configuration-level permission for a zone. When false, the controller is inert and cannot be started.
2. **Operator/application Start and Stop:** normal lifecycle commands that do not create a fault and do not require configuration to be reloaded.
3. **Run permissive:** a live, generic operating-condition input supplied by the host. The host remains responsible for combining its end-use-specific conditions. TempCtl provides a backup inhibit, clear feedback, and delayed escalation to a fault.

The design must remain generic. TempCtl must not acquire plant-specific knowledge such as pump names, flow limits, door interlocks, process modes, or stand sequencing. The host combines those conditions into one `runPermissive` value and remains the primary decision-maker.

The reasons for this change are:

- prevent overloading `TempCtrlEnable` and `TcInit` with normal run control;
- guarantee a direct, non-faulting Stop that commands both outputs off;
- block Start when the host's operating conditions are not satisfied;
- stop safely if the permissive is lost during operation;
- retain an unambiguous reason after a momentary condition recovers;
- avoid escalating a momentary permissive loss into a latched fault;
- preserve the existing priority of configuration, sensor, disagreement, and relay-feedback faults;
- keep the C ABI simple enough for LabVIEW's Import Shared Library wizard on Windows and NI Linux Real-Time.

Capturing wall-clock or calendar time for a fault is explicitly deferred. `nowMs` remains the wrapping LabVIEW Tick Count used only for elapsed-time behavior. The host may add its own wall-clock timestamp when it observes a status transition.

---

## 2. Scope

This release shall include:

- a v4 controller implementation and public header;
- new `TcStart` and `TcStop` exports;
- a live `runPermissive` input on `TcCheckTemp`;
- an operating-condition timeout appended to the setup array;
- lifecycle/permissive fields appended to diagnostics;
- four new non-fault statuses, one new fault status, and one new warning;
- revised Init and Reset behavior so neither implicitly starts control;
- Windows x86 and x64 DLL builds;
- NI Linux Real-Time x86-64 and 32-bit ARM builds;
- continued Linux ARM64 build support unless the release owner explicitly removes it;
- the same LabVIEW-importable `.h` file inside every binary package;
- updated unit, integration, packaging, DBC/ECD/CanTp, simulator, and target tests;
- regenerated v4 `tempctl.dbc` and `tempctl.ecd` files using compact U16 millisecond diagnostics;
- an updated TempSim application and CLI scenarios;
- a standalone v4 reference/integration document for users of the DLL and `.so`;
- native and LabVIEW integration testing on the Intel x86-64 cRIO and NI myRIO-1900.

This release shall not:

- add plant-specific operating-condition inputs or logic;
- add a wall-clock timestamp to the API or diagnostics;
- automatically restart after a Stop, blocked Start, or permissive trip;
- change the meaning of existing temperature, sensor, comparison, or relay-feedback rules except where lifecycle gating requires it;
- convert the single warning output to a bitmask;
- add heap allocation, file I/O, console I/O, blocking calls, hardware access, or CAN transport to TempCtl.

If individual operating-condition reason bits are later required, they need a separate requirements decision. v4.0.0 accepts only the combined generic permissive.

---

## 3. Version and compatibility

- `TC_VERSION_MAJOR = 4`, `TC_VERSION_MINOR = 0`, `TC_VERSION_PATCH = 0`.
- `TcVersion()` shall return `0x040000`.
- `TcSetupCount()` shall return `18`.
- `TcDiagCount()` shall return `28`.
- The v4 public header and v4 binary are an inseparable pair.
- v3 wrapper VIs must not be used with a v4 binary because `TcCheckTemp` and both array contracts change.
- All generated LabVIEW wrapper VIs must be regenerated or deliberately updated and reverified against the v4 header.
- Package names, file-version metadata, README files, manifests, dependency reports, test logs, DBC artifacts, and simulator compatibility declarations must identify v4.0.0.

The implementation may preserve v3 exports only if doing so is explicitly requested later. Compatibility shims are not required by this handoff and should not be added merely for convenience; they increase the surface and test burden before a multi-week release interval.

---

## 4. Exported API

All exports use C linkage and the C calling convention (`cdecl`). Exported signatures use only `int32_t`, `uint32_t`, `double`, and pointers to those types.

```c
int32_t TcVersion(void);       /* 0x040000 */
int32_t TcSetupCount(void);    /* 18 */
int32_t TcDiagCount(void);     /* 28 */

int32_t TcInit(int32_t zone, uint32_t nowMs,
               const double *setupArray, int32_t setupLen,
               int32_t *status, int32_t *warning);

int32_t TcStart(int32_t zone, uint32_t nowMs,
                int32_t runPermissive,
                int32_t *status, int32_t *warning);

int32_t TcStop(int32_t zone, uint32_t nowMs,
               int32_t *doHeater, int32_t *doCooler,
               int32_t *status, int32_t *warning);

int32_t TcCheckTemp(int32_t zone, uint32_t nowMs,
                    double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB,
                    int32_t runPermissive,
                    int32_t *doHeater, int32_t *doCooler,
                    int32_t *status, int32_t *warning);

int32_t TcReset(int32_t zone, uint32_t nowMs,
                int32_t *status, int32_t *warning);

int32_t TcGetDiag(int32_t zone, double *diagArray, int32_t diagLen);
```

### 4.1 Return codes

Keep the v3 call-result contract:

| Value | Name | Meaning |
|---:|---|---|
| 0 | `TC_OK` | The call was valid and executed. A blocked Start still returns `TC_OK`; status and warning explain the result. |
| -1 | `TC_ERR_ARG` | Required pointer is null or an array length is invalid. |
| -2 | `TC_ERR_ZONE` | Zone is outside 0..15. |

On a negative return, no state advances and no output pointer is written.

### 4.2 Boolean conversion

The live `int32_t runPermissive` input shall use zero = false and nonzero = true. This follows the existing relay-feedback integer convention and avoids adding a LabVIEW-specific Boolean ABI.

### 4.3 Normal call pattern

```text
startup/configuration:  TcInit(... setup[18] ...)       -> configured but stopped
start request:          TcStart(... runPermissive ...)  -> accepted or blocked
each control tick:      TcCheckTemp(... runPermissive ...)
normal stop:            TcStop(...)                     -> both commands returned as zero
fault reset:            TcReset(...)                    -> fault cleared, controller stopped
display/logging:        TcGetDiag(... diag[28] ...)      -> read-only

active reconfiguration: TcStop(...) -> apply zero DOs -> TcInit(...)
active non-fault reset: TcStop(...) -> apply zero DOs -> TcReset(...)
```

Every call for the same zone, including the read-only `TcGetDiag`, must remain serialized by the host. Different zones may be processed independently. The library remains lock-free and not thread-safe for concurrent calls to the same zone.

---

## 5. Setup array — `TcSetupCount() = 18`

Indexes 0 through 16 retain their v3 meanings and numeric order. Append exactly one field:

| Index | Define | Name | Type represented as DBL | Unit | Validation |
|---:|---|---|---|---|---|
| 17 | `TC_SETUP_OPERATING_CONDITION_TIMEOUT` | `OperatingConditionTimeout` | `uint32_t` | ms | Whole milliseconds after truncation; must be >= 1 when `TempCtrlEnable = 1`. Invalid enabled setup produces `ConfigFault`; invalid disabled setup produces `ConfigInvalid`, consistent with existing config rules. |

Purpose: the timeout controls when a run-permissive loss that occurred during active control is promoted from a non-fault trip to `OperatingConditionFault`.

The timeout does **not** delay de-energizing outputs. It delays fault classification only.

Configuration guidance shall recommend a timeout longer than the longest expected transient and normally at least two host loop periods. The standard countdown one-tick grace applies: the first false observation starts at the full remaining time and expiration may occur only on a later false tick.

No field may be inserted into the middle of the existing setup array.

---

## 6. Diagnostics array — `TcDiagCount() = 28`

Indexes 0 through 24 retain their v3 meanings and numeric order. Append:

| Index | Define | Name | Meaning |
|---:|---|---|---|
| 25 | `TC_DIAG_RUN_PERMISSIVE` | `RunPermissive` | Most recent permissive actually evaluated by an enabled, non-faulted `TcStart` or `TcCheckTemp`, represented as 0.0/1.0. Report NaN after Init/Reset until such an evaluation. Disabled/faulted calls and an idempotent Start while already active do not update it. Stop leaves the last evaluated value unchanged. |
| 26 | `TC_DIAG_OPERATING_CONDITION_REMAIN_MS` | `OperatingConditionRemainMs` | Remaining milliseconds before a pending permissive trip becomes `OperatingConditionFault`; 0 when not pending. |
| 27 | `TC_DIAG_CONTROLLER_STARTED` | `ControllerStarted` | 1 when Start has been accepted and control is eligible to run; 0 while disabled, stopped, blocked, tripped, or faulted. |

`TcGetDiag` remains read-only. Calling it must not advance the permissive countdown, change a warning, clear a latched status, or modify commands.

For v4, the existing diagnostic mirrors have these expanded meanings:

- `StatusMirror` and `WarningMirror` reflect the results of the most recent successful stateful call: Init, Start, Stop, CheckTemp, or Reset;
- `doHeaterMirror` and `doCoolerMirror` reflect the internal commands after that call, including the internal zeros established by Init or Reset even though those two APIs do not return DO values;
- `TcGetDiag` only reads the mirrors and does not become the call represented by them.

Wall-clock fault time is not added in v4.0.0.

Because diagnostics are encoded into the TempCtl DBC/CanTp message, the database generator and every generated table must be updated for the 28-field layout. The DBC, ECD, and flat tables must remain generated artifacts with one authoritative layout definition.

### 6.1 CAN database wire representation

The C API and controller timing remain full-width: setup values are still supplied as DBL values representing whole milliseconds, internal timers remain `uint32_t`, `nowMs` remains `uint32_t`, and `TcGetDiag` still returns DBL values. The following change applies only to their serialized representation in `tempctl.dbc`, `tempctl.ecd`, and generated CanTp tables.

Encode every millisecond diagnostic as an unsigned 16-bit signal, factor 1 ms/bit and offset 0:

- `DeadbandRemainMs`;
- `AtSetPtRemainMs`;
- `CompareRemainMs`;
- `HeaterFbRemainMs`;
- `CoolerFbRemainMs`;
- `Temp1OorAccumMs`;
- `Temp2OorAccumMs`;
- new `OperatingConditionRemainMs`.

Use 0 through 64,255 ms (`0x0000` through `0xFAFF`) as the valid physical range and `0xFFFF` as J1939 Not Available. A finite diagnostic value above 64,255 ms shall saturate to 64,255 in the CAN representation; this saturation must not alter the controller's internal timer or the DBL value returned by `TcGetDiag`. NaN shall pack as Not Available. Tests shall cover 0, 1, 64,255, 64,256, a larger `uint32_t` value, and NaN.

Retain J1939 PGN 65280 (`0xFF00`) and CAN identifier `0x18FF00FE`. The v4 signal layout and payload replace the v3 layout: the timer-width reduction changes the bit positions of later signals, so v3 DBC/ECD/CanTp definitions must not be used to decode a v4 payload. The migration guide shall state this wire incompatibility explicitly. Under the specified sequential, byte-aligned layout, the expected v4 payload is 44 bytes; the generator shall calculate and assert the length rather than relying only on this documented value.

Generate both `tempctl.dbc` and `tempctl.ecd` from the same authoritative signal model. Do not hand-edit the binary ECD. After generation, decode/flatten `tempctl.ecd` and compare every channel by name against the DBC for message ID, PGN, payload length, start bit, bit length, signedness, byte order, factor, offset, limits, units, and lookup values.

The existing ECD conversion path may reorder channels alphabetically. Directly wiring the 28-element `diagArray` into CanTp is allowed only when ECD channel order exactly matches `TC_DIAG_*` index order. If the generated ECD reorders channels, generate and ship an explicit, tested diagnostic-index-to-ECD-channel reorder table; silent reliance on channel order is a release failure.

---

## 7. Status codes

Existing codes 0 through 5 and 10 through 16 retain their current numeric values and meanings. Assign the reserved state codes and the next fault code as follows:

| Code | Define | Name | Meaning |
|---:|---|---|---|
| 0 | `TC_ST_TEMP_CTRL_DISABLED` | `TempCtrlDisabled` | No setup or master enable false. Inert; outputs off. |
| 1 | `TC_ST_TEMP_AT_SETPT` | `TempAtSetPt` | Existing active-control state. |
| 2 | `TC_ST_HEATER_ON` | `HeaterOn` | Existing active-control state. |
| 3 | `TC_ST_COOLER_ON` | `CoolerOn` | Existing active-control state. |
| 4 | `TC_ST_HEAT_PENDING` | `HeatPending` | Existing active-control state. |
| 5 | `TC_ST_COOL_PENDING` | `CoolPending` | Existing active-control state. |
| 6 | `TC_ST_IDLE_STOPPED` | `IdleStopped` | Valid enabled setup, but Start has not been accepted or a normal Stop/Reset/Init left the controller idle. |
| 7 | `TC_ST_IDLE_START_BLOCKED` | `IdleStartBlocked` | A Start was attempted while `runPermissive = 0`. No fault countdown runs. |
| 8 | `TC_ST_OPERATING_CONDITION_PENDING` | `OperatingConditionPending` | Permissive was lost during active control; outputs are off and the fault countdown is running. |
| 9 | `TC_ST_IDLE_OPERATING_CONDITION_TRIPPED` | `IdleOperatingConditionTripped` | A permissive loss stopped control but recovered or was manually stopped before the timeout. The reason remains latched; a new Start is required. |
| 10–16 | existing defines | Existing faults | Unchanged. |
| 17 | `TC_ST_OPERATING_CONDITION_FAULT` | `OperatingConditionFault` | Permissive remained false through `OperatingConditionTimeout` after being lost during active control. Latched fault. |

Every status >= `TC_ST_FAULT_FIRST` (10) remains a fault.

### 7.1 Status authority

Status is the authoritative indication of controller state and the retained cause of a stop. The new warning is secondary and may be hidden by a lower-numbered warning under the existing single-warning priority rule.

### 7.2 Status precedence

From highest to lowest authority:

1. invalid API call: return a negative call result and write nothing;
2. no setup or `TempCtrlEnable = 0`: `TempCtrlDisabled`;
3. a previously latched fault: preserve it and its frozen warning;
4. an existing controller fault maturing on the current active tick, using the existing order;
5. `OperatingConditionFault`;
6. operating-condition pending/tripped/Start-blocked states;
7. `IdleStopped`;
8. normal active-control state.

On the first tick that both an existing controller fault matures and `runPermissive` becomes false, the existing controller fault wins. Extend the existing same-tick fault order to:

```text
configuration -> sensor range -> sensor disagreement -> heater feedback
-> cooler feedback -> operating condition
```

The first latched fault continues to win permanently until an allowed Reset or Init.

---

## 8. Warning codes

Existing warning codes 0 through 7 retain their values and behavior. Add:

| Code | Define | Name | Sets | Clears |
|---:|---|---|---|---|
| 8 | `TC_WN_OPERATING_CONDITION_NOT_MET` | `OperatingConditionNotMet` | A Start is blocked by false permissive, or the permissive is false during an operating-condition pending/tripped state. | The live permissive becomes true, except that a latched `OperatingConditionFault` freezes the warning under the normal fault rule. |

The warning remains a single code and the lowest active numeric code wins. Therefore codes 1–7 may mask code 8. This is acceptable because statuses 7, 8, 9, and 17 preserve the stop/fault meaning.

Do not independently latch warning 8 after the permissive recovers. Latch the status/cause instead. This preserves the existing distinction:

- warnings normally describe a current condition;
- status describes the controller's state and retained stop/fault cause;
- warnings freeze only when a fault latches, except for pre-existing documented persistent warnings such as `RunningOnTemp2`.

---

## 9. Lifecycle and permissive behavior

### R10.1 — Master enable

`TempCtrlEnable` remains the master configuration enable.

- When false or no setup exists, Start cannot succeed.
- `TcCheckTemp` remains inert: commands zero, no sensor/permissive/control checks, status `TempCtrlDisabled`.
- A Start attempt returns `TC_OK` with `TempCtrlDisabled`; it is not an argument error.
- Existing disabled-config behavior (`ConfigInvalid`) remains intact.

### R10.2 — Init

A passing enabled `TcInit` shall:

- validate and store all 18 setup values;
- clear faults and runtime history using the existing Init rules;
- clear the Started flag;
- set both internal relay commands and previous commands to zero;
- set the time reference to `nowMs`;
- return `IdleStopped` and the appropriate warning.

v3's relay-preservation behavior across re-Init is removed. Init never keeps a heater or cooler command and never starts control. This is intentional and conservative.

`TcInit` does not have `doHeater` or `doCooler` output parameters. It can clear the library's internal commands, but it cannot retract a command that the host already delivered to physical I/O. Therefore, whenever a zone may be active, the required host sequence is `TcStop` -> apply the returned zero DO commands -> `TcInit`. A direct Init remains valid for an uninitialized, disabled, faulted, or known-idle zone. The wrappers, reference guide, TempSim, and integration tests shall enforce and demonstrate this distinction.

A passing disabled Init returns `TempCtrlDisabled`. Config-fault behavior remains as defined by v3 except for the new timeout validation.

### R10.3 — Start

`TcStart` shall evaluate prerequisites in this order:

1. validate zone and required output pointers;
2. if no setup or master disabled, return `TC_OK` with `TempCtrlDisabled`;
3. if a fault is latched, preserve the fault and frozen warning;
4. if already started, return the current status and warning as an unconditional idempotent no-op; do not evaluate or record this Start call's permissive and do not reset timers, averages, commands, or time references;
5. if `OperatingConditionPending` and `runPermissive = 0`, preserve pending status, warning, full accumulated countdown state, and time reference; only `TcCheckTemp` advances the countdown;
6. if `IdleOperatingConditionTripped` and `runPermissive = 0`, preserve the stronger trip status and report `OperatingConditionNotMet` subject to warning priority rather than replacing the cause with `IdleStartBlocked`;
7. for any other stopped non-fault state with `runPermissive = 0`, leave commands zero, set Started = 0, set `IdleStartBlocked`, and report `OperatingConditionNotMet` subject to warning priority;
8. otherwise accept Start. A true permissive supplied by an explicit Start may restart a pending or tripped controller.

An accepted Start shall:

- set Started = 1;
- record the successfully evaluated true permissive in `RunPermissive` diagnostics;
- keep both commands zero until the next `TcCheckTemp` makes a qualified decision;
- set `lastMs = nowMs`, so stopped time never advances a countdown;
- clear deadband, at-setpoint, comparison, relay-feedback, and operating-condition countdowns;
- clear moving-average sample buffers and `Initial_HC_Flag`, so stale samples from before a stop are not treated as current;
- preserve setup, latched sensor-failure/degraded state, out-of-range accumulators, and hourly health history; normal Stop/Start must not heal a failed or degrading sensor;
- clear a non-fault Start-blocked or operating-condition-trip status;
- clear warning 8 and recompute the returned warning from any remaining persistent condition; `RunningOnTemp2` remains active until Reset/Init under its existing rule;
- return `TempAtSetPt` provisionally until the first CheckTemp computes the actual active state, matching the existing pre-first-check convention.

There is no automatic retry. A previously blocked or tripped controller starts only from a new successful `TcStart` call.

Calling Start while already started is deliberately checked before `runPermissive`. `TcCheckTemp`, which returns the DO commands, remains the only call that can act on a newly false live permissive while control is active.

### R10.4 — Normal Stop

`TcStop` shall:

- validate zone and all four output pointers;
- command and return `doHeater = 0`, `doCooler = 0` immediately;
- set Started = 0 and update the previous-command mirrors to zero;
- cancel deadband, at-setpoint, comparison, and relay-feedback countdowns;
- set the time reference to `nowMs`;
- preserve configuration, sensor-failure state, out-of-range accumulators, hourly health history, and any latched fault;
- preserve an existing fault status and frozen warning;
- from an uninitialized zone, return commands zero, `TempCtrlDisabled`, and `NoWarning`;
- from an initialized disabled zone, return commands zero, `TempCtrlDisabled`, and `ConfigInvalid` only when that existing disabled-configuration warning is active;
- from `IdleStopped`, remain `IdleStopped`;
- from normal active operation, set `IdleStopped`;
- from `OperatingConditionPending` or `IdleOperatingConditionTripped`, cancel the permissive fault countdown and set/retain `IdleOperatingConditionTripped` so the reason is not erased;
- from `IdleStartBlocked`, treat explicit Stop as acknowledgment, set `IdleStopped`, and clear warning 8;
- leave `RunPermissive` diagnostic unchanged because Stop has no live permissive input.

Warning handling on a successful non-fault Stop shall be deterministic:

- a faulted zone preserves its frozen warning;
- Stop from pending/tripped retains warning 8 when the last evaluated permissive was false; a later tripped-state CheckTemp with true permissive or a successful Start clears it;
- ordinary Stop clears transient warnings 1 through 5 and 8;
- persistent `RunningOnTemp2` remains active under the existing v3 rule;
- `ConfigInvalid` remains applicable only to an initialized disabled zone.

Normal Stop never creates or clears a fault.

### R10.5 — Reset

Keep v3 Reset clearing behavior, including the special rule that Reset does not clear `ConfigFault`, with these lifecycle changes:

- Reset always sets Started = 0 and both internal commands to zero.
- A successful Reset leaves a valid enabled zone at `IdleStopped`; it does not resume control on the next CheckTemp without Start.
- Reset clears non-fault blocked/tripped lifecycle states.
- Because Reset has no live permissive argument, it does not claim Start is blocked. A later Start evaluates the current permissive.

Like Init, Reset has no DO output parameters. If Reset is invoked on a non-fault zone that may still be active, the host shall first call `TcStop` and apply its returned zero commands. A faulted zone already returned zero commands on the faulting CheckTemp; Reset may then be called directly. A direct Reset still clears the internal commands, but that internal change alone is not a substitute for delivering zero commands to physical I/O.

### R10.6 — CheckTemp while not started

When the zone is valid and enabled but Started = 0:

- always return both commands as zero;
- do not advance sensor, averaging, comparison, control, or relay-feedback logic;
- store the supplied permissive for diagnostics when not faulted;
- preserve `IdleStopped`, `IdleStartBlocked`, or `IdleOperatingConditionTripped` as applicable;
- update warning 8 from the current permissive only for the blocked/tripped states;
- do not start an operating-condition fault countdown merely because a stopped controller sees false permissive.

`OperatingConditionPending` is the one exception: its permissive countdown continues through `TcCheckTemp` calls even though Started was cleared when the trip occurred.

### R10.7 — Loss of permissive during active control

On an active CheckTemp tick:

1. perform existing sensor, comparison, and relay-feedback fault evaluation first so an existing fault that matures on the same tick retains priority;
2. if no fault won and `runPermissive = 0`, immediately command both outputs zero;
3. set Started = 0;
4. set `OperatingConditionPending`;
5. set warning 8 subject to normal warning priority;
6. start `OperatingConditionTimeout` with the full remaining value;
7. freeze/clear normal control and feedback countdowns so intentional de-energization cannot create a relay-feedback fault.

While pending:

- skip normal sensor, averaging, comparison, control, and feedback processing;
- false permissive advances only the operating-condition countdown;
- true permissive cancels the countdown and transitions to `IdleOperatingConditionTripped`;
- recovery never restarts control automatically;
- timeout expiry latches `OperatingConditionFault` and freezes the warning according to the normal fault rule.

The operating-condition countdown uses the same wrap-safe and backwards-time handling as all other countdowns.

### R10.8 — Deliberate safety exception to the one-sample rule

v3 states that a relay command never changes because of one sample. v4 narrows that rule:

- no relay may **energize**, reverse direction, or make a normal process-control transition because of one sample;
- a false run permissive may **de-energize** both outputs on its first observed sample;
- this cannot cause chatter because recovery does not re-energize anything; a new explicit Start is required;
- the timeout qualifies promotion to a latched fault, not the initial safe shutdown.

This exception is deliberate and must appear in the specification, capability document, package guide, and tests.

### R10.9 — Feedback after an intentional stop

Relay-feedback checking must not interpret the deliberate command-to-zero transition caused by Init, Stop, Reset, or permissive loss as a mismatch fault. Set previous command mirrors consistently and suppress feedback evaluation while disabled, stopped, pending, tripped, or faulted.

---

## 10. Required state-transition acceptance table

| From | Event | To | Outputs | Fault? | Restart rule |
|---|---|---|---|---|---|
| Uninitialized | Start | Disabled | Off | No | Init first |
| Disabled | Start | Disabled | Off | No | Enable through valid Init, then Start |
| IdleStopped | Start, permissive true | Active/provisional AtSetPt | Off until CheckTemp | No | Accepted |
| IdleStopped | Start, permissive false | IdleStartBlocked | Off | No | New Start after recovery |
| IdleStartBlocked | permissive recovers during CheckTemp | IdleStartBlocked | Off | No | New Start required |
| IdleStartBlocked | Stop | IdleStopped | Off | No | Start when ready |
| Active | Stop | IdleStopped | Off immediately | No | New Start required |
| Active | permissive false | OperatingConditionPending | Off immediately | No, timer starts | No auto-restart |
| Pending | permissive recovers before timeout | IdleOperatingConditionTripped | Off | No | New Start required |
| Pending | Start, permissive false | OperatingConditionPending | Off | No, timer preserved | Continue CheckTemp countdown or Stop |
| Pending | Start, permissive true | Active/provisional AtSetPt | Off until CheckTemp | No | Explicit restart accepted |
| Pending | Stop | IdleOperatingConditionTripped | Off | No | New Start required |
| Pending | permissive false through timeout | OperatingConditionFault | Off | Yes | Reset then Start |
| Condition-tripped idle | Start, permissive true | Active/provisional AtSetPt | Off until CheckTemp | No | Accepted; clears trip status |
| Condition-tripped idle | Start, permissive false | IdleOperatingConditionTripped | Off | No | Trip cause retained; Start when permissive is true |
| Any non-fault state | existing controller fault | Existing fault | Off | Yes | Existing reset/init rules, then Start |
| Any fault | Start or Stop | Same fault | Off | Remains | Reset/valid Init as applicable |
| Resettable fault | Reset | IdleStopped | Off | Cleared | Start required |
| ConfigFault | Reset | ConfigFault | Off | Remains | Passing Init, then Start |

---

## 11. Build and package requirements

### 11.1 Required controller targets

Build the same v4 source for:

| Package/target | Required binary | Architecture |
|---|---|---|
| Windows 64-bit test/integration | `tempctl.dll`, import library, `test_tempctl.exe` | PE x86-64 |
| Windows 32-bit LabVIEW 2026 | `tempctl.dll`, import library, `test_tempctl.exe` | PE x86 |
| Intel cRIO NI Linux RT | `libtempctl.so`, `test_tempctl` | ELF x86-64 |
| NI myRIO-1900 NI Linux RT | `libtempctl.so`, `test_tempctl` | ELF 32-bit ARM for the target's ARM Cortex-A9 userspace ABI; not ARM64 |
| Raspberry Pi / existing ARM64 support | `libtempctl.so`, `test_tempctl` | ELF AArch64 |

The myRIO-1900 uses a Xilinx Z-7010 with a dual-core ARM Cortex-A9 and ARM-based NI Linux Real-Time. Do not reuse the x86-64 cRIO or AArch64 Raspberry Pi binary. Determine the exact compiler target, floating-point ABI, dynamic loader, glibc compatibility, and NI toolchain/sysroot from the actual myRIO image before finalizing the build. Verify the result on the target with `file`, `readelf`, dependency inspection, and native execution.

Authoritative hardware references:

- NI myRIO-1900 User Guide and Specifications: https://download.ni.com/support/manuals/376047c.pdf
- NI myRIO overview identifying the dual-core ARM Cortex-A9: https://www.ni.com/en/support/documentation/supplemental/13/ni-myrio-frequently-asked-questions.html

### 11.2 Header in every package

Every binary package/directory shall contain its own copy of the same public `tempctl.h` adjacent to or clearly packaged with the binary:

- Windows x64 DLL package;
- Windows x86 DLL package;
- Linux x86-64 `.so` package;
- Linux 32-bit ARM/myRIO `.so` package;
- Linux ARM64 `.so` package.

All copies must be byte-identical and included in the release manifest. The header shall contain:

- v4 version and count constants;
- all nine exported prototypes;
- complete setup and diagnostic index defines;
- complete status, warning, and return-code defines;
- function comments describing input/output direction and array lengths;
- C linkage guards and platform export macros;
- only LabVIEW-importable signature types.

Also provide the header under `src/`. If a simplified importer-only header is shipped, it must be generated or contract-tested against the authoritative header and clearly named; silent divergence is a release failure.

### 11.3 Binary inspection

For every build, record in `DEPENDENCIES.txt` or an equivalent generated report:

- file format, architecture, and bitness;
- exported names;
- dependent libraries and required symbol versions;
- SONAME for each `.so`;
- confirmation that there is no unexpected C/C++ runtime dependency.

Generate SHA-256 hashes for every shipped file and fail packaging if any expected target, header, test executable, document, generated DBC, generated ECD, CanTp table, or ECD reorder table (when required) is absent.

### 11.4 Determinism

All controller exports remain bounded and free of heap allocation, I/O, blocking, callbacks, and hardware access. Static state remains isolated per zone. The library owns no thread, clock, CAN interface, or physical output.

---

## 12. LabVIEW import and wrapper acceptance

The v4 header must be accepted by the 32-bit LabVIEW 2026 Import Shared Library wizard against the Windows x86 DLL.

Document the verified parser configuration:

```text
Include paths, in order:
C:\Program Files (x86)\National Instruments\Shared\LVDB 2015\include\ansi
C:\Program Files (x86)\National Instruments\Shared\LVDB 2015\include

Preprocessor definition:
_WIN32
```

Expected node configuration:

- C calling convention;
- Run in any thread;
- generated wrapper VIs initially non-reentrant;
- every call for the same zone, including `TcGetDiag`, serialized by LabVIEW dataflow;
- `setupArray`: one-dimensional DBL Array Data Pointer, minimum size 18;
- `diagArray`: one-dimensional DBL Array Data Pointer, minimum size 28 and caller-allocated;
- scalar `int32_t *` outputs: signed 32-bit Pointer to Value;
- `runPermissive`: signed 32-bit value.

The import wizard's inability to distinguish `double *` scalar from an array remains expected; the reference document must explicitly instruct the user to verify/correct the two arrays.

Run wrapper smoke tests in this order: Version -> SetupCount -> DiagCount -> Init -> Start -> CheckTemp -> Stop -> Reset -> diagnostics.

Test the wrapper VIs against:

- the Windows x86 DLL under 32-bit LabVIEW 2026;
- the x86-64 `.so` under LabVIEW Real-Time on the target Intel cRIO;
- the 32-bit ARM `.so` under LabVIEW Real-Time on the NI myRIO-1900.

The library reference must be target-aware. Do not leave an absolute Windows DLL path in an RT wrapper. The reference guide must explain that `tempctl.dll` and `libtempctl.so` have different basenames, so extension substitution alone does not resolve both unless package naming or an alias is deliberately aligned.

---

## 13. Unit and integration test requirements

Retain every applicable v3 test and update expected Init/Reset/lifecycle results. Add tests for at least the following:

### 13.1 API and ABI

1. Version returns `0x040000`; setup count 18; diagnostic count 28.
2. Every null pointer and invalid zone/length case for `TcStart`, `TcStop`, and changed functions.
3. Negative returns write no outputs and change no state.
4. Export list is identical across every DLL and `.so`.
5. Buffers beyond element 18/28 remain untouched.

### 13.2 Init, enable, Start, Stop, and Reset

1. Valid enabled Init returns `IdleStopped`, outputs internally off, Started 0.
2. Init while heating no longer preserves the relay command.
3. Disabled or uninitialized Start cannot start and retains `TempCtrlDisabled`.
4. Accepted Start sets Started 1 but does not energize a relay until CheckTemp.
5. Repeated Start while active is idempotent and does not restart countdowns.
6. Stop from every active status returns both commands zero and `IdleStopped`.
7. Stop does not clear sensor failure, accumulators, or hourly counts.
8. Reset leaves the controller stopped; CheckTemp alone cannot resume it.
9. ConfigFault remains uncleared by Reset.
10. An active host sequence uses Stop and applies its returned zero commands before Init or non-fault Reset.
11. Direct Init/Reset clears internal commands but is not credited as immediate physical-output de-energization because those APIs return no DO values.

### 13.3 Start blocking

1. Start with false permissive returns `TC_OK`, status `IdleStartBlocked`, warning 8, commands off, no fault countdown.
2. Repeated CheckTemp while false does not promote a blocked Start to a fault.
3. Permissive recovery does not auto-start.
4. A later successful Start clears the blocked status.
5. Explicit Stop acknowledges a blocked Start and returns `IdleStopped`.
6. A failed Start from `IdleOperatingConditionTripped` retains the trip status instead of replacing it with `IdleStartBlocked`.

### 13.4 Permissive loss during operation

1. First false sample immediately returns both commands zero, status pending, full timeout remaining, Started 0.
2. A one-tick loss followed by recovery produces `IdleOperatingConditionTripped`, no fault, and no automatic restart.
3. False permissive held continuously faults on the exact qualified tick, not the observation tick.
4. Recovery one tick before timeout cancels the fault.
5. Stop during pending cancels escalation but retains the condition-trip cause.
6. A successful Start after recovery clears the non-fault trip status.
7. Start remains blocked until permissive is true.
8. Relay-feedback logic does not fault on the intentional output-off transition.
9. Normal sensor/control state is frozen while pending/tripped/stopped.
10. Existing sensor/disagreement/feedback fault maturing on the first false-permissive tick wins over the condition fault path.
11. A latched existing fault cannot be overwritten by later permissive changes.
12. `TcGetDiag` calls at arbitrary rates do not advance the condition timer.
13. Start with false permissive while pending preserves the pending status, elapsed countdown, and time reference.
14. Explicit Start with true permissive while pending or tripped is accepted and clears the non-fault condition state.
15. Repeated Start while already active is a no-op even when that Start call supplies false permissive; the next CheckTemp performs the live safety action and returns zero commands.

### 13.5 Time behavior

1. Operating-condition countdown across `uint32_t` wrap.
2. Backwards `nowMs` step adds zero elapsed time.
3. Same timestamp adds zero elapsed time.
4. Long forward gap expires a pending condition timeout correctly.
5. Stopped time is excluded by resetting the time reference at accepted Start.

### 13.6 Warning and status consistency

1. Warning 8 is lowest priority and can be masked without hiding the condition status.
2. Warning 8 clears live on recovery in the non-fault trip state while status 9 remains latched.
3. OperatingConditionFault freezes the warning under the normal fault rule.
4. All statuses 0–9 are non-faults and every status >=10 is treated as a fault.
5. Status/Warning mirrors update after every successful Init, Start, Stop, CheckTemp, or Reset; DO mirrors equal the internal commands after that call.
6. Init/Reset set `RunPermissive` to NaN; enabled non-fault Start/Check evaluations update it; disabled/faulted calls and active idempotent Start do not; Stop leaves it unchanged.
7. Successful Start clears warning 8 while preserving any applicable persistent `RunningOnTemp2` warning.
8. Stop from blocked clears warning 8; Stop from pending/tripped retains it while the last evaluated permissive is false; ordinary Stop clears transient warnings 1 through 5 and 8.
9. Stop behavior is covered for uninitialized, disabled, idle, blocked, pending, tripped, active, and faulted states.

### 13.7 Multi-zone and concurrency assumptions

1. Different zones may be interleaved with independent Start/Stop/permissive states.
2. A condition trip in one zone changes nothing in another.
3. Documentation forbids overlapping same-zone calls, including concurrent `TcGetDiag` reads.
4. A concurrency/stress test intentionally overlaps different-zone calls and confirms that independent zones share no mutable bookkeeping.

### 13.8 DBC, ECD, and CanTp wire format

1. All eight millisecond diagnostics are unsigned 16-bit, factor 1, offset 0 in both DBC and ECD.
2. Values 0, 1, and 64,255 round-trip exactly; 64,256 and larger values saturate to 64,255 on the wire without changing `TcGetDiag`.
3. NaN packs and unpacks as J1939 Not Available.
4. The generated DBC and decoded ECD agree by channel name on every signal property.
5. The generated payload length is 44 bytes and uses PGN 65280 / CAN ID `0x18FF00FE`.
6. Bit-exact oracle tests cover DBC tables and the flattened ECD definition.
7. If ECD channel order differs from diagnostic index order, the generated reorder table is applied and tested in both transmit and receive directions.
8. A v3 database definition is rejected or produces a clear version/layout error when used with the v4 simulator or test harness.

---

## 14. TempSim requirements

Update the TempSim application and CLI to the v4 API. Because TempSim is separately versioned, increment its version according to its own release policy; a major compatible release is recommended because the controller API changes incompatibly.

Required simulator changes:

- load only a v4 library with version/count checks before a scenario starts;
- use setup arrays of 18 and diagnostics arrays of 28;
- call `TcStart` before scenarios that expect active control;
- expose Start and Stop actions in the UI and scripted scenario format;
- add a live `Run Permissive` control/input;
- display the new lifecycle statuses and warning;
- display Started, permissive, and remaining condition-fault time;
- ensure a simulator Stop writes both modeled relay outputs off;
- update CSV, `.ncl`, CAN/DBC/ECD, and trace column definitions for new diagnostics;
- load the regenerated v4 `tempctl.ecd` and apply its generated reorder table if its channel order differs from `TC_DIAG_*` order;
- update status/warning name tables and expected-value assertions;
- retain the existing Windows screenshot/render gate;
- fail clearly when a v3 binary or stale DBC/table is loaded.

Add simulator scenarios:

1. enabled Init followed by idle—no control before Start;
2. successful Start and ordinary heat/cool trajectory;
3. manual Stop from HeatPending, HeaterOn, CoolPending, and CoolerOn;
4. blocked Start with permissive false;
5. one-tick permissive loss—immediate safe output-off, no fault, latched trip state;
6. recovery during countdown—no automatic restart;
7. sustained permissive loss—fault at the exact timeout;
8. Stop during condition pending—no later fault, cause retained;
9. fault-priority collision on the first false-permissive tick;
10. Reset then CheckTemp without Start—remains idle;
11. active reconfiguration uses Stop, applies zero modeled outputs, then Init; Init leaves the controller stopped and requires Start;
12. active non-fault Reset uses Stop, applies zero modeled outputs, then Reset; CheckTemp alone does not resume control;
13. Start while pending with false permissive preserves the countdown; Start while pending/tripped with true permissive explicitly restarts;
14. two zones with independent lifecycle/permissive states.

TempSim release gates must run on its supported Windows x64, Linux x86-64, and Linux ARM64 packages. Add a Linux 32-bit ARM build only if the TempSim runtime supports the actual myRIO userspace. Regardless of TempSim runtime availability on myRIO, the identical scenario vectors and expectations must be replayed on the myRIO using the native C test and/or a LabVIEW RT test harness; myRIO validation may not be omitted.

---

## 15. Target-system acceptance

### 15.1 Windows

- Run native tests under Windows x64 and x86.
- Generate wrapper VIs using the x86 DLL/header in 32-bit LabVIEW 2026.
- Execute every wrapper smoke test, including array mappings and Start/Stop.
- Record the LabVIEW version, architecture, parser settings, binary hash, and result.

### 15.2 Intel x86-64 cRIO

- Copy the x86-64 `.so`, identical header, and native test executable to the target.
- Inspect architecture, exports, dependencies, SONAME, and glibc requirements on the target.
- Run the complete native test suite on the target.
- Run the LabVIEW RT wrapper integration scenario against the deployed `.so`.
- Verify bounded execution and correct output-off behavior for Stop and permissive loss.
- Save a dated target log in the controller package and summarize it in `TESTLOG.txt`.

### 15.3 NI myRIO-1900

- Build a distinct 32-bit ARM `.so` and native test executable for the myRIO's ARM Cortex-A9 NI Linux Real-Time userspace.
- Confirm the actual target ABI/toolchain rather than inferring it from processor marketing names.
- Inspect the binary on the target (`file`, `readelf`/`nm`, dependency tool available on the image).
- Run the full native controller test suite on the myRIO-1900.
- Deploy and exercise the generated LabVIEW wrapper VIs on the myRIO target.
- Run at least the full v4 lifecycle/permissive scenario set through a LabVIEW RT test harness or compatible TempSim CLI scenario runner.
- Confirm `TcVersion`, setup/diagnostic counts, Init/Start/Check/Stop/Reset, array buffers, exact timeout behavior, and same-tick priority.
- Record target model, NI Linux RT image/software version, LabVIEW RT version, CPU/ABI details, compiler/toolchain, hashes, and results in a dated log included with the release.

An inspected but unexecuted myRIO binary does not satisfy acceptance.

### 15.4 Raspberry Pi / ARM64

If ARM64 remains a supported package, retain its native unit and simulator gates and update them to v4. It does not substitute for the myRIO test because AArch64 and ARM Cortex-A9 NI Linux RT are different targets.

---

## 16. Documentation deliverables

Ship a standalone reference document, recommended name `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`, in every distribution and the source documentation tree. It must contain:

- target/version matrix and correct binary selection;
- all exported prototypes and return codes;
- full setup and diagnostics tables;
- full status and warning tables;
- lifecycle state diagram or transition table;
- exact Init, Start, Stop, CheckTemp, Reset, and fault semantics;
- the distinction between master enable, Started, live permissive, non-fault trip, and fault;
- explicit statement that a false permissive de-energizes immediately but faults only after timeout;
- no-auto-restart rule;
- same-tick status/fault precedence;
- LabVIEW Import Shared Library instructions, verified include paths, `_WIN32`, cdecl, Run in any thread, and manual array corrections;
- requirement to serialize every same-zone call, explicitly including `TcGetDiag`;
- caller-owned output-buffer requirements;
- required `TcStop` -> apply zero DOs -> Init/active non-fault Reset sequences and the reason Init/Reset alone cannot retract an output already delivered by the host;
- Windows x86/x64, Intel cRIO x86-64, myRIO-1900 32-bit ARM, and ARM64 deployment instructions;
- target-side export/dependency diagnostics and common LabVIEW load errors;
- warning that the host must still implement physical safe states/watchdogs because the DLL cannot act if the application stops calling it;
- configuration guidance for every timeout;
- examples showing the normal call sequence and all blocked/tripped/faulted sequences.

Also update:

- normative `TEMPCTL-SPEC-v4.0.0.md`;
- capability/operating-rules document;
- distribution README;
- LabVIEW integration guide if retained separately;
- testing guide and release-gate commands;
- changelog with a complete v3-to-v4 migration section;
- dependency report, manifest, and test log;
- DBC documentation and generated tables;
- regenerated `tempctl.ecd`, its generation procedure, and any required diagnostic-to-ECD reorder table;
- TempSim guide, scenario reference, and changelog.

The migration section must explicitly warn that v3 wrapper VIs, v3 array constants, and v3 DBC/ECD/CanTp layouts are incompatible with v4.

---

## 17. Packaging and release gates

Packaging shall fail unless all of the following are true:

1. Version/count helpers report 4.0.0, 18, and 28.
2. All native unit tests pass on Windows x64 and x86.
3. All Linux target binaries are built and their architecture/export/dependency reports match their labels.
4. Native tests pass on the actual Intel cRIO and actual NI myRIO-1900.
5. Required ARM64 tests pass if that package remains supported.
6. LabVIEW wrapper generation and Windows wrapper smoke tests pass.
7. LabVIEW RT wrapper tests pass on both cRIO and myRIO.
8. TempSim passes all old applicable scenarios and all new v4 scenarios with zero expectation failures.
9. DBC and ECD regeneration is clean; all eight millisecond diagnostics are U16; and oracle/CanTp packing tests pass with 28 diagnostics and the 44-byte payload.
10. Every binary package contains the byte-identical `tempctl.h`.
11. Every required reference/specification/testing document is present and version-consistent.
12. Manifest hashes match every packaged file.
13. Release logs identify any explicitly waived platform gate; no myRIO execution gate may be waived under this handoff.

---

## 18. Change-to-test traceability

| Change | Intent | Required evidence |
|---|---|---|
| V4-1: explicit Start | Separate run lifecycle from master enable/configuration without bypassing a pending trip | Start accepted/blocked/idempotent and pending/tripped Start tests; wrapper test |
| V4-2: explicit Stop | Normal safe shutdown without fault or re-Init | Zero-output tests from every active state |
| V4-3: live permissive | Generic host-supplied backup operating-condition check | Active tick, stopped tick, disabled/faulted gating tests |
| V4-4: blocked Start | Prevent activation under invalid operating conditions | No countdown/no fault/no auto-start tests |
| V4-5: immediate trip | Conservative output de-energization | First-false-tick output and status assertions |
| V4-6: delayed condition fault | Ignore momentary loss as a fault while retaining cause | Recovery-before-timeout and exact-timeout tests |
| V4-7: latched non-fault cause | Preserve why control stopped | Status remains 9 after recovery until Start/Reset |
| V4-8: fault precedence | Do not hide more important existing status/fault feedback | Same-tick collision tests for every existing fault class |
| V4-9: revised Init/Reset | Never run without explicit Start and ensure the host applies zero DOs before an active reconfigure/reset | Stop-before-Init/Reset integration sequences and post-call idle tests |
| V4-10: diagnostics/DBC | Make lifecycle observable and transportable | Index-by-index diagnostics and bit-exact CAN oracle tests |
| V4-11: headers per package | Make LabVIEW import self-contained | Manifest/hash comparison across all packages |
| V4-12: myRIO support | Validate ARM NI Linux RT, not only x86-64/AArch64 | Native and LabVIEW RT logs from actual myRIO-1900 |
| V4-13: updated TempSim | Preserve independent scenario validation | CLI/UI scenario and package logs |
| V4-14: user reference | Prevent integration rediscovery and wrapper mistakes | Documentation checklist review and clean-room wrapper import |
| V4-15: compact regenerated ECD | Reduce CAN payload while keeping the controller's full-width timing | Generated DBC/ECD comparison, U16 boundary/saturation tests, payload-length assertion, and ECD-order test |

---

## 19. Deferred items and known limits

The following remain outside v4.0.0 unless separately approved:

1. Wall-clock/calendar timestamp for faults or stop events.
2. Individual operating-condition names or reason-bit masks inside TempCtl.
3. Automatic restart after permissive recovery.
4. Physically stuck relay/contact detection beyond digital-output feedback.
5. Heater/cooler no-progress or maximum-on-time faults.
6. Wrong-but-in-range survivor sensor detection after failover.
7. Multiple simultaneous warning outputs or a warning bitmask.
8. A separate independent process over-temperature trip.

The host and physical I/O layer remain responsible for watchdogs and fail-safe output behavior if LabVIEW, the RT process, the controller, or the call sequence stops executing.

---

## 20. Definition of done

TempCtl v4.0.0 is complete only when the implementation, all target binaries, identical packaged headers, wrapper VIs, DBC/tables, updated TempSim, documentation, and tests agree on one lifecycle model:

- Init configures and leaves idle.
- A zone that may be active is stopped and its returned zero DO commands are applied before Init or non-fault Reset.
- Enable permits Start but does not itself start.
- Start requires a valid enabled setup, no fault, and true permissive.
- Start while already active is an unconditional no-op; `TcCheckTemp` remains the live permissive enforcement call while active.
- Stop de-energizes without fault.
- False permissive blocks Start.
- False permissive during operation de-energizes immediately, retains the cause, and requires a new Start.
- Only a sustained active-operation permissive loss becomes a latched fault.
- Existing faults keep priority and are never overwritten.
- Reset clears eligible faults but leaves the controller stopped.
- No path automatically restarts control.
- Every call for one zone, including diagnostics, is serialized by the host.
- The same documented C contract operates under Windows LabVIEW, Intel cRIO NI Linux RT, and ARM myRIO-1900 NI Linux RT using target-native binaries.
