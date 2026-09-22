# TempSim - the TempCtl v4 + CanTp simulator

Two programs built on one core (`TempSim.Core`, .NET 10), both driving the
**real** `tempctl` and `cantp` libraries through P/Invoke; nothing is
re-implemented in C#. Every tick: plant (or a scripted temperature profile)
-> sensors -> `TcCheckTemp` (with the live run permissive) -> relays ->
plant, then `TcGetDiag` reads the 28 diagnostics, `CanTp_Pack` turns them
into the 8 NI-XNET raw frame records of one 44-byte J1939 BAM, and
`CanTp_Unpack` reads them back as the receive-side proof (the "unpack
mismatches" counter must stay 0).

TempSim is released on its own version line (`CHANGELOG.md` here), one
package per target. The TempCtl controller package and the CanTp package
are separate downloads; TempSim embeds the `tempctl` and `cantp` binaries it
was built with and prints their versions at start-up. **TempSim 3.x drives
TempCtl 4.x only**: a library with another major version, or a table with
other than 28 signals, is refused with a clear message.

## Files in this package

| File | Purpose |
|---|---|
| `TempSim\TempSim.exe` (win-x64 package) | Interactive Windows simulator: graph, Start / Stop / Run permissive, status / warning / lifecycle line, eight countdown and accumulator bars, lamps, live setup (every change is Stop -> `TcInit` -> Start), fault injection, frames panel |
| `TempSim\TempSim.Cli.exe` / `TempSim\TempSim.Cli` | The 29 scripted scenarios with built-in expectations -> CSV + `.ncl`; on Linux also SocketCAN transmit / receive |
| `TempSim\tempctl.dll` / `libtempctl.so`, `cantp.dll` / `libcantp.so` | The controller and transport binaries the simulator drives |
| `TempSim\TempCtl.json`, `TempSim\tempctl.ecd` | The CanTp definition of the diagnostics message, as a dbc2tables table and as the generated ECD database; both are checked against each other at start-up |
| `examples\tempsim-screenshot.png` | The Windows simulator after 75 s of the `failover` scenario (the release gate's screenshot) |
| `examples\failover.csv`, `.ncl` | That scenario's trace and NI-XNET log (open the `.ncl` in NI-XNET Bus Monitor) |
| `TESTLOG.txt` | The gate run (every scenario with its expectations, the screenshot run) and the Raspberry Pi logs |
| `testlogs\` | Raw logs from the Raspberry Pi bench: Pi-vs-Windows byte comparison, live SocketCAN loop, Pi scenario runs |
| `MANIFEST.txt`, `CHANGELOG.md`, `LICENSE.txt` | SHA-256 of every file, release history, MIT |
| `src\` | The simulator's C# source, `Directory.Build.props` (version), `NativeAssets.targets`, `build_sim.bat` |

Both programs are self-contained (bundled .NET runtime; nothing to
install). The native libraries, `TempCtl.json` and `tempctl.ecd` sit next to
the executable; `--native-dir DIR` (CLI) / `TEMPSIM_NATIVE_DIR` points at
other builds of the libraries.

## 0. The v4 lifecycle in the simulator

TempCtl v4 separates three things the host used to express with
`TempCtrlEnable` alone: the master enable (setup index 0), the operator's
Start / Stop, and the live **run permissive** the host computes from its
own operating conditions. The simulator models the host:

| Simulator action | Library calls | Result |
|---|---|---|
| fresh run (`Restart`, scenario start) | `TcReset`, `TcInit`, then `TcStart` when `StartOnInit` (default true) | control from t = 0, as a host does at power-up; a scenario may set `StartOnInit = false` and Start later |
| **Start** | `TcStart(runPermissive)` | accepted (provisional TempAtSetPt, relays off until the first tick decides) or refused as IdleStartBlocked with warning 8 |
| **Stop** | `TcStop` | both commands 0 at once, the modelled relays written off, IdleStopped (or the trip cause kept), no fault |
| **Run permissive** off while running | next `TcCheckTemp(..., 0)` | both relays off on that sample, OperatingConditionPending; back on before `OperatingConditionTimeout`: IdleOperatingConditionTripped (no restart); held: OperatingConditionFault |
| setup edit / scenario `ReInit` | `TcStop` -> zero DOs applied -> `TcInit` -> `TcStart` when the operator's intent is "running" | the host sequence the handoff requires; `ReInit(restart: false)` leaves the zone IdleStopped |
| **Reset fault** / scenario `Reset` | `TcStop` -> zero DOs applied -> `TcReset` -> `TcStart` when running | faults cleared, history cleared, running again only because the intent was; `Reset(restart: false)` leaves it stopped |

Nothing restarts control except Start. The operator's intent ("running")
is set by Start and by a StartOnInit run, cleared by Stop.

## 1. TempSim.exe (Windows)

The dark blue thermal test bench shows the temperature chart and animated
fixture together. It starts in **fixture physics** mode; the **Run fixture**
button returns to that mode after a scripted controller scenario.

- **Toolbar:** Pause / Run, **Start**, **Stop**, the **Run permissive**
  check box (the live input of every tick), Reset fault, Restart, speed
  1x..20x, fixture configuration, motors, scenario picker.
- **Graph:** Temp1 (orange), Temp2 (cyan), control temperature (white), plant
  (grey), setpoint and high/low deadband (green), signal max/min (salmon).
  Signal max/min mean the configured controller validity limits, HiLimit and
  LoLimit, not observed extrema. All lines retain their history; the chart
  shows the last 120 simulated seconds. Heat/cool lanes show relay commands.
- **Inlet/outlet:** supply inlet (mint), UUT outlet (purple), and signed ΔT = outlet minus inlet. Both controller probes measure the outlet, with independent sensor errors. The inlet/outlet channels are physical model values, separate from the redundant controller inputs.
- **Fixture:** one to three motors, a circulating oil pump, inline oil heater,
  unit under test, and an external fan. Oil particles move through the supply,
  UUT and return. Motor, pump and fan rotation follows simulated motion;
  rotation is slowed for visibility and is not a tachometer. Heater outline
  shows physical relay state; delivered heat is also displayed.
- **Time:** Run/Pause, restart, and 1x, 2x, 3x, 5x, 10x, 20x. Elapsed wall time
  schedules fixed controller ticks without changing the physics step. Pause
  freezes physics and animation. Under CPU overload the simulation catches up
  in bounded batches; the displayed simulated time is authoritative.
- **Live status:** controller status (grey when stopped, orange when Start
  is blocked or a permissive trip is latched, orange-red while the
  operating-condition countdown runs, red on a fault), warning, the
  lifecycle line (Started, live and last-evaluated permissive, condition-fault
  countdown), the eight countdown / accumulator bars including the
  operating-condition fault delay, command and feedback lamps, active
  sensor, raw/averaged temperatures.
- **Configuration tab:** all 18 controller setup values (including
  `OperatingConditionTimeout`), ambient temperature, legacy plant rates,
  sensor offset/noise/lag and fault injection, relay delay/stuck faults, and
  CSV/NI-XNET logging. A controller change is Stop -> `TcInit` -> Start
  (when running). Numeric inputs use a decimal point; Enter or leaving the
  field applies them.
- **Fixture configuration:** a separate scrollable screen for every fixture
  physics parameter. Edit values and select **Apply physics**. Invalid values
  show an explanation without changing the running model. The modal screen
  pauses time while editing and restores the previous Run/Pause state.
- **CAN data:** raw J1939 records (8 per tick) and all 28 decoded
  diagnostics, with scrolling.

### Estimated physical model

These are first-pass assumptions, not measured bench calibration. Two thermal
masses represent the supply oil (UUT inlet) and the oil held inside the UUT
plus its fixture mass (UUT outlet). Each mass is internally well mixed.
Actuation deposits motor heat in the UUT; recirculating oil carries that heat
back to the supply. Both redundant controller probes measure the outlet with
independently configurable offset, lag and noise. Local hot spots and pipe
transport delays are not modeled.
The requested inline component is interpreted as an **inline oil heater**.

| Parameter | Starting estimate |
|---|---|
| Motors | 2, commanded running; configurable count 1–3 |
| Each motor | 1.5 kW rated, 1800 / 3000 rpm, 60% load, 25% heat fraction |
| Motor response | 2 s |
| Oil pump | Running, 12 L/min, 1 s response |
| Inline heater | 6 kW; 4 L/min gives 50% transfer |
| Oil | 8 L total, including 0.5 L inside UUT; 0.85 kg/L, 2000 J/(kg·K) |
| Fixture | 12 kg, 500 J/(kg·K) |
| Passive heat loss | 8 W/K to ambient |
| Fan | Automatic from cooler relay; 1600 / 2000 rpm |
| Fan cooling / response | 120 W/K at rated speed; 1.5 s response |

Supply heat capacity uses total oil volume minus UUT hold-up volume. Outlet
heat capacity combines the UUT oil and fixture. Actuation heating scales with
motor count, rated power, load, heat fraction and actual/rated speed. Heater
delivery to the supply is rated power times flow / (flow + transfer-flow
parameter). With zero flow, no heater energy is delivered, but actuation can
continue heating the UUT. Passive and fan losses act on the UUT/outlet mass.

Recirculation transfers heat at mass-flow rate times oil specific heat times
the inlet/outlet difference. A coupled implicit update conserves internal heat
transfer and remains stable at zero or high flow. The plant channel is the
energy-weighted mean of both temperatures. Substeps are at most 50 ms.

Actuation can make the outlet warmer than the inlet; more flow generally
reduces the difference. During heater warm-up the inlet can instead be hotter.
The signed ΔT retains both cases. The UUT oil hold-up, total oil volume,
fixture mass, motor heat fraction, flow and fan losses are configurable.
The motors and oil pump are independent operator commands; TempCtl still
commands only heater and cooler. A controller fault or Stop drops its
commands but is not a complete machine shutdown interlock. Manual fan mode
is available. Changing TempUnits reinterprets numeric temperatures as F or
C; it does not convert existing setpoints, limits or sensor offsets. SI
thermal calculations account for the selected unit's temperature scale.

Fixture edits preserve temperature and current speeds. Entering fixture mode
from a controller scenario cancels its timed events/profiles and starts fresh
controller state at the current temperature. **Run fixture** and **Restart**
start from the configured initial plant temperature. Built-in scenarios always
use the original plant model.

Settings persist in `%LOCALAPPDATA%\TempSim\settings.json`; window position
is in `window.json`. A 2.x settings file loads (the new `StartOnInit`,
`RunPermissive` and `OperatingConditionTimeoutMs` take their defaults).
The CLI accepts `--scenario fixture --config FILE` for a custom fixture run;
built-in controller scenarios deliberately disable fixture physics.

CSV logging also writes `NAME.fixture.csv` when fixture mode is active: time, enabled flag, temperature units (0=F, 1=C), inlet, outlet, signed ΔT, oil flow, actuation heat, heater heat and cooling power. These additional physical channels do not alter the controller diagnostics or CAN layout.

Headless rendering: `TempSim.exe --screenshot out.png --scenario fixture
--seconds 60`. Use `--scenario failover --seconds 75` for the controller gate.
Optional `--view physics`, `--view configuration`, or `--view can` captures
those screens. The render check uses a fixed window size, ignores saved user
settings, writes `out.perf.txt`, and leaves saved settings untouched.

## 2. TempSim.Cli

```
TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N] [--every S]
            [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE.json|FILE.ecd]
TempSim.Cli --list
TempSim.Cli --rx IFACE [--seconds N]         (Linux)
```

At start-up it prints the library versions, the message table in use and
the `tempctl.ecd` cross-check (channel names, bit positions, scaling and
limits compared with `TempCtl.json`; a stale or reordered ECD is an error,
exit code 2). `--table tempctl.ecd` defines the CanTp slot from the ECD with
`CanTp_DefineFlat` instead and verifies the rows CanTp derived.

Per scenario it writes `DIR/NAME.csv` and `DIR/NAME.ncl` (and
`NAME.zone1.csv` for the two-zone scenarios), prints a state line every
`--every` seconds (default 5; `--every 0` prints every tick: status,
warning, relay commands and physical states, active sensor, the five
countdowns, both accumulators, the event counts, the flag, the permissive,
the operating-condition countdown and Started), the events as they fire and
each expectation with ok / FAIL, and ends with the tick count, frame count,
unpack mismatches, expectations passed and the final status and warning.
Exit code 1 if any scenario had an unpack mismatch or a failed expectation.

### Scenarios

The 15 required by the v3.0.0 handoff (section 10) run under the v4
lifecycle (Start at t = 0); S2 became `reconfigure-stop-init` because a
re-Init no longer keeps a relay. `flicker-25` is Amendment A. The last 14
are the required scenarios of the v4.0.0 handoff, section 14.

| Name | What happens and what is checked |
|---|---|
| `heat-up` | S1 Single sensor, Start at t = 0: HeatPending on tick 1 with 500 ms remaining, HeaterON at 0.6 s, released after AtSetPtTimeout, TempAtSetPt, flag set, no fault through the cycling |
| `reconfigure-stop-init` | S2 / V4-11 Setpoint 70 at 3 s while heating through Stop -> zero DOs -> TcInit: heater dropped, physical relay off, IdleStopped, still idle at 4.9 s; Start at 5 s: HeatPending -> HeaterON -> 70 reached; setpoint 40 at 40 s with the automatic re-Start: CoolPending, CoolerON |
| `cool-down` | S3 Start at 70: CoolPending, CoolerON at 0.6 s, release at the setpoint |
| `chatter` | S4 Profile-driven: one-sample NaN, 200 spikes and setpoint spikes while heating, idle and cooling change no relay; averages hold only in-range samples; four events counted |
| `flicker` | S5 Profile-driven leaky accumulator (drain 0.5): sparse glitches charge one tick and drain in two (3 events, no failure); 75 % duty fails after 3.0 s (about 1.6 x ErrorTimeout); after a reset a 50 % duty fails after 7.6 s (about 4 x ErrorTimeout, 39 events) |
| `flicker-25` | S5b Below the one-third boundary: 33 % duty (1 out, 2 in) nets zero, 25 % duty drains to zero every cycle; never above one tick, never fails, 108 events counted |
| `control-pause` | S6 Heating with the at-setpoint countdown at 400 ms, sensor at 200 for 0.8 s: relay held, countdown frozen at 400, accumulator 800, then resumes and releases at 3.3 s |
| `failover` | S7 Sensor 1 opens at 40 s: warning 1, failover on the 20th tick with the accumulator at 2000, no fault; sensor back at 50 s: RunningOnTemp2, control on corrected sensor 2; sensor 2 stuck at 200 at 70 s: BothSensorsFailed; reset (+ Start) at 90 s |
| `disagree` | S8 Sensor 2 +8 at 40 s: observed at 40.1, warning at 40.6 (T/10), cleared at 43.2 after agreement; drift again at 50 s: TempDisagreeFault at 55.1, latched |
| `compare-gating` | S9 Profile-driven, sensors 10 apart from the start: no comparison while heating, flag at release, observed the tick after, warning at 200 ms; an excursion resets it, restart from zero; fault after the full 2000 ms |
| `relay-feedback` | S10 Heater DO one tick late: HeaterFBMismatch for one tick per transition, no fault; cooler stuck closed at 30 s: warning at 30.1, CoolerFBFault at 31.1; reset at 40 s; FeedbackEnable 0 at 50 s (Stop -> Init -> Start) silences a stuck cooler |
| `config-fault` | S11 TcInit with DeadbandHi -1 while running: ConfigFault, relays off, Start refused; Reset keeps it; TcInit with Enable 0: ConfigInvalid; valid TcInit + Start runs again |
| `enable-disable` | S12 Zone disabled: open sensor, stuck sensor, stuck relay and a Start leave status 0, no warning, nothing accumulated, not started; TcInit Enable 1 + Start at 30 s starts control |
| `reset` | S13 Sensor opens at 10 s: Temp1FailHigh at 11.9 s; reset (+ Start) at 20 s with the sensor still open clears everything and faults again at 21.9 s; sensor back, reset, clean |
| `two-zones` | S14 Zone 0 (two sensors, setpoint 50) and zone 1 (one sensor, setpoint 80, DeadbandTimeout 1000) in the same loop: own bands and timing, zone 0's failover leaves zone 1 untouched, zone 1's trace equals a solo run |
| `time` | S15 Tick Count starting 300 ms before 2^32 wraps during the first countdown; a 60 s backwards step at 2.3 s leaves the at-setpoint countdown at 300 ms; a 600 s gap at 8.2 s completes the pending countdown |
| `idle-before-start` | V4-1 Enabled Init only (`StartOnInit` false): IdleStopped, relays off, raw reading mirrored, no averaging or accumulation for 10 s with a reading below the band; Start at 10 s: HeatPending with the full 500 ms, HeaterON |
| `start-heat-cool` | V4-2 Idle until Start at 2 s: HeatPending (observed on the Start tick), HeaterON at 2.5 s, TempAtSetPt; setpoint 30 at 40 s through Stop -> Init -> Start: CoolPending, CoolerON |
| `stop-from-active` | V4-3 Stop from HeatPending, HeaterON, CoolPending and CoolerON: both commands 0 at once, modelled relays off, countdowns cancelled, IdleStopped, warning none, no restart without Start |
| `blocked-start` | V4-4 Start with the permissive false: IdleStartBlocked, warning 8, no countdown for 4 s, no fault; permissive back at 6 s: warning clears live, status 7 stays, nothing starts; Start at 8 s runs |
| `permissive-trip` | V4-5 Heating; permissive false for one tick at 3.0 s: relays off on that sample, OperatingConditionPending with 3000 ms, Started 0, warning 8; true at 3.1 s: IdleOperatingConditionTripped, countdown 0, warning clears; still tripped at 5.9 s; Start at 6 s: HeatPending, HeaterON |
| `permissive-recover` | V4-6 Permissive lost at 3.0 s: 200 ms left at 5.8 s, 100 at 5.9 s; back at 6.0 s (the tick that would have faulted): tripped, no fault; no restart until Start at 10 s |
| `permissive-fault` | V4-7 Permissive lost and held: OperatingConditionFault exactly at 6.0 s with warning 8 frozen; permissive true at 8 s and Start at 9 s change nothing; Reset (+ Start) at 10 s runs again |
| `stop-while-pending` | V4-8 Permissive lost at 3 s, Stop at 4 s: countdown cancelled, IdleOperatingConditionTripped kept, warning 8 while the permissive is false, no fault at 7 s; permissive true at 8 s clears the warning; Start at 9 s |
| `permissive-fault-priority` | V4-9 Sensor open from 3.0 s fails at 4.9 s (ErrorTimeout 2000); the permissive drops on that same tick: Temp1FailHigh wins, no condition countdown; later permissive changes and a Start change nothing |
| `reset-stays-idle` | V4-10 Sensor fault; Reset at 8 s without a re-Start: IdleStopped, clean; four seconds of CheckTemp below the band change nothing (raw mirrored, no countdown); Start at 12 s: HeatPending, HeaterON |
| `reset-stop-first` | V4-12 Heating; Reset at 3 s through Stop -> zero DOs -> TcReset without a re-Start: heater and relay off, averages and flag cleared, IdleStopped through 5.9 s; Start at 6 s |
| `start-while-pending` | V4-13 Permissive lost at 3 s; Start with it false at 4 s: pending kept, countdown continues (2000 left); permissive true + Start at 5 s: HeatPending, countdown 0; second loss at 8 s, recovery at 8.5 s (tripped), Start at 9 s accepted |
| `two-zones-lifecycle` | V4-14 Zone 0 loses its permissive at 5 s (pending, OperatingConditionFault at 8 s) while zone 1 keeps heating; zone 1 Stop at 6 s (IdleStopped) and Start at 10 s (HeatPending, HeaterON); zone 0 permissive back at 15 s (fault latched), Reset + Start at 20 s: both running |

### Scripted temperature profiles

A config's `Profile` is a list of `{AtSeconds, Temp1, Temp2}` points. When
present, the sensors read the profile instead of the plant (step-hold: a
point applies from its time until the next; `NaN` is an open sensor); sensor
offset, noise, lag and faults still apply on top. `StartTickMs` sets the
value of the millisecond tick at t = 0 (near 2^32 to test the wrap), and a
scenario event can shift the clock (backwards steps, long gaps). `Companion`
is a second config for zone 1 stepped in the same loop. `StartOnInit` and
`RunPermissive` set the lifecycle at t = 0; scenario events call
`Start()`, `Stop()`, `Reset(restart)`, `ReInit(edit, restart)` and set
`RunPermissive`.

### Determinism, the cross-platform proof

The plant, sensors and noise use only `+ - * /` in IEEE double and a
xorshift generator, the controller is IEEE double in C, and the CSV is
written with fixed rounding and invariant culture. The same scenario
therefore produces **byte-identical CSV and .ncl files on Windows x64 and on
the Raspberry Pi** (`testlogs\pi-sim-2026-09-22.txt`: 29 scenarios, 208/208
expectations, every file identical). Run `--scenario all` on a new target
and `cmp` the files against `examples\` or a Windows run to prove the
libraries behave the same there.

### CSV columns

`t_s, plant, temp1, temp2, ctrl, t1avg, t2avg, do_heat, do_cool, heater,
cooler, status, warning, active, db_rem_ms, asp_rem_ms, cmp_rem_ms,
hfb_rem_ms, cfb_rem_ms, t1_accum_ms, t2_accum_ms, t1_events, t2_events,
hc_flag, hi_band, lo_band, run_perm, oc_rem_ms, started, rc, payload`

`heater`/`cooler` are the physical relay states (after stuck/delay
modelling), everything from `ctrl` to `started` comes from `TcGetDiag`
(`status`/`warning` are the `TcCheckTemp` outputs, which the mirrors equal;
`run_perm` is the last permissive the controller evaluated, `NaN` after
Init / Reset; `oc_rem_ms` the operating-condition countdown; `started` the
ControllerStarted flag), `rc` the last return value, `payload` the 44-byte
message as hex.

### Config file (`--config`)

JSON with `PeriodMs`, `Seconds`, `Seed`, `StartTickMs`, `StartOnInit`,
`RunPermissive`, `Plant` (Ambient, Initial, LagPerSec, HeatRate, CoolRate),
`Sensor1`/`Sensor2` (Offset, NoiseAmplitude, LagPerSec), `Heater`/`Cooler`
(DelayTicks, StuckOpen, StuckClosed), `Controller` (the 18 setup values by
name, `OperatingConditionTimeoutMs` last), `Can` (SourceAddress, SpacingMs,
Interface), `Profile` and `Companion`. The WPF app's
`%LOCALAPPDATA%\TempSim\settings.json` has the same shape and can be passed
to the CLI. Scenarios apply their own overrides on top of the file.

### Linux CAN (SocketCAN)

`--can can1` also transmits every frame on the interface (the 8 records of a
tick are written back-to-back; add `--realtime` to pace ticks). `--rx can0`
listens, feeds every received frame to `CanTp_RxFeed` and prints each
completed TempCtl message by signal name. Bring the interface up first, e.g.
`sudo ip link set can0 up type can bitrate 250000`. A v3 receiver (55-byte
layout) cannot decode v4 frames: the U16 timers move every later signal.

## 3. Rebuilding

In the `temp-controller-dll` repository (the source under `src\` here is
`sim\` there):

```bat
build.bat all                                   :: tempctl binaries (needed by the sim)
python tools\make_tempctl_dbc.py --tables --ecd :: dbc\tempctl.dbc + dbc\tables + dbc\tempctl.ecd (needs cantools)
build_sim.bat all                               :: publishes build\sim\{win-x64,linux-x64,linux-arm64} + headless checks
package_sim.bat X.Y.Z [password] ["rids"]       :: dist\TempSim_vX.Y.Z_<rid>{,.zip,_unencrypted.zip}
```

`sim\NativeAssets.targets` copies the right `tempctl` (from `build\<rid>`)
and vendored `cantp` (from `third_party\cantp`) binaries per
RuntimeIdentifier; the two project files copy `TempCtl.json` and
`tempctl.ecd`; `sim\Directory.Build.props` holds TempSim's version.
`dotnet run --project sim\TempSim.Cli` works for a quick Windows x64 run.

### Status display and motor controls (since 2.2.1)

Timer tracks show explicit Idle, Disabled, Timing or Fault snapshot states, with milliseconds and configured thresholds. Active countdown bars drain; sensor invalid-reading bars fill and recover. The drive symbols are finned motor housings with rotating shafts. Start/Stop motors on the toolbar controls their shared run command independently of the heater and external fan. Motor speed coasts down after Stop; Pause freezes the entire simulation.
