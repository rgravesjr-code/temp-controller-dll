# TempSim — Changelog

TempSim is the closed-loop simulator for the TempCtl controller. It is
versioned on its own; the `tempctl` and `cantp` versions a build embeds are
printed by both programs at start-up and recorded in the package's
`TESTLOG.txt`.

---

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
