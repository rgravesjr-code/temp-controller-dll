# TempSim — the TempCtl v2 + CanTp simulator

Two programs built on one core (`sim\TempSim.Core`, .NET 10), both driving
the **real** `tempctl` and `cantp` libraries through P/Invoke — nothing is
re-implemented in C#. Every tick: plant → sensors → `TcStep` → relays →
plant, then `CanTp_PackSgl` turns the 27-element output array into the 9
NI-XNET raw frame records of one J1939 BAM, and `CanTp_Unpack` reads them
back as the receive-side proof (the "unpack mismatches" counter must stay 0).

| Program | Platforms | Purpose |
|---|---|---|
| `simulator\win-x64\TempSim.exe` | Windows x64 | Interactive: graph, lamps, countdowns, live settings, fault injection, frames panel |
| `simulator\win-x64\TempSim.Cli.exe`, `simulator\linux-x64\TempSim.Cli`, `simulator\linux-arm64\TempSim.Cli` | Windows x64, Linux x86_64, Linux aarch64 | Scripted scenarios → CSV + `.ncl`; Linux: SocketCAN transmit / receive |

Both are self-contained (bundled .NET runtime). The native libraries and
`TempCtl.json` (the CanTp table generated from `dbc\tempctl.dbc`) sit next
to the executable; `--native-dir DIR` (CLI) / `TEMPSIM_NATIVE_DIR` points at
other builds of the libraries.

## 1. TempSim.exe (Windows)

Start it and the plant runs from ambient toward the setpoint in real time.

- **Graph**: plant temperature (grey dashed), Temp1 (orange), Temp2 (blue),
  ControlTemp (black), setpoint (green), HiBand/LoBand (dotted), limits (red
  dashed); heating/cooling lanes below; orange shading = Degraded, red =
  Stopped. Last 120 s.
- **Controller status**: TempStatus, ErrorStatus bits by name, the two
  countdowns as bars, lamps for the heating/cooling commands and the relay
  feedback contacts, active sensor and raw readings.
- **Controller configuration**: the 11 configuration inputs of `TcStep`,
  applied on the next tick (Enter or leave the field).
- **Plant**: ambient, heat/cool rates (°/s), lag, and a "plant temperature
  now" field to jump the process.
- **Sensor 1 / 2**: offset, noise, lag, **Fault** (None / Open = NaN /
  StuckLast / StuckValue), **Override** slider to drive a reading by hand.
- **Relays**: stuck open / stuck closed (feedback never follows the
  command), answer delay in ticks.
- **Toolbar**: Pause/Run, **Reset (operator)** = `TcStep` action 2,
  **Restart (Init)** = new plant + action 0, speed 1x/5x/20x/max, and the
  built-in **scenarios** (Load = restart with that scenario's settings and
  its timed events; the event text shows what fired).
- **Frames panel**: the raw records of the last tick (timestamp offset, ID,
  TP.CM/TP.DT, payload hex) and the reassembled 50-byte payload, beside the
  27 values decoded by `CanTp_Unpack` from those same bytes with the
  controller's own values for comparison (note the wire resolution: 0.03125
  ° for temperatures).
- **Logging**: tick the box to write `%LOCALAPPDATA%\TempSim\logs\tempsim-<time>.csv`
  and `.ncl` while running.

Settings (all of the above) persist in `%LOCALAPPDATA%\TempSim\settings.json`
and the window position in `window.json`. Delete them to start over.

Headless check (used by the release gate):
`TempSim.exe --screenshot out.png [--scenario NAME] [--seconds N]` runs the
scenario flat out, renders the window to `out.png`, writes `out.perf.txt`
and exits with 0 when there were no unpack mismatches.

## 2. TempSim.Cli

```
TempSim.Cli [--scenario NAME|all] [--out DIR] [--config FILE] [--seconds N] [--period-ms N]
            [--can IFACE] [--realtime] [--quiet] [--native-dir DIR] [--table FILE]
TempSim.Cli --list
TempSim.Cli --rx IFACE [--seconds N]         (Linux)
```

Per scenario it writes `DIR/NAME.csv` and `DIR/NAME.ncl`, prints a state
line every 5 s and the events as they fire, and ends with the tick count,
frame count, unpack mismatches, final status and error bits. Exit code 1 if
any scenario had an unpack mismatch.

### Scenarios

| Name | What happens |
|---|---|
| `warmup` | Cold start at ambient: filter warm-up, heat pending, heating to setpoint, in-band cycling |
| `setpoint-step` | Setpoint 50 → 30 at 60 s (cooler engages), → 70 at 90 s |
| `sensor-failover` | Sensor 1 opens at 40 s: relays drop, T1 fails after ErrorTimeout, control fails over to sensor 2 (Degraded); sensor 1 returns at 60 s but stays failed until the operator reset at 80 s |
| `both-sensors-fail` | Sensor 2 sticks at 200 at 30 s (Degraded), sensor 1 opens at 50 s (Stopped), reset at 70 s |
| `disagree` | Sensor 2 drifts +8 ° at 40 s: disagreement bit, control continues on sensor 1 |
| `feedback-fault` | Heater sticks open at 30 s (heater feedback bit, operation continues); cooler sticks closed at 80 s (cooler feedback bit; the stuck cooler drives the plant below LoLimit until both sensors fail lo and the controller stops) |
| `single-sensor` | One sensor, no feedback: sensor opens at 50 s → Stopped after ErrorTimeout; reset at 70 s |

### Determinism, the cross-platform proof

The plant, sensors and noise use only `+ − × ÷` in IEEE double and a
xorshift generator, the controller is IEEE single in C, and the CSV is
written with fixed rounding and invariant culture. The same scenario
therefore produces **byte-identical CSV and .ncl files on Windows x64 and on
the Raspberry Pi** (`docs\testlogs\pi-vs-windows-2026-09-04.txt`). Run
`--scenario all` on a new target and `cmp` the files against
`examples\out\` or a Windows run to prove the libraries behave the same
there.

### CSV columns

`t_s, plant, temp1, temp2, ctrl, t1f, t2f, heat_cmd, cool_cmd, heater, cooler,
err, status, active, err_remain_ms, db_remain_ms, hi_band, lo_band, rc, payload`

`heater`/`cooler` are the physical relay states (after stuck/delay
modelling), `err`/`status`/`active` the controller outputs, `rc` the
`TcStep` return value, `payload` the 50-byte message as hex.

### Config file (`--config`)

JSON with the sections `PeriodMs`, `Seconds`, `Seed`, `Plant` (Ambient,
Initial, LagPerSec, HeatRate, CoolRate), `Sensor1`/`Sensor2` (Offset,
NoiseAmplitude, LagPerSec), `Heater`/`Cooler` (DelayTicks, StuckOpen,
StuckClosed), `Controller` (the 11 configuration inputs) and `Can`
(SourceAddress, SpacingMs, Interface). The WPF app's
`%LOCALAPPDATA%\TempSim\settings.json` has the same shape and can be passed
to the CLI. Scenarios apply their own overrides on top of the file.

### Linux CAN (SocketCAN)

`--can can1` also transmits every frame on the interface (the 9 records of a
tick are written back-to-back; add `--realtime` to pace ticks). `--rx can0`
listens, feeds every received frame to `CanTp_RxFeed` and prints each
completed TempCtl message. This is the bench loop recorded in
`docs\testlogs\pi-socketcan-loop-2026-09-04.txt`: TempSim on pi-engine's
can1 → TempSim `--rx` on pi-trans's can0, 81 messages in 729 frames, all
reassembled. Bring the interface up first, e.g.
`sudo ip link set can0 up type can bitrate 250000`.

## 3. Rebuilding

```bat
build.bat all                                   :: tempctl binaries (needed by the sim)
python tools\make_tempctl_dbc.py --tables       :: dbc\tempctl.dbc + dbc\tables (needs cantools)
build_sim.bat all                               :: publishes build\sim\{win-x64,linux-x64,linux-arm64}
```

`sim\NativeAssets.targets` copies the right `tempctl` and vendored `cantp`
binaries per RuntimeIdentifier. `dotnet run --project sim\TempSim.Cli`
works for a quick Windows x64 run.
