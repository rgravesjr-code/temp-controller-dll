# TempCtl v4.0.0 - API and LabVIEW Integration Guide

The standalone reference for users of `tempctl.dll` and `libtempctl.so`:
which binary to load, every export, the two arrays, every status and
warning, the lifecycle, the LabVIEW import, the deployment per target and
the call sequences. The normative rules are `TEMPCTL-SPEC-v4.0.0.md`, the
prose rules `TEMPCTL-CAPABILITY-v4.0.0.md`; this guide repeats what an
integrator needs so nothing has to be rediscovered.

## 1. Targets and versions

| Target | Binary in this package | Architecture | Loader / runtime need | Executed in this release |
|---|---|---|---|---|
| Windows 64-bit (LabVIEW x64, Python, .NET) | `tempctl.dll`, `tempctl.lib`, `test_tempctl.exe` | PE x86-64 | none (static CRT, imports `KERNEL32.dll` only) | yes: 2186 unit checks, oracle |
| Windows 32-bit LabVIEW 2026 | `x86\tempctl.dll`, `x86\tempctl.lib`, `x86\test_tempctl.exe` | PE x86 | none | yes: 2186 unit checks |
| Intel cRIO-904x/905x/906x, NI Linux RT | `linux-x64\libtempctl.so`, `linux-x64\test_tempctl` | ELF64 x86-64, glibc 2.2.5+ | libc only | see `TESTLOG.txt` (owner-run log, or built and inspected) |
| NI myRIO-1900, NI Linux RT (Xilinx Zynq-7010, dual Cortex-A9) | `linux-armhf\libtempctl.so`, `linux-armhf\test_tempctl` | ELF32 ARM, EABI v5, hard float (`-mcpu=cortex_a9`) | the library imports nothing from libc; the test needs glibc 2.4+ | see `TESTLOG.txt` (owner-run log, or built and inspected) |
| Raspberry Pi 4/5 (bench) | `linux-arm64\libtempctl.so`, `linux-arm64\test_tempctl` | ELF64 AArch64, glibc 2.17+ | libc only | yes: 2186 unit checks, 29 simulator scenarios |

Every folder carries the same `tempctl.h` (byte-identical, hashed in
`MANIFEST.txt`). `TcVersion()` returns `0x040000`; `TcSetupCount()` 18;
`TcDiagCount()` 28. **Do not mix versions**: v3 wrapper VIs, v3 array
constants (17 / 25) and the v3 DBC / ECD / CanTp tables (55-byte message)
are incompatible with a v4 binary. Pick the binary by architecture, never by
board name alone: the cRIO is x86-64, the myRIO is 32-bit ARM, the Pi is
AArch64. On the target, `file libtempctl.so` and `python3 elfinfo.py
libtempctl.so` (from `tools\`) show what you have.

## 2. Exports and return codes

```c
int32_t TcVersion(void);       /* 0x040000 */
int32_t TcSetupCount(void);    /* 18 */
int32_t TcDiagCount(void);     /* 28 */
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

All C linkage, cdecl, `int32_t` / `uint32_t` / `double` and pointers to
them only. Booleans are `int32_t`: 0 = false, anything else = true.

| Return | Name | Meaning |
|---|---|---|
| 0 | `TC_OK` | The call ran. The controller's state is in `status` / `warning`, not here. A refused Start is `TC_OK`. |
| -1 | `TC_ERR_ARG` | A required pointer is null, `setupLen != 18`, or `diagLen < 28`. Nothing ran; no output written. |
| -2 | `TC_ERR_ZONE` | `zone` outside 0..15. Nothing ran; no output written. |

**Caller-owned buffers.** `setupArray` (18 DBL) and `diagArray` (at least
28 DBL, pre-sized by the caller) are the host's memory; the library never
allocates. Elements of `diagArray` beyond 28 are not written.

**Serialization.** Every call for one zone, including the read-only
`TcGetDiag`, must be serialized by the host (LabVIEW dataflow does this
when the wrapper VIs are chained; do not call `TcGetDiag` for a zone from a
parallel loop while that zone's `TcCheckTemp` may run). Different zones may
be processed independently. The library has no locks and owns no thread,
clock, CAN interface or output.

**`nowMs`** is the LabVIEW *Tick Count (ms)*: free-running, wrapping,
only differences are used. A backwards step counts as 0 ms. No wall-clock
time is captured; add your own timestamp when you observe a status change.

## 3. The setup array (18 DBL, `TcInit`)

Booleans: `> 0.1` is 1. Timeouts: whole ms, fractions truncated, minimum 1.
Temperatures in the unit named by `TempUnits` (label only).

| # | Name | Unit | Meaning | Guidance |
|---|---|---|---|---|
| 0 | TempCtrlEnable | 0/1 | master enable; permits Start, does not start | 0 makes the zone inert |
| 1 | TempUnits | 0/1 | 0 degF, 1 degC, label only | |
| 2 | Setpoint | deg | heating releases at >=, cooling at <= | inside both bands |
| 3 | DeadbandHi | deg >= 0 | `HiBand = Setpoint + DeadbandHi` | not both deadbands 0 |
| 4 | DeadbandLo | deg >= 0 | `LoBand = Setpoint - DeadbandLo` | |
| 5 | HiLimit | deg | sensor valid range, top | > HiBand |
| 6 | LoLimit | deg | sensor valid range, bottom | < LoBand |
| 7 | ErrorTimeout | ms | accumulated out-of-range time that fails a sensor | at least two loop periods; 1-3 s typical for thermocouples |
| 8 | DeadbandTimeout | ms | time outside the band before a relay turns on | longer than the plant's noise excursions |
| 9 | AtSetPtTimeout | ms | time at the setpoint before the running relay turns off | sets the overshoot with the plant's rate |
| 10 | Temp2Enable | 0/1 | second sensor fitted | |
| 11 | Temp2Offset | deg | added to temp2 before every use | finite (checked only if 10) |
| 12 | Temp2Tolerance | deg >= 0 | largest allowed `|Temp1Avg - Temp2Avg|` | (only if 10) |
| 13 | TempCompareTimeout | ms | disagreement time before the fault; warning at 1/10 | seconds to tens of seconds (only if 10) |
| 14 | FilterPoints | 1..64 | average length for the comparison; anything else -> 4 | never fails |
| 15 | FeedbackEnable | 0/1 | compare the DO read-backs with the commands | |
| 16 | RelayFeedbackTimeout | ms | mismatch time before a relay fault | longer than the worst DO-loop latency, several loop periods (only if 15) |
| 17 | OperatingConditionTimeout | ms | how long a lost run permissive may stay false, after control stopped, before `OperatingConditionFault` | longer than the longest expected transient of your conditions and at least two loop periods; it never delays the relay shutdown |

An invalid enabled setup gives `ConfigFault` (14); an invalid disabled
setup gives warning `ConfigInvalid` (7). New parameters are only ever
appended.

## 4. The diagnostics array (28 DBL, `TcGetDiag`)

| # | Name | Meaning |
|---|---|---|
| 0 | ControlTemp | raw value control acts on (sensor 2 corrected); NaN before the first active tick or while the reading is NaN |
| 1 | ActiveSensor | 1 or 2 |
| 2 | Temp1Raw | last temp1 (mirrored while stopped as well) |
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
| 17 | Temp1OorEventsPerHour | in-range -> out-of-range transitions in the last 60 min (wall time) |
| 18 | Temp2OorEventsPerHour | same for sensor 2 |
| 19 | StatusMirror | status of the last Init / Start / Stop / CheckTemp / Reset |
| 20 | WarningMirror | warning of that call |
| 21 | doHeaterMirror | internal heater command after that call (0 after Init / Reset / Stop / trip) |
| 22 | doCoolerMirror | internal cooler command after that call |
| 23 | AppliedFilterPoints | FilterPoints in use |
| 24 | ZoneInitialized | 1 once a setup has been loaded |
| 25 | RunPermissive | last permissive evaluated by an enabled, non-faulted Start or CheckTemp (0/1); NaN after Init / Reset until then |
| 26 | OperatingConditionRemainMs | ms before a pending permissive loss becomes OperatingConditionFault; 0 when not pending |
| 27 | ControllerStarted | 1 while a Start is accepted and control may run |

On the CAN wire (`dbc\tempctl.dbc`, `dbc\tempctl.ecd`, `dbc\tables\`) the
28 values are one 44-byte J1939 BAM under PGN 65280 / id `0x18FF00FE`; the
eight millisecond values are U16 and saturate at 64255 on the wire (the
array itself is full width); NaN is "not available" (all ones).

## 5. Status and warning codes

| Status | Name | Class | Meaning |
|---|---|---|---|
| 0 | TempCtrlDisabled | inert | Enable 0 or no setup; relays 0; Start refused |
| 1 | TempAtSetPt | running | relays off, in band (also provisional right after Start) |
| 2 | HeaterON | running | heating commanded |
| 3 | CoolerON | running | cooling commanded |
| 4 | HeatPending | running | below LoBand, deadband countdown running |
| 5 | CoolPending | running | above HiBand, deadband countdown running |
| 6 | IdleStopped | stopped | configured, not started (after Init / Reset / Stop) |
| 7 | IdleStartBlocked | stopped | Start refused: permissive false; no countdown |
| 8 | OperatingConditionPending | stopped | permissive lost while running; relays off; fault countdown running |
| 9 | IdleOperatingConditionTripped | stopped | the loss recovered or was stopped before the timeout; cause latched until Start / Reset / Init |
| 10 | Temp1FailHigh | fault | single-sensor mode: sensor 1 failed high (incl. NaN / Inf) |
| 11 | Temp1FailLow | fault | single-sensor mode: sensor 1 failed low |
| 12 | BothSensorsFailed | fault | two-sensor mode: no healthy sensor left |
| 13 | TempDisagreeFault | fault | sensors disagreed for TempCompareTimeout |
| 14 | ConfigFault | fault | Init check failed with Enable 1; only a passing Init clears it |
| 15 | HeaterFBFault | fault | heater DO feedback mismatched for RelayFeedbackTimeout |
| 16 | CoolerFBFault | fault | cooler DO feedback mismatched for RelayFeedbackTimeout |
| 17 | OperatingConditionFault | fault | permissive stayed false for OperatingConditionTimeout after being lost while running |

Every status >= 10 is a fault: relays 0, latched, diagnostics frozen, until
Reset or Init. The status is the authoritative state and the retained cause
of a stop; the warning is secondary and single-valued (lowest code wins).

| Warning | Name | Set | Cleared |
|---|---|---|---|
| 0 | NoWarning | | |
| 1 | Temp1OutOfRange | sensor 1 out of range now (running) | back in range; Stop / trip |
| 2 | Temp2OutOfRange | corrected sensor 2 out of range now | back in range; Stop / trip |
| 3 | HeaterFBMismatch | heater read-back != previous command | match; Stop / trip |
| 4 | CoolerFBMismatch | cooler read-back != previous command | match; Stop / trip |
| 5 | TempDisagree | disagreement for TempCompareTimeout / 10 | agreement or the comparison pauses |
| 6 | RunningOnTemp2 | sensor 1 failed, control on sensor 2 | Reset / Init only (survives Stop and Start) |
| 7 | ConfigInvalid | Init with Enable 0 failed the check | the next passing Init |
| 8 | OperatingConditionNotMet | Start refused by a false permissive, or permissive false while pending / tripped | permissive true again; Stop from blocked; a successful Start; frozen by OperatingConditionFault |

Same-tick fault order: config -> sensor range -> disagreement -> heater
feedback -> cooler feedback -> operating condition. An existing fault
maturing on the tick the permissive is lost wins; a latched fault is never
overwritten.

## 6. The lifecycle

Three separate concepts:

- **Master enable** (`TempCtrlEnable`): configuration. False = inert, cannot be started.
- **Started** (`TcStart` / `TcStop`): the operator's or application's run command. No fault, no reload of the configuration.
- **Run permissive** (`runPermissive` on Start and every CheckTemp): the host's live, combined operating condition. TempCtl is the backup: it blocks a Start, stops at once when the permissive is lost, keeps the reason, and faults only if the loss lasts.

A false permissive **de-energizes both relays on the first sample** that
observes it and faults **only after `OperatingConditionTimeout`**. Nothing
restarts control except `TcStart`: not the permissive returning, not a
CheckTemp, not Reset, not Init.

| From | Event | To | Outputs | Fault | Restart rule |
|---|---|---|---|---|---|
| Uninitialized | Start | TempCtrlDisabled | off | no | Init first |
| Disabled | Start | TempCtrlDisabled | off | no | enable through a valid Init, then Start |
| IdleStopped | Start, permissive true | running (provisional TempAtSetPt) | off until the first CheckTemp decides | no | accepted |
| IdleStopped | Start, permissive false | IdleStartBlocked | off | no | new Start after recovery |
| IdleStartBlocked | permissive recovers (CheckTemp) | IdleStartBlocked, warning clears | off | no | new Start required |
| IdleStartBlocked | Stop | IdleStopped | off | no | Start when ready |
| running | Stop | IdleStopped | off immediately | no | new Start required |
| running | permissive false (CheckTemp) | OperatingConditionPending | off immediately | no, timer starts | no auto-restart |
| Pending | permissive recovers | IdleOperatingConditionTripped | off | no | new Start required |
| Pending | Start, permissive false | Pending (timer preserved) | off | no | keep calling CheckTemp, or Stop |
| Pending | Start, permissive true | running | off until CheckTemp | no | explicit restart |
| Pending | Stop | IdleOperatingConditionTripped | off | no | new Start required |
| Pending | false through the timeout | OperatingConditionFault | off | yes | Reset, then Start |
| Tripped | Start, permissive true | running | off until CheckTemp | no | clears the trip |
| Tripped | Start, permissive false | Tripped (warning 8) | off | no | Start when true |
| Tripped | Stop | Tripped (cause kept) | off | no | new Start required |
| any non-fault | an existing controller fault | that fault | off | yes | Reset / Init, then Start |
| any fault | Start or Stop | same fault | off | stays | Reset, or a valid Init |
| resettable fault | Reset | IdleStopped | off | cleared | Start required |
| ConfigFault | Reset | ConfigFault | off | stays | passing Init, then Start |

**Init and Reset return no DO values.** They zero the library's commands but
cannot retract what the host already wrote to the outputs. Required
sequences for a zone that may be active:

```
active reconfiguration:  TcStop -> write the returned 0/0 to the DOs -> TcInit -> TcStart
active non-fault reset:  TcStop -> write the returned 0/0 to the DOs -> TcReset -> TcStart
```

A faulted zone returned 0/0 on its fault tick; `TcReset` may follow
directly. A direct `TcInit` is fine on an uninitialised, disabled, faulted
or known-idle zone.

While a zone is not started, `TcCheckTemp` still has to be called if you
want the pending countdown to run and the diagnostics to follow the inputs;
it returns 0/0, evaluates no sensor, and only stores the permissive and
mirrors the raw temperatures. A sensor that fails while stopped is detected
after the next Start.

## 7. LabVIEW: importing the header

Use **Tools -> Import -> Shared Library (.dll)** in **32-bit LabVIEW 2026**
against `x86\tempctl.dll` and `tempctl.h` (the same header is valid for the
x64 DLL and every `.so`). Verified parser configuration:

```
Include paths, in order:
  C:\Program Files (x86)\National Instruments\Shared\LVDB 2015\include\ansi
  C:\Program Files (x86)\National Instruments\Shared\LVDB 2015\include
Preprocessor definition:
  _WIN32
```

Select all nine functions. Expected node configuration:

- Calling convention **C**; **Run in any thread** (no allocation, I/O or blocking inside); error checking default.
- Generated wrapper VIs non-reentrant at first; every call for one zone, including `TcGetDiag`, serialized by dataflow.
- `setupArray`: **Array, 1 dimension, 8-byte Double, Array Data Pointer**, minimum size 18, `setupLen` wired as its length (18).
- `diagArray`: **Array, 1 dimension, 8-byte Double, Array Data Pointer**, caller-allocated with `Initialize Array` to 28, `diagLen` wired (28).
- every `int32_t*` output: **Numeric, Signed 32-bit, Pointer to Value**.
- `runPermissive`: Numeric, Signed 32-bit, Value (wire a Boolean through *Boolean To (0,1)*).

The wizard cannot tell a `double*` scalar from an array: **verify and, if
needed, correct the two arrays** (`setupArray`, `diagArray`); nothing else
needs correcting. Per-function CLFN tables are in `LABVIEW_INTEGRATION.md`
section 3.

**Wrapper smoke test**, in this order: `TcVersion` (0x040000) ->
`TcSetupCount` (18) -> `TcDiagCount` (28) -> `TcInit` (status 6) ->
`TcStart` with permissive 1 (status 1) -> `TcCheckTemp` with a reading below
the band (status 4, both DOs 0, `DeadbandRemainMs` = `DeadbandTimeout`) ->
`TcStop` (0/0, status 6) -> `TcReset` (status 6) -> `TcGetDiag` (28 values,
`ZoneInitialized` 1, `ControllerStarted` 0, `RunPermissive` NaN).

**Library path per target.** `tempctl.dll` and `libtempctl.so` have
different basenames, so LabVIEW's automatic extension substitution alone
does not resolve both. Either make the library path a conditional-disable
or a control (`tempctl.dll` next to the VI on Windows,
`/usr/local/lib/libtempctl.so` on RT), or deploy the `.so` under a matching
alias. Never leave an absolute Windows path in an RT wrapper.

## 8. Deployment per target

| Target | Copy | Then |
|---|---|---|
| Windows x64 | `tempctl.dll` (+ `third_party\cantp\cantp.dll` for CAN) next to the VI / EXE or on PATH | `test_tempctl.exe` -> `TempCtl 4.0.0 unit tests: 2186 passed, 0 failed` |
| Windows x86 (32-bit LabVIEW) | `x86\tempctl.dll` (+ `third_party\cantp\x86\cantp.dll`) | `x86\test_tempctl.exe` |
| Intel cRIO | `scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<ip>:/usr/local/lib/`; `scp linux-x64\test_tempctl admin@<ip>:/home/admin/` | `ssh admin@<ip> "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl; /home/admin/test_tempctl"` |
| myRIO-1900 | `scp linux-armhf\libtempctl.so third_party\cantp\linux-armhf\libcantp.so admin@<ip>:/usr/local/lib/`; `scp linux-armhf\test_tempctl admin@<ip>:/home/admin/` | same command; expect the same `2186 passed` line |
| Raspberry Pi (aarch64) | `linux-arm64\...` | same |

SSH on an NI target: NI MAX -> the target -> System Settings -> Enable
Secure Shell. Confirm the target's ABI before deploying:

```
file /usr/local/lib/libtempctl.so          # ELF 32-bit LSB shared object, ARM, EABI5 (myRIO) / x86-64 (cRIO)
ldd --version | head -1                    # the target's glibc; the libraries need none, the tests 2.4 (ARM) / 2.2.5 (x86-64)
python3 elfinfo.py libtempctl.so           # exports, NEEDED, SONAME (tools\elfinfo.py, no dependencies)
```

`TESTLOG.txt` states for each Linux target whether `test_tempctl` was
executed on real hardware in this release or only built and inspected.

## 9. Common LabVIEW errors

| Symptom | Cause / fix |
|---|---|
| Error 7 (file not found) on RT | wrong path, wrong architecture folder, or not executable: `ls -l /usr/local/lib/libtempctl.so`, `chmod 755`, `file` |
| Error 13 / "not a valid Win32 application" | 32-bit LabVIEW loading the x64 DLL or the reverse: use `x86\` for 32-bit LabVIEW |
| "wrong ELF class" or "cannot execute binary" on RT | the x86-64 `.so` on the myRIO or the ARM `.so` on the cRIO; check with `file` |
| Return -1 | `setupLen` not 18, `diagLen` below 28, or an unwired pointer output; size from `TcSetupCount` / `TcDiagCount`; a v3 wrapper with 17 / 25 |
| Return -2 | zone outside 0..15 |
| Status 6 and nothing happens | Init, Reset or Stop left the zone stopped: call `TcStart` |
| Status 7 | the last Start saw a false permissive; fix the conditions and Start again (nothing auto-starts) |
| Status 8 then 9 | the permissive dropped while running; relays are off; Start again when the conditions are back |
| Status 17 | the permissive stayed false for `OperatingConditionTimeout`; Reset, then Start; lengthen the timeout if brief transients are expected |
| Status 14 right after Init | config check (units, deadbands, band inside limits, a timeout below 1 ms or NaN, `OperatingConditionTimeout` included) |
| Status 10-13, 15, 16 | see `LABVIEW_INTEGRATION.md` section 7 |
| Warning 3/4 for one pass at each relay change | normal DO-loop lag; raise `RelayFeedbackTimeout` if it ever faults |
| Warning 8 with status 6 | impossible by design: `IdleStopped` never carries warning 8; if seen, the wrapper wires the outputs of different calls together |

## 10. The host still owns the safe state

TempCtl acts only when it is called. If LabVIEW, the RT process or the
loop stops, the last DO values the host wrote stay on the outputs. The host
and the I/O layer must implement watchdogs and fail-safe output behaviour;
`TcStop`'s zeros and the permissive shutdown are conservative controller
behaviour, not a safety interlock.

## 11. Call sequences

```
power-up:            TcInit(z, tick, setup, 18, &st, &wn)         st 6 IdleStopped (14 ConfigFault: fix, Init again)
operator start:      TcStart(z, tick, perm, &st, &wn)             st 1 accepted / 7 blocked (perm 0) / 0 disabled / fault kept
each pass:           TcCheckTemp(z, tick, t1, t2, hfb, cfb, perm, &dh, &dc, &st, &wn); write dh, dc to the DOs
operator stop:       TcStop(z, tick, &dh, &dc, &st, &wn); write dh = dc = 0 to the DOs      st 6 (or 9 if a trip was pending)
parameter change:    TcStop -> write 0/0 -> TcInit(new setup) -> TcStart
after a fault:       (0/0 already written on the fault tick) TcReset -> fix -> TcStart
display / CAN:       TcGetDiag(z, diag, 28) -> CanTp_Pack(...)   any rate, same-zone serialized
```

Blocked, tripped and faulted sequences, tick by tick, are the `blocked-start`,
`permissive-trip`, `permissive-recover`, `permissive-fault`,
`stop-while-pending`, `start-while-pending` and `permissive-fault-priority`
scenarios of the TempSim package (`SIMULATOR.md`, with CSV traces).
