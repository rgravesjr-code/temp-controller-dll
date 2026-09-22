# TempSim — Changelog

TempSim is the closed-loop simulator for the TempCtl controller. It is
versioned on its own; the `tempctl` and `cantp` versions a build embeds are
printed by both programs at start-up and recorded in the package's
`TESTLOG.txt`.

---

## v2.2.1 - 2026-09-20

- Replace ambiguous blank timer tracks with explicit idle/disabled/fault states,
  colored active bars, milliseconds remaining and sensor accumulation values.
- Draw finned motor housings and rotating shafts instead of fan-like rotors.
  Label each drive MOTOR M1/M2/M3 and add a toolbar Start/Stop motors button.
- Verify idle, active-disagreement and fault screenshots. Controller and physics
  behavior are unchanged; motor commands still operate independently of TempCtl.

## v2.2.0 - 2026-09-19

- Model distinct UUT inlet/outlet thermal masses with recirculation, actuation
  heating at the UUT, supply heater input, and UUT fan/passive losses.
- Add inlet/outlet graph traces, schematic readouts and signed outlet-minus-
  inlet delta. Redundant controller probes both measure the outlet.
- Expose UUT oil hold-up volume alongside total loop volume and existing
  actuation/flow/thermal settings. Validate each thermal mass separately.
- Add fixture CSV sidecar channels and CLI `--scenario fixture` for custom
  physics runs. Existing controller CSV and CAN schemas remain unchanged.
- Verify 30/30 physics checks, 110/110 controller expectations and a 600-second
  fixture run without faults or unpack mismatches. Pi validation is pending.

## v2.1.0 - 2026-09-19

- Add a dark blue Windows dashboard with simultaneous temperature history and
  animated oil-loop schematic: 1–3 motors, pump, inline heater, UUT and fan.
- Add opt-in estimated fixture physics and a validated configuration screen
  for motors, flow, heater power, thermal mass and fan response. Desktop starts
  in fixture mode; existing scripted scenarios retain their original plant.
- Add elapsed-wall-time pacing at 1x/2x/3x/5x/10x/20x and capture every tick in
  the graph. Reference lines now retain the history of setpoint/limit edits.
- Keep controller configuration, fault injection, logging and CAN inspection
  available in tabs. Add headless captures for the fixture and settings views.
- Windows verification: 110/110 existing scenario expectations, 19/19 new
  physics checks and rendered dashboard/configuration/CAN/failover screens.
  Raspberry Pi verification of the new fixture model is still pending.

## v2.0.0 - 2026-09-18

Simulator for TempCtl **3.0.0** (new API: `TcInit` / `TcCheckTemp` /
`TcReset` / `TcGetDiag`; Amendment A drain rate). Embeds tempctl 3.0.0 and
cantp 1.3.1.

- Drives the v3 calls: the 17-value setup goes through `TcInit` (every
  edit in the WPF setup panel is a re-Init, so R9.3 relay keeping is
  visible), `TcCheckTemp` takes only the live signals, `TcGetDiag` is read
  after every call and its 25 values are what `CanTp_Pack` puts on the wire
  (55-byte BAM, `TempCtl.json` regenerated from the new `tempctl.dbc`).
- **16 scenarios** = the 15 required scenarios of the v3.0.0 handoff
  (`heat-up`, `setpoint-reinit`, `cool-down`, `chatter`, `flicker`,
  `control-pause`, `failover`, `disagree`, `compare-gating`,
  `relay-feedback`, `config-fault`, `enable-disable`, `reset`, `two-zones`,
  `time`) plus `flicker-25` (Amendment A: the never-failing side of the
  one-third duty boundary), each with **built-in expectations** (110 in
  total) on status, warning, relays, countdowns and accumulators at exact
  ticks; the CLI and the screenshot gate fail on any of them.
- **Scripted temperature profiles** (`Profile` in the config: step-hold
  points, NaN = open sensor) for tick-exact timing scenarios, a
  `StartTickMs` and clock-shift events for the 2^32 wrap, backwards steps
  and long gaps, and a `Companion` zone stepped in the same loop for the
  two-zone scenario (its trace is compared with a solo run).
- WPF: status and warning by name, seven bars (five countdowns, two
  accumulators), DO read-back lamps, `Initial_HC_Flag`, hourly event
  counts, the 17-field setup panel, the 25-row decoded list; faults shade
  red, running-on-sensor-2 shades orange. CLI: `--every S` (0 = every tick)
  state lines with all countdowns and accumulators, expectation reporting,
  `NAME.zone1.csv`.
- CSV columns changed (diagnostics instead of the v2 output array).
- Verified on the Raspberry Pi (linux-arm64): 110/110 expectations, every
  CSV and .ncl byte-identical to Windows (`testlogs\pi-sim-2026-09-18.txt`).
- The v1.x scenarios (`warmup`, `setpoint-step`, `sensor-failover`,
  `both-sensors-fail`, `disagree`, `feedback-fault`, `single-sensor`) are
  superseded by the above.

## v1.0.0 - 2026-09-15

First release of TempSim as its own package (until now the simulator was
part of the TempCtl controller package, versions 2.0.0 to 2.0.2, where it
was 99 % of the download). Functionally the simulator of TempCtl 2.0.2:

- `TempSim.exe` (Windows x64, WPF): strip chart with sensors / ControlTemp /
  plant / setpoint / bands / limits / relay lanes, countdown bars, relay and
  feedback lamps, live controller settings, plant rates, sensor override
  sliders and fault injection, relay stuck switches, raw frames panel beside
  the CanTp-unpacked values, optional CSV/.ncl logging, settings persisted,
  `--screenshot` headless mode.
- `TempSim.Cli` (Windows x64, Linux x64, Linux arm64): seven scripted
  scenarios, CSV + .ncl per scenario, optional SocketCAN transmit (`--can`)
  and receive/decode (`--rx`) on Linux.
- Embeds tempctl 2.0.3 and cantp 1.3.1 (the vendored subset in
  `third_party\cantp`); calls `CanTp_Define` / `PackSgl` / `Unpack` /
  `RxFeed` only, so any CanTp 1.x works.
- Packages: one zip per target (`TempSim_v1.0.0_win-x64`, `_linux-x64`,
  `_linux-arm64`), each with the self-contained publish under `TempSim\`,
  `SIMULATOR.md`, the gate log, the failover scenario's CSV/.ncl and
  screenshot as examples, the Raspberry Pi logs and the C# source.
- Version now comes from `sim\Directory.Build.props` and is printed by both
  programs (was a hard-coded "2.0.0").
