# TempSim — Changelog

TempSim is the closed-loop simulator for the TempCtl controller. It is
versioned on its own; the `tempctl` and `cantp` versions a build embeds are
printed by both programs at start-up and recorded in the package's
`TESTLOG.txt`.

---

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
