# Testing Guide - TempCtl v4

Release gate for v4.0.0: `build.bat all` must finish with
`TempCtl 4.0.0 unit tests: 2194 passed, 0 failed` for both the x64 and the
x86 test executables and build the three Linux targets; `python
tools\make_tempctl_dbc.py --tables --ecd` must regenerate the DBC, the CanTp
tables and the ECD without a difference (44-byte payload asserted, ECD read
back and compared with the DBC and the tables); `tests\oracle_test.py` must
print `ALL OK`. `package_dist.bat` re-runs the Windows gates, checks that
`dbc\tempctl.dbc`, `dbc\tables\TempCtl.json` and `dbc\tempctl.ecd` match the
generator, runs the oracle, appends the target logs from `docs\testlogs\`
and records everything in `TESTLOG.txt`; it refuses to package a failing
build. The simulator has its own gate (`build_sim.bat` / `package_sim.bat`):
the 29 scenarios with all 208 expectations passing and zero unpack
mismatches, the ECD cross-check, plus the WPF screenshot run; its log is in
the TempSim package.

## What the gates cover

### `tests\test_main.c` (2194 checks, compiled with `src\tempctl.c`)

One test function per rule group of `TEMPCTL-SPEC-v4.0.0.md`; the v3
change list rows (C1-C22) and scenarios (S1-S15) and the v4 handoff's
section 13 items (13.x.y) are named in the comments. The harness starts
every v3 test with Init + Start (permissive 1) so the v3 checks keep their
meaning under the lifecycle; the raw calls are used by the R10 groups.

- **API (C1, 13.1):** version 0x040000, counts 18 / 28, every argument
  error (-1, -2) for every function including `TcStart` and `TcStop`, the
  v3 setup length (17) refused, outputs untouched and no state change on a
  negative return, longer diag buffers accepted and left untouched beyond 28.
- **R2 enable / lifecycle (C3, S12):** uninitialised zone inert and
  `TC_OK`; `Enable = 0` inert under out-of-range, NaN, below-band and
  feedback stimuli; the 0.1 boolean threshold; power-on state; Start and
  Stop on disabled / uninitialised zones.
- **R3 units (C4), R4 filter (C9), R4.4 NaN and chatter (C10, S4), R5.4
  accumulator and R5.6 events (C11, S5), R5.5 failover (C12, S7), R5.7 pause
  (C13, S6), R6 offset / disagreement / gating (C5-C7, S8, S9), R7 control
  and `Initial_HC_Flag` (C8, C14, S1, S3), R8 feedback (C15, S10), R9.5
  config (C16, S11), R9.1-R9.2 Init / Reset (C17, S2, S13), R9.6 fault
  behaviour (C19), R1.4 / R1.5 time (C21, S15), S14 two zones, C22
  diagnostics:** as in v3 (see the v3 spec's test list), with these v4
  changes: the R9.3 relay keeping is replaced by "a re-Init drops the relay
  and leaves the zone `IdleStopped`" (13.2.2); the feedback previous
  command is 0 after Init, Reset, Stop, Start and a trip (R8.3, 13.4.8); the
  `OperatingConditionTimeout` joins the 31 invalid-setup cases (0, 0.999,
  -1, NaN); the diagnostics point check covers the three new fields.
- **R10.2 / R10.5 Init and Reset (13.2.1, 13.2.2, 13.2.8-13.2.11):** a valid
  enabled Init is `IdleStopped` with relays off, Started 0, permissive NaN;
  20 stopped ticks below the band and 20 NaN ticks change nothing (no
  countdown, no accumulation, no warning, raw mirrored, average NaN); Init
  while heating drops the relay; direct Init / Reset zero the mirrors; Reset
  leaves the zone stopped and CheckTemp alone never resumes; `ConfigFault`
  survives Reset, Start and Stop; the Stop -> Init and Stop -> Reset host
  sequences; Reset clears the blocked and tripped states and a pending
  countdown.
- **R10.3 Start (13.2.3-13.2.5, 13.3, 13.4.15):** refused on uninitialised
  and disabled zones (with `ConfigInvalid` kept); accepted Start: Started 1,
  provisional `TempAtSetPt`, relays off until the first qualified decision,
  fresh 500 ms countdown; Start while started is a no-op even with a false
  permissive, no countdown restart, no time re-base, the next CheckTemp
  performs the live safety action; blocked Start: `TC_OK`, status 7,
  warning 8, no countdown through 50 false ticks and an hour; recovery
  clears warning 8 live without starting; a later Start clears the block;
  Stop acknowledges a blocked Start; `IdleStopped` never warns 8; Start
  preserves the failover, accumulators and hourly counts and clears the
  averages and the flag; the hourly rings age in wall time across a Stop
  (at Start and on stopped ticks) while the accumulator does not drain.
- **R10.4 Stop (13.2.6, 13.2.7, 13.6.8, 13.6.9):** Stop from `TempAtSetPt`,
  `HeatPending`, `HeaterON`, `CoolPending` and `CoolerON` returns 0/0 and
  `IdleStopped` with the countdowns cancelled; stopped time excluded by the
  Start re-base; Stop idempotent; Stop on an uninitialised, disabled (with
  and without `ConfigInvalid`) and faulted zone; ordinary Stop clears
  warnings 1, 2, 3, 4, 5 and keeps the history; `RunningOnTemp2` persists;
  `RunPermissive` unchanged; Stop does not evaluate a permissive.
- **R10.7 / R10.8 permissive loss (13.4):** first false sample: 0/0,
  pending, full timeout, Started 0, permissive 0, from `HeaterON`, `CoolerON`,
  `HeatPending` and `TempAtSetPt`; one-tick loss -> tripped, no fault, no
  restart, warning 8 live; refused Start from tripped keeps the trip cause;
  fault on the exact qualified tick (observation + 10) with the countdown
  pinned per tick; latched through 20 true ticks, Start and Stop with the
  diagnostics frozen; Reset then Start; recovery one tick before the timeout;
  no second countdown from tripped; Stop during pending keeps the cause and
  warning 8 while the last permissive was false; Start with a false
  permissive while pending preserves the countdown and the time reference
  (exact remaining values after a 250 ms gap); Start with a true permissive
  while pending or tripped restarts; sensor / control state frozen while
  pending and tripped (accumulator, events, average, ControlTemp); feedback
  never faults on the intentional off transition and catches a still-closed
  relay after the restart; same-tick priority for sensor, disagreement,
  heater-feedback and cooler-feedback faults over the permissive; a fault
  that would mature one tick later is frozen by the trip; 100 `TcGetDiag`
  calls advance nothing; the trip clears the transient warnings and
  `RunningOnTemp2` masks 8 through pending and the fault.
- **R10.7 time (13.5):** the operating-condition countdown across the 2^32
  wrap, a 50 s backwards step and the same timestamp add nothing, a 600 s
  gap expires it, stopped time excluded.
- **Status / warning consistency (13.6):** every code value; the mirrors
  follow Init, Start, Stop, CheckTemp and Reset but not `TcGetDiag`;
  `RunPermissive` NaN after Init / Reset, updated by enabled non-faulted
  Start and CheckTemp (a refused Start included), not by disabled, faulted
  or idempotent calls, unchanged by Stop, frozen at the value evaluated on a
  fault tick; a disabled zone never warns 8.
- **Two zones (13.7):** one zone trips and faults on its permissive while
  the other keeps heating; Stop, Reset and Start on one leave the other
  untouched; all 16 zones hold independent lifecycle states.

### `tests\oracle_test.py` (independent implementations)

Loads `tempctl.dll` and the vendored `cantp.dll` through ctypes and defines
two CanTp slots: one from `dbc\tables\TempCtl.json` (`CanTp_Define`), one
from the `TempCtl` cluster of `dbc\tempctl.ecd` (`CanTp_DefineFlat`, read
with CanTp's `ecdflat.py`). Both slots must read back identical rows and
pack identical bytes. It then runs boundary vectors and a scripted scenario
(Init, a stopped tick, a blocked Start, Stop, an accepted Start, heat-up on a
toy plant, at-setpoint release, sensor 1 open with failover to sensor 2, a
one-tick permissive loss with an explicit restart, a sustained loss to
`OperatingConditionFault` on the exact tick, Reset, a relay feedback fault,
Stop on a faulted zone, single-sensor NaN stream, Reset + Start + Stop from
`HeaterON`, a 100 s operating-condition timeout that reads 100000 in the
array and 0xFAFF on the wire). After every call the 28-value diagnostics
array is packed by `CanTp_Pack`, the BAM is reassembled by the script's own
TP.CM/TP.DT parser, and the payload is compared bit-for-bit with cantools
encoding the same values with `dbc\tempctl.dbc` (pad bits masked; NaN
signals checked for the all-ones pattern; the eight millisecond signals
checked for `min(value, 64255)` and 0xFFFF on NaN). cantools' decode of
CanTp's payload must equal `CanTp_Unpack`, and the diagnostics mirrors must
equal the call outputs. It refuses a non-v4 binary and a table with other
than 28 rows with a clear message. 1346 arrays (30 boundary vectors).

```bat
pip install cantools
python tests\oracle_test.py [--tempctl build\win-x64\tempctl.dll] [--cantp third_party\cantp\cantp.dll] [--pylibs DIR]
```

### `tools\make_tempctl_dbc.py`

Verifies the DBC signal order and the status / warning value tables against
the `TC_DIAG_*`, `TC_ST_*` and `TC_WN_*` defines in `tempctl.h`, asserts the
44-byte payload, loads the written DBC with cantools (strict), checks the
frame id, length, `VFrameFormat`, order, that no two signals overlap and
that every millisecond signal is U16 / 1 / 0 / 0..64255; with `--ecd` it
writes `tempctl.ecd` (an encrypted LabVIEW flatten of one `J1939Msg(V4)`
cluster, channels in `TC_DIAG_*` order), decrypts it again with CanTp's
`ecdflat.py` and compares every channel (start bit, length, type, byte
order, factor, offset, min, max, unit, lookup table) with the DBC and the
derived CanTp rows with `dbc2tables`' rows. `package_dist.bat` regenerates
all three and fails if the shipped files differ.

### Simulator gates (TempSim package)

`TempSim.Cli --scenario all`: 29 scenarios with 208 built-in expectations
(status, warning, relays, physical relay states, Started, permissive,
countdown and accumulator values at exact ticks), every tick packed by
`CanTp_Pack` and read back by `CanTp_Unpack`; the shipped `tempctl.ecd`
cross-checked against `TempCtl.json` at start-up; exit 1 on any failed
expectation or unpack mismatch, 2 on a stale table / ECD or unreached
expectations. A non-v4 library is rejected before running scenarios.
`TempSim.exe --screenshot`: loads the failover scenario, runs
75 s, renders the window, and reports checks reached in that snapshot;
later expectations remain explicitly listed as not yet due. Additional
WPF gates complete blocked-start, permissive-trip and reset scenarios,
checking the permissive checkbox against the live input on every tick.
Both run
against the same `tempctl` binaries this package ships.

### On Linux

`linux-arm64\test_tempctl` was run on a Raspberry Pi 5 (aarch64):
`2194 passed, 0 failed` (`docs\testlogs\pi-test_tempctl-2026-09-22-review.txt`,
appended to `TESTLOG.txt`). The `linux-arm64` simulator ran all 29 scenarios
with 208/208 expectations and produced 60 CSV and `.ncl` files byte-identical
to the Windows run (TempSim package, `testlogs\pi-sim-2026-09-22-review.txt`).
The 600-second fixture run adds three more byte-identical files, alongside
30 passing fixture checks and 26 regression checks on each platform.

`linux-x64\test_tempctl` (Intel cRIO) and `linux-armhf\test_tempctl`
(myRIO-1900) are the same program. The `.so` files are inspected at package
time with `tools\elfinfo.py` (`DEPENDENCIES.txt`: ELF class, machine,
exports, imports, SONAME; the ARM library is EABI v5 hard float and imports
nothing from libc). Whether each was executed on its target in this release
is stated in `TESTLOG.txt`: a validated dated log in `docs\testlogs\`
(`crio-test_tempctl-<date>.txt`, `myrio-test_tempctl-<date>.txt`) when the
owner ran it, otherwise "built and inspected, not executed". The 32-bit ARM
binary cannot run on the aarch64 Pi (no 32-bit loader), so the myRIO is its
only execution target.

A filename alone is not evidence of execution. A current target log must
contain these metadata lines (actual lowercase or uppercase SHA-256 hashes),
the matching `TempCtl 4.0.0 unit tests: 2194 passed, 0 failed` summary,
and `exit=0` captured from the test process. The Pi log is required;
V4-D5 permits the Intel cRIO and myRIO to remain built/inspected only:

```text
# target: linux-armhf
# test_sha256: <sha256 of the test_tempctl being packaged>
# library_sha256: <sha256 of the libtempctl.so being packaged>
```

Use `linux-x64` or `linux-arm64` for those targets. Also record model,
image/OS, date and command. Old or hash-mismatched logs remain historical
and cannot mark rebuilt binaries executed; a matching failed/incomplete
log blocks packaging. This certifies execution of the test executable;
loading the shared library through LabVIEW remains a separate owner gate.

`tests/TempSim.RegressionChecks` covers event timestamps, stopped-time
exclusion, companion clocks, noise bounds/mean, truncated runs, table
identity and missing/corrupt ECD rejection, CAN ordering and paced sends.
It also compares all diagnostics over 64,000 samples on 16 concurrent
native zones with a serial reference (handoff 13.7.4).
The 30 fixture checks and these regressions run against the exact Core and
native binaries being packaged via `scripts/verify_sim_regressions.ps1`.
`powershell -File tests/test_release_gates.ps1` exercises oracle failure
and target-log validation, including stale hashes and failed/incomplete logs.

### Owner-executed gates (not automated here)

- LabVIEW: import `tempctl.h` with the x86 DLL in 32-bit LabVIEW 2026 with
  the parser settings of `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md` section 7,
  correct the two arrays, run the smoke sequence Version -> SetupCount ->
  DiagCount -> Init -> Start -> CheckTemp -> Stop -> Reset -> GetDiag; record
  the LabVIEW version, architecture, parser settings, DLL hash and result.
- LabVIEW RT: the same wrapper VIs against `linux-x64\libtempctl.so` on the
  cRIO and `linux-armhf\libtempctl.so` on the myRIO-1900, including the
  lifecycle / permissive sequences; record target model, image version,
  LabVIEW RT version, ABI, hashes and results.
