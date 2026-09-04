# Testing Guide — TempCtl v2

Release gate for v2.0.0: `build.bat all` must finish with `181 passed, 0
failed` for **both** the x64 and the x86 test executables and build both
Linux targets; `tests\oracle_test.py` must print `ALL OK`; `build_sim.bat
all` must end with the simulator CLI reporting `unpack mismatches 0` for
every scenario and the WPF screenshot run exiting 0. `package_dist.bat`
re-runs the Windows gates, checks that `dbc\tempctl.dbc` matches the
generator, and records everything in `TESTLOG.txt`; it refuses to package a
failing build.

## What the gates cover

### `tests\test_main.c` (181 checks, compiled with `src\tempctl.c`)

- basic control with a 1-point filter (v1-equivalent): deadband countdown →
  heat, relay held until setpoint, deadband → cool, countdown values and
  TempStatus at every stage
- deadband offsets: bands follow a live setpoint change; countdown restarts
  when the condition clears
- limit fault: countdown, latched T1_HI bit, Stopped, no change while
  stopped (filters still update), Reset, LoLimit path, countdown from full
  after Reset with the condition still present
- fault while heating; overshoot past the far limit just ends the cycle
- bad readings: relays off at once, NaN not averaged, T1_BAD after
  ErrorTimeout, never-valid sensor (filtered NaN) counts as bad
- initial relay state from Init (heating wins), input relay fields ignored
  on Step, live setpoint change
- zero timeouts act on first observation; 32-bit tick wrap
- argument errors −1..−4, every configuration warning (negative deadband,
  band vs limit, negative timeout/tolerance, FilterPoints > 64 clamped, 0 =
  default), `TcVersion`/`TcInputCount`/`TcSignalCount`, in-place `in == out`
- filter: warm-up status, running average, NaN skipped, live change of
  FilterPoints, 64-sample ring wrap, control on the filtered value not the raw
- failover: T1 hi → failed, ActiveSensor 2, Degraded, cooling continues on
  the new sensor, control follows sensor 2, T1 stays failed when back in
  limits, T2 fails too → Stopped, Reset restores sensor 1; backup failing
  while the primary is fine (Degraded, no switch), then the primary fails
  (Stopped); Temp2 disabled ignores sensor 2 entirely; disabling Temp2 while
  on sensor 2 falls back to sensor 1
- disagreement: within tolerance, pending, cleared in time, latched bit with
  control continuing on sensor 1, later limit failure still fails over
- feedback: mismatch countdown, relay answering in time, relay dropping out
  while commanded (bit set, heating continues), latched, cooler feedback
  stuck on, FeedbackEnable = 0 ignores the inputs, Init state counts as the
  first command
- ErrorRemainMs is the minimum of the running countdowns; Degraded with a
  feedback bit

### `tests\oracle_test.py` (independent implementations)

Loads `tempctl.dll` and the vendored `cantp.dll` through ctypes and runs a
scripted 130 s scenario (warm-up, heating, sensor 1 open with failover,
reset, heater feedback fault, Temp2 disabled). For **every tick** the 27
outputs are packed by `CanTp_PackSgl`, the BAM is reassembled by the
script's own TP.CM/TP.DT parser, and the payload is compared bit-for-bit
with **cantools** encoding the same values with `dbc\tempctl.dbc` (pad bits
masked; NaN signals checked for the all-ones pattern). cantools' decode of
CanTp's payload must equal `CanTp_Unpack`. 1262 ticks.

```bat
pip install cantools
python tests\oracle_test.py [--tempctl build\win-x64\tempctl.dll] [--cantp third_party\cantp\cantp.dll] [--pylibs DIR]
```

### `tools\make_tempctl_dbc.py`

Verifies the DBC signal order against `enum TcSignal` in `tempctl.h`, loads
the written DBC with cantools (strict), checks the frame id, length,
`VFrameFormat`, order and that no two signals overlap. `package_dist.bat`
regenerates the DBC and fails if `dbc\tempctl.dbc` differs.

### Simulator gates (`build_sim.bat`)

`TempSim.Cli --scenario all`: seven scenarios, 1200 ticks each, every tick
packed and unpacked; exit 1 on any unpack mismatch. `TempSim.exe
--screenshot`: loads the failover scenario, runs 70 s, renders the window.

### On Linux

`linux-arm64\test_tempctl` was run on a Raspberry Pi 5: `181 passed, 0
failed`. The `linux-arm64\TempSim.Cli` scenarios produced CSV and `.ncl`
files byte-identical to the Windows run
(`docs\testlogs\pi-vs-windows-2026-09-04.txt`), and the live SocketCAN loop
between the two bench Pis reassembled every message
(`docs\testlogs\pi-socketcan-loop-2026-09-04.txt`).

`linux-x64\test_tempctl` is the same program for the cRIO; run it once on
the target (see `LABVIEW_INTEGRATION.md` §2). The `.so` files are inspected
at package time with `tools\elfinfo.py`; the result is in `DEPENDENCIES.txt`.
