# Testing Guide - TempCtl v3

Release gate for v3.0.0: `build.bat all` must finish with
`TempCtl 3.0.0 unit tests: 1516 passed, 0 failed` for both the x64 and the
x86 test executables and build both Linux targets; `tests\oracle_test.py`
must print `ALL OK`. `package_dist.bat` re-runs the Windows gates, checks
that `dbc\tempctl.dbc` matches the generator, runs the oracle, appends the
Raspberry Pi log from `docs\testlogs\` and records everything in
`TESTLOG.txt`; it refuses to package a failing build. The simulator has its
own gate (`build_sim.bat` / `package_sim.bat`): the 16 scenarios with all of
their expectations passing and zero unpack mismatches, plus the WPF
screenshot run; its log is in the TempSim package.

## What the gates cover

### `tests\test_main.c` (1516 checks, compiled with `src\tempctl.c`)

One test function per rule group of `TEMPCTL-SPEC-v3.0.0.md`; the change
list rows (C1-C22) and required scenarios (S1-S15) of the v3.0.0 handoff
are named in the comments.

- **API (C1):** version and counts, every argument error (-1, -2) for every
  function, outputs untouched on a negative return, longer buffers accepted.
- **R2 enable / lifecycle (C3, S12):** uninitialised zone inert and
  `TC_OK`; `Enable = 0` inert under out-of-range, NaN, below-band and
  feedback stimuli (no warning, no fault, no accumulation, no averaging);
  the 0.1 boolean threshold; power-on state.
- **R3 units (C4):** identical trajectories with units 0 and 1 over a
  profile that exercises both relays; units 2, 0.5, NaN -> ConfigFault.
- **R4 filter (C9):** every invalid FilterPoints value becomes 4 without a
  fault or warning; 64, 2.9, 1, 64.9 handled; control on the raw value with
  a 64-point average; the moving-average arithmetic; out-of-range and NaN
  samples never averaged; Init and Reset clear the averages; sensor 2's
  average uses the corrected value; NaN fields with Temp2 disabled.
- **R4.4 NaN and chatter (C10, S4):** single NaN, +/-Inf and spikes in idle,
  pending, heating and cooling change no relay and corrupt no average; NaN,
  +Inf, -Inf and over-limit streams fail high, under-limit fails low; NaN on
  a disabled sensor 2 ignored.
- **R5.4 accumulator (DRAIN 0.5), R5.6 events (C11, S5):** one-tick glitch
  charges 100 and drains 50 + 50; sparse glitches never fail and each is
  counted; a sustained excursion is one event and drains at half rate; the
  Amendment A table with ErrorTimeout 1000: 33 % duty nets zero and never
  fails, 25 % never fails, 50 % fails on tick 37 (3.7 s, about 4 x), 75 %
  fails on tick 15 (1.5 s, about 1.6 x), 100 % fails at exactly 1000 ms;
  an odd 101 ms period keeps the half-ms accumulator exact (101, 50.5, 0);
  an ErrorTimeout of one loop period fails a sensor from one sample (the
  documented host floor); the direction at the moment of failure; 60-minute
  ring expiry with partial and full expiry; Init/Reset clear the counts;
  sensor 2's own accumulator and counter.
- **R5.5 failover (C12, S7):** sensor 1 fails with sensor 2 healthy -> no
  fault, control on corrected sensor 2, warning 1 masking 6, RunningOnTemp2
  when back in range, accumulator frozen; excursions on the failed sensor;
  then sensor 2 fails -> BothSensorsFailed, latched, warning frozen; sensor
  2 failing first (no switch, warning while out of range); both on one tick;
  single-sensor faults 10/11; never re-admitted; Init and Reset restore
  sensor 1.
- **R5.7 pause (C13, S6):** at-setpoint and deadband countdowns freeze
  through an excursion (limit values and NaN) and resume where they stopped;
  idle holds; the non-active sensor's excursion does not touch control;
  pause ending in a fault and in a failover with the relay kept.
- **R6.1 offset (C5):** limit check, comparison and post-switch control on
  the corrected value, positive and negative offsets, boundary at HiLimit.
- **R6.3-R6.6 disagreement (C6, C7, S8):** observed / warning at T/10 /
  fault at T tick by tick with `CompareRemainMs`; recovery clears at once
  and restarts from zero; both directions; exactly the tolerance agrees;
  T/10 = 0; control continues while it runs.
- **R6.2 / R6.5 gating (S9):** not before Initial_HC_Flag (and first on the
  tick after it), not during an excursion (restart from zero), not before
  both averages are full (out-of-range samples do not fill them), not once
  a sensor failed, not with Temp2 disabled.
- **R7 control (C8, S1, S3):** the complete heat-up with every countdown
  value; a single sample at the setpoint never drops the relay; breaking
  the at-setpoint condition resets it; re-entering the band clears the
  deadband countdown, crossing to the other side restarts it; inclusive
  band edges; cool-down and release; overshoot on the release tick starts
  the opposite countdown; never both relays; deadband countdown idle while
  a relay is on; setpoint change by Init moves the bands.
- **R7.5 Initial_HC_Flag (C14):** in-band start (tick 1, band edge),
  out-of-band start (set on release, not while heating or pending), cooling
  completion, cleared by Init and Reset, sticky afterwards.
- **R8 feedback (C15, S10):** honest relays never warn; 1-tick mismatch
  warns only; sustained mismatch faults that relay; heater and cooler
  independent, heater first on the same tick; stuck-open heater while
  heating; a one-tick DO-loop lag warns once per transition; FeedbackEnable
  = 0 silent; no checking while stopped or disabled; previous command after
  Reset (0) and after a relay-keeping re-Init (kept); non-zero read-backs.
- **R9.5 config (C16, S11):** 31 invalid setups each with Enable = 1
  (ConfigFault, inert, Reset does not clear, passing Init does) and Enable =
  0 (ConfigInvalid, cleared by the next passing Init); both deadbands zero;
  one zero deadband allowed; NaN enable; disabled-feature parameters not
  checked; 1 ms timeouts accepted and still need a later tick; a failing
  Init while running stops the zone; huge timeouts saturate.
- **R9.1-R9.3 Init / Reset (C17, C18, S2, S13):** setpoint change by re-Init
  keeps the heater and restarts the at-setpoint countdown; the new logic
  may drop it; cooler kept; Enable = 0, a fault, a previously disabled zone
  and power-up all start with relays off; Init clears everything; Reset
  keeps the setup and clears history, relays off; fault again after the
  full timeout; Reset drops a running relay; new time reference.
- **R9.6 fault behaviour (C19):** warning frozen at the fault tick's value,
  diagnostics frozen, first fault wins for sensor-vs-feedback,
  disagreement-vs-feedback and heater-vs-cooler, no later overwrite, mirrors
  equal the outputs.
- **R1.4 / R1.5 time (C21, S15):** 2^32 wrap during a countdown, the
  accumulator and the hourly ring; backwards steps of 50 s and 5 ms count as
  0 ms; the same call time twice; long gaps; the largest forward step
  (0x7FFFFFFF) counts, one more is backwards.
- **S14 two zones:** two zones with different setups stepped alternately
  produce exactly the traces of running each alone; a Reset on one leaves
  the other untouched; all 16 zones usable.
- **C22 diagnostics:** repeated `TcGetDiag` calls change nothing (two
  identical runs, one with three diag calls per tick, compared field by
  field); every index checked against a documented value at a known point;
  the buffer beyond 25 untouched; Init/Reset mirrors before the first
  CheckTemp.

### `tests\oracle_test.py` (independent implementations)

Loads `tempctl.dll` and the vendored `cantp.dll` through ctypes and runs a
scripted scenario (heat-up on a toy plant, at-setpoint release, sensor 1
open with failover to sensor 2, reset, relay feedback fault, single-sensor
NaN stream, Temp2 disabled). After every call the 25-value diagnostics
array is packed by `CanTp_Pack`, the BAM is reassembled by the script's own
TP.CM/TP.DT parser, and the payload is compared bit-for-bit with cantools
encoding the same values with `dbc\tempctl.dbc` (pad bits masked; NaN
signals checked for the all-ones pattern). cantools' decode of CanTp's
payload must equal `CanTp_Unpack`, and the diagnostics mirrors must equal
the call outputs. 1287 arrays.

```bat
pip install cantools
python tests\oracle_test.py [--tempctl build\win-x64\tempctl.dll] [--cantp third_party\cantp\cantp.dll] [--pylibs DIR]
```

### `tools\make_tempctl_dbc.py`

Verifies the DBC signal order against the `TC_DIAG_*` defines in
`tempctl.h`, loads the written DBC with cantools (strict), checks the frame
id, length, `VFrameFormat`, order and that no two signals overlap.
`package_dist.bat` regenerates the DBC and fails if `dbc\tempctl.dbc`
differs.

### Simulator gates (TempSim package)

`TempSim.Cli --scenario all`: the 15 scenarios of the handoff plus
`flicker-25` (both sides of the one-third duty boundary, Amendment A) with
110 built-in expectations (status, warning, relays, countdown and
accumulator values at exact ticks), every tick packed by `CanTp_Pack` and
read back by `CanTp_Unpack`; exit 1 on any failed expectation or unpack
mismatch.
`TempSim.exe --screenshot`: loads the failover scenario, runs 75 s, renders
the window, exits 0 only with all expectations met. Both run against the
same `tempctl` binaries this package ships.

### On Linux

`linux-arm64\test_tempctl` was run on a Raspberry Pi 5 (aarch64):
`1516 passed, 0 failed` (`docs\testlogs\pi-test_tempctl-2026-09-18.txt`,
appended to `TESTLOG.txt`). The `linux-arm64` simulator ran all 16 scenarios
with 110/110 expectations and produced CSV and `.ncl` files byte-identical
to the Windows run (TempSim package, `testlogs\pi-sim-2026-09-18.txt`).

`linux-x64\test_tempctl` is the same program for the cRIO; run it once on
the target (`LABVIEW_INTEGRATION.md` section 2). The `.so` files are
inspected at package time with `tools\elfinfo.py` (`DEPENDENCIES.txt`): the
cRIO library imports only `memset` from libc.
