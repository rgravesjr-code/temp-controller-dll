# TempSim - the TempCtl v3 + CanTp simulator

Two programs built on one core (`TempSim.Core`, .NET 10), both driving the
**real** `tempctl` and `cantp` libraries through P/Invoke; nothing is
re-implemented in C#. Every tick: plant (or a scripted temperature profile)
-> sensors -> `TcCheckTemp` -> relays -> plant, then `TcGetDiag` reads the
25 diagnostics, `CanTp_Pack` turns them into the 9 NI-XNET raw frame records
of one J1939 BAM, and `CanTp_Unpack` reads them back as the receive-side
proof (the "unpack mismatches" counter must stay 0).

TempSim is released on its own version line (`CHANGELOG.md` here), one
package per target. The TempCtl controller package and the CanTp package
are separate downloads; TempSim embeds the `tempctl` and `cantp` binaries it
was built with and prints their versions at start-up.

## Files in this package

| File | Purpose |
|---|---|
| `TempSim\TempSim.exe` (win-x64 package) | Interactive Windows simulator: graph, status / warning, seven countdown and accumulator bars, lamps, live setup (every change is a `TcInit`), fault injection, frames panel |
| `TempSim\TempSim.Cli.exe` / `TempSim\TempSim.Cli` | The 16 scripted scenarios with built-in expectations -> CSV + `.ncl`; on Linux also SocketCAN transmit / receive |
| `TempSim\tempctl.dll` / `libtempctl.so`, `cantp.dll` / `libcantp.so`, `TempCtl.json` | The controller and transport binaries the simulator drives, and the CanTp table of the diagnostics message |
| `examples\tempsim-screenshot.png` | The Windows simulator after 75 s of the `failover` scenario (the release gate's screenshot) |
| `examples\failover.csv`, `.ncl` | That scenario's trace and NI-XNET log (open the `.ncl` in NI-XNET Bus Monitor) |
| `TESTLOG.txt` | The gate run (every scenario with its expectations, the screenshot run) and the Raspberry Pi logs |
| `testlogs\` | Raw logs from the Raspberry Pi bench: Pi-vs-Windows byte comparison, live SocketCAN loop, Pi scenario runs |
| `MANIFEST.txt`, `CHANGELOG.md`, `LICENSE.txt` | SHA-256 of every file, release history, MIT |
| `src\` | The simulator's C# source, `Directory.Build.props` (version), `NativeAssets.targets`, `build_sim.bat` |

Both programs are self-contained (bundled .NET runtime; nothing to
install). The native libraries and `TempCtl.json` (the CanTp table
generated from the controller's `tempctl.dbc`) sit next to the executable;
`--native-dir DIR` (CLI) / `TEMPSIM_NATIVE_DIR` points at other builds of
the libraries.

## 1. TempSim.exe (Windows)

Start it and the plant runs from ambient toward the setpoint in real time.

- **Graph**: plant temperature (grey dashed), Temp1 (orange), Temp2 (blue),
  ControlTemp (black), setpoint (green), HiBand/LoBand (dotted), limits (red
  dashed); heating/cooling lanes below; orange shading = running on sensor 2
  (warning 6), red = stopped on a fault (status >= 10). Last 120 s.
- **Controller status**: the status code by name, the warning by name, the
  values control uses (ControlTemp, both averages, the band), the five
  countdowns (deadband, at-setpoint, disagreement, heater and cooler
  feedback) and the two out-of-range accumulators as bars with their
  remaining / accumulated milliseconds, lamps for the relay commands and the
  DO read-backs, the active sensor, raw and corrected readings,
  `Initial_HC_Flag`, the hourly out-of-range event counts and the
  FilterPoints in use.
- **Controller setup**: the 17 `TcInit` values. Every change (Enter or leave
  the field) is a `TcInit` with the full array, so a running zone keeps its
  relays exactly as the library specifies (R9.3), and an invalid setup shows
  `ConfigFault` / `ConfigInvalid` at once.
- **Plant**: ambient, heat/cool rates (deg/s), lag, and a "plant temperature
  now" field to jump the process.
- **Sensor 1 / 2**: offset, noise, lag, **Fault** (None / Open = NaN /
  StuckLast / StuckValue), **Override** slider to drive a reading by hand.
- **Relays**: stuck open / stuck closed (the DO read-back never follows the
  command), answer delay in ticks (a DO-loop latency).
- **Toolbar**: Pause/Run, **Reset (TcReset)**, **Restart (TcInit)** = new
  plant, `TcReset` then `TcInit`, speed 1x/5x/20x/max, and the built-in
  **scenarios** (Load = restart with that scenario's settings, timed events
  and expectations; the text shows what fired and whether each expectation
  held).
- **Frames panel**: the raw records of the last tick (timestamp offset, ID,
  TP.CM/TP.DT, payload hex) and the reassembled 55-byte payload, beside the
  25 diagnostics decoded by `CanTp_Unpack` from those same bytes with the
  controller's own values for comparison (note the wire resolution: 0.03125
  deg for temperatures).
- **Logging**: tick the box to write `%LOCALAPPDATA%\TempSim\logs\tempsim-<time>.csv`
  and `.ncl` while running.

Settings (all of the above) persist in `%LOCALAPPDATA%\TempSim\settings.json`
and the window position in `window.json`. Delete them to start over.

Headless check (used by the release gate):
`TempSim.exe --screenshot out.png [--scenario NAME] [--seconds N]` runs the
scenario flat out, renders the window to `out.png`, writes `out.perf.txt`
and exits with 0 only when there were no unpack mismatches and every
expectation of the scenario held.

## 2. TempSim.Cli

```
TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N] [--every S]
            [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE]
TempSim.Cli --list
TempSim.Cli --rx IFACE [--seconds N]         (Linux)
```

Per scenario it writes `DIR/NAME.csv` and `DIR/NAME.ncl` (and
`NAME.zone1.csv` for the two-zone scenario), prints a state line every
`--every` seconds (default 5; `--every 0` prints every tick: status,
warning, relay commands and physical states, active sensor, the five
countdowns, both accumulators, the event counts and the flag), the events as
they fire and each expectation with ok / FAIL, and ends with the tick count,
frame count, unpack mismatches, expectations passed and the final status and
warning. Exit code 1 if any scenario had an unpack mismatch or a failed
expectation.

### Scenarios (the 15 required by the v3.0.0 handoff, section 10, plus `flicker-25` from Amendment A)

| Name | What happens and what is checked |
|---|---|
| `heat-up` | S1 Single sensor, cold start: HeatPending on tick 1 with 500 ms remaining, HeaterON at 0.6 s, released after AtSetPtTimeout, TempAtSetPt, flag set, no fault through the cycling |
| `setpoint-reinit` | S2 TcInit setpoint 70 at 3 s while heating: heater kept, bands moved, at-setpoint countdown restarted; TcInit setpoint 40 at 40 s (still heating at 71): heater kept, released after 500 ms, CoolPending, CoolerON |
| `cool-down` | S3 Start at 70: CoolPending, CoolerON at 0.6 s, release at the setpoint |
| `chatter` | S4 Profile-driven: one-sample NaN, 200 spikes and setpoint spikes while heating, idle and cooling change no relay; averages hold only in-range samples; four events counted |
| `flicker` | S5 Profile-driven leaky accumulator (drain 0.5): sparse glitches charge one tick and drain in two (3 events, no failure); 75 % duty fails after 3.0 s (about 1.6 x ErrorTimeout); after a reset a 50 % duty fails after 7.6 s (about 4 x ErrorTimeout, 39 events) |
| `flicker-25` | S5b Below the one-third boundary: 33 % duty (1 out, 2 in) nets zero, 25 % duty drains to zero every cycle; never above one tick, never fails, 108 events counted |
| `control-pause` | S6 Heating with the at-setpoint countdown at 400 ms, sensor at 200 for 0.8 s: relay held, countdown frozen at 400, accumulator 800, then resumes and releases at 3.3 s |
| `failover` | S7 Sensor 1 opens at 40 s: warning 1, failover on the 20th tick with the accumulator at 2000, no fault; sensor back at 50 s: RunningOnTemp2, control on corrected sensor 2; sensor 2 stuck at 200 at 70 s: BothSensorsFailed; reset at 90 s |
| `disagree` | S8 Sensor 2 +8 at 40 s: observed at 40.1, warning at 40.6 (T/10), cleared at 43.2 after agreement; drift again at 50 s: TempDisagreeFault at 55.1, latched |
| `compare-gating` | S9 Profile-driven, sensors 10 apart from the start: no comparison while heating, flag at release, observed the tick after, warning at 200 ms; an excursion resets it, restart from zero; fault after the full 2000 ms |
| `relay-feedback` | S10 Heater DO one tick late: HeaterFBMismatch for one tick per transition, no fault; cooler stuck closed at 30 s: warning at 30.1, CoolerFBFault at 31.1; reset at 40 s; FeedbackEnable 0 at 50 s silences a stuck cooler |
| `config-fault` | S11 TcInit with DeadbandHi -1 while running: ConfigFault, relays off; Reset keeps it; TcInit with Enable 0: ConfigInvalid; valid TcInit clears everything |
| `enable-disable` | S12 Zone disabled: open sensor, stuck sensor and stuck relay leave status 0, no warning, nothing accumulated; TcInit Enable 1 at 30 s starts control |
| `reset` | S13 Sensor opens at 10 s: Temp1FailHigh at 11.9 s; reset at 20 s with the sensor still open clears everything and faults again at 21.9 s; sensor back, reset, clean |
| `two-zones` | S14 Zone 0 (two sensors, setpoint 50) and zone 1 (one sensor, setpoint 80, DeadbandTimeout 1000) in the same loop: own bands and timing, zone 0's failover leaves zone 1 untouched, zone 1's trace equals a solo run |
| `time` | S15 Tick Count starting 300 ms before 2^32 wraps during the first countdown; a 60 s backwards step at 2.3 s leaves the at-setpoint countdown at 300 ms; a 600 s gap at 8.2 s completes the pending countdown |

### Scripted temperature profiles

A config's `Profile` is a list of `{AtSeconds, Temp1, Temp2}` points. When
present, the sensors read the profile instead of the plant (step-hold: a
point applies from its time until the next; `NaN` is an open sensor); sensor
offset, noise, lag and faults still apply on top. `StartTickMs` sets the
value of the millisecond tick at t = 0 (near 2^32 to test the wrap), and a
scenario event can shift the clock (backwards steps, long gaps). `Companion`
is a second config for zone 1 stepped in the same loop.

### Determinism, the cross-platform proof

The plant, sensors and noise use only `+ - * /` in IEEE double and a
xorshift generator, the controller is IEEE double in C, and the CSV is
written with fixed rounding and invariant culture. The same scenario
therefore produces **byte-identical CSV and .ncl files on Windows x64 and on
the Raspberry Pi** (`testlogs\pi-sim-2026-09-18.txt`: 16 scenarios, 110/110
expectations, every file identical). Run `--scenario all` on a new target
and `cmp` the files against `examples\` or a Windows run to prove the
libraries behave the same there.

### CSV columns

`t_s, plant, temp1, temp2, ctrl, t1avg, t2avg, do_heat, do_cool, heater,
cooler, status, warning, active, db_rem_ms, asp_rem_ms, cmp_rem_ms,
hfb_rem_ms, cfb_rem_ms, t1_accum_ms, t2_accum_ms, t1_events, t2_events,
hc_flag, hi_band, lo_band, rc, payload`

`heater`/`cooler` are the physical relay states (after stuck/delay
modelling), everything from `ctrl` to `lo_band` comes from `TcGetDiag`
(`status`/`warning` are the `TcCheckTemp` outputs, which the mirrors
equal), `rc` the last return value, `payload` the 55-byte message as hex.

### Config file (`--config`)

JSON with `PeriodMs`, `Seconds`, `Seed`, `StartTickMs`, `Plant` (Ambient,
Initial, LagPerSec, HeatRate, CoolRate), `Sensor1`/`Sensor2` (Offset,
NoiseAmplitude, LagPerSec), `Heater`/`Cooler` (DelayTicks, StuckOpen,
StuckClosed), `Controller` (the 17 setup values by name), `Can`
(SourceAddress, SpacingMs, Interface), `Profile` and `Companion`. The WPF
app's `%LOCALAPPDATA%\TempSim\settings.json` has the same shape and can be
passed to the CLI. Scenarios apply their own overrides on top of the file.

### Linux CAN (SocketCAN)

`--can can1` also transmits every frame on the interface (the 9 records of a
tick are written back-to-back; add `--realtime` to pace ticks). `--rx can0`
listens, feeds every received frame to `CanTp_RxFeed` and prints each
completed TempCtl message by signal name. Bring the interface up first, e.g.
`sudo ip link set can0 up type can bitrate 250000`.

## 3. Rebuilding

In the `temp-controller-dll` repository (the source under `src\` here is
`sim\` there):

```bat
build.bat all                                   :: tempctl binaries (needed by the sim)
python tools\make_tempctl_dbc.py --tables       :: dbc\tempctl.dbc + dbc\tables (needs cantools)
build_sim.bat all                               :: publishes build\sim\{win-x64,linux-x64,linux-arm64} + headless checks
package_sim.bat X.Y.Z [password] ["rids"]       :: dist\TempSim_vX.Y.Z_<rid>{,.zip,_unencrypted.zip}
```

`sim\NativeAssets.targets` copies the right `tempctl` (from `build\<rid>`)
and vendored `cantp` (from `third_party\cantp`) binaries per
RuntimeIdentifier; `sim\Directory.Build.props` holds TempSim's version.
`dotnet run --project sim\TempSim.Cli` works for a quick Windows x64 run.
