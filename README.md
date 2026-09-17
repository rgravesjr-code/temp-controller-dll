# TempCtl — temperature controller for LabVIEW (DLL / .so), with the TempSim simulator

Two products in one repository, released as separate packages:

- **TempCtl** (`src/`): a small C99 library for LabVIEW's Call Library
  Function Node. Deadband heating/cooling control with sensor validation:
  one or two thermocouples checked against limits with a leaky out-of-range
  accumulator, failover to the second (offset-corrected) sensor, a two-stage
  disagreement check, time-qualified relay transitions (no chatter), and a
  per-relay DO feedback check. One source tree builds `tempctl.dll`
  (Windows x64 and x86) and `libtempctl.so` (Linux x86_64 for the
  cRIO-904x/905x/906x, aarch64 for the Raspberry Pi). No runtime
  dependencies. **Signals in, signals out**: no CAN inside; its diagnostics
  array is a DBC message (`dbc/tempctl.dbc`, PGN 65280 as a J1939 BAM)
  transported by the separate **CanTp** library,
  [`rgravesjr-code/can-tp-dll`](https://github.com/rgravesjr-code/can-tp-dll).
- **TempSim** (`sim/`): a .NET 10 simulator (WPF app for Windows, console for
  Windows/Linux) that closes the loop around the real `tempctl` and `cantp`
  binaries: plant or scripted temperature profiles, sensors, relays, fault
  injection, the 16 scenarios (the 15 of the v3.0.0 handoff plus flicker-25) with built-in
  expectations, CSV and NI-XNET `.ncl` logs, SocketCAN on Linux. It is the
  controller's closed-loop test bench and a demo for the LabVIEW side.

TempCtl depends on CanTp (minimum v1.0.0, see `third_party/cantp/VENDORED.txt`);
CanTp depends on nothing here. A CanTp release does not by itself trigger a
TempCtl release.

```
src/tempctl.h            public API: TcInit / TcCheckTemp / TcReset / TcGetDiag (+ TcVersion, TcSetupCount, TcDiagCount)
src/tempctl.c            controller state machine, 16 zone slots, static memory only
tests/test_main.c        1516 unit checks by rule number (R1..R9, C1..C22, S1..S15), compiled with the source
tests/oracle_test.py     ctypes: TcCheckTemp/TcGetDiag -> CanTp_Pack -> BAM, bit-for-bit vs cantools + tempctl.dbc
tools/make_tempctl_dbc.py   generates dbc/tempctl.dbc over the TC_DIAG_* array and (via CanTp's dbc2tables) dbc/tables/
dbc/                     tempctl.dbc + CanTp tables (JSON, CSV for LabVIEW, C header)
third_party/cantp/       the CanTp subset TempCtl needs: header, binaries, dbc2tables.py, license (VENDORED.txt)
docs/package/            every document that ships in the TempCtl package (flat at its root): DISTRIBUTION_README,
                         TEMPCTL_PACKAGE_GUIDE (API), LABVIEW_INTEGRATION, TESTING, TEMPCTL-SPEC-v3.0.0, TEMPCTL-CAPABILITY-v3.0.0
docs/notes/              repo-only: the v3.0.0 handoff and capability docx (the contract), the 2026-09-04 split handoff,
                         cover notes, the superseded v2.0.1 spec and Word files
docs/testlogs/           bare test_tempctl runs on the Raspberry Pi (appended to the package's TESTLOG.txt)
sim/TempSim.Core         plant / profile / sensor / relay models, P/Invoke, scenarios + expectations, CSV + .ncl, SocketCAN
sim/TempSim.Cli          console simulator (win-x64, linux-x64, linux-arm64)
sim/TempSim.Wpf          Windows simulator with graph, countdown bars, lamps, live setup, fault injection, frames panel
sim/SIMULATOR.md, sim/CHANGELOG.md, sim/Directory.Build.props (TempSim version), sim/testlogs/ (Pi runs)
build.bat                tempctl for every target + the 1516-check gates
build_sim.bat            publishes build/sim/{win-x64,linux-x64,linux-arm64} + the scenario and screenshot gates
package_dist.bat         dist/TempCtl_vX.Y.Z      (controller package, a few MB)
package_sim.bat          dist/TempSim_vX.Y.Z_<rid> (simulator packages, one per target)
scripts/                 version validators, generic DEPENDENCIES/MANIFEST generator
claudetodelete/          parked files (owner rule: never delete); gitignored
```

## Build

Requires Visual Studio 2022+ Build Tools (C++ workload), a zig toolchain
for the Linux cross-builds (`..\tools\zig-x86_64-windows-*\zig.exe` or
`%ZIG_HOME%`), the .NET 10 SDK for the simulator, and Python with
`cantools` for the DBC and the oracle.

```bat
build.bat all                                :: dll x64+x86, 1516-check gates, .so linux-x64 + linux-arm64
python tools\make_tempctl_dbc.py --tables    :: dbc\tempctl.dbc + dbc\tables\
python tests\oracle_test.py                  :: ALL OK
package_dist.bat 3.0.0 [zip-password]        :: dist\TempCtl_v3.0.0*          (controller)
build_sim.bat all                            :: build\sim\{win-x64,linux-x64,linux-arm64} + 16 scenarios + screenshot
package_sim.bat 2.0.0 [zip-password]         :: dist\TempSim_v2.0.0_{win-x64,linux-x64,linux-arm64}*
```

## Use

```c
int32_t TcInit(int32_t zone, uint32_t nowMs, const double* setupArray, int32_t setupLen, int32_t* status, int32_t* warning);
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs, double temp1, double temp2, int32_t diHeaterFB, int32_t diCoolerFB,
                    int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning);
int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning);
int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);
```

`setupArray`: 17 DBL — TempCtrlEnable, TempUnits, Setpoint, DeadbandHi,
DeadbandLo, HiLimit, LoLimit, ErrorTimeout, DeadbandTimeout, AtSetPtTimeout,
Temp2Enable, Temp2Offset, Temp2Tolerance, TempCompareTimeout, FilterPoints,
FeedbackEnable, RelayFeedbackTimeout. `status` 0..5 states / 10..16 faults,
`warning` 0..7. `diagArray`: 25 DBL (ControlTemp, ActiveSensor, raw and
corrected readings, averages, bands, Initial_HC_Flag, five countdowns, two
accumulators, two hourly event counts, status/warning/relay mirrors,
FilterPoints in use, ZoneInitialized), which is exactly the CAN message order
for `CanTp_Pack`. Semantics: `docs/package/TEMPCTL-SPEC-v3.0.0.md`, tables:
`docs/package/TEMPCTL_PACKAGE_GUIDE.md`, LabVIEW wiring:
`docs/package/LABVIEW_INTEGRATION.md`, simulator: `sim/SIMULATOR.md`.

## Deploy to the cRIO-9045

```
scp build\linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio>:/usr/local/lib/
scp build\linux-x64\test_tempctl admin@<crio>:/tmp/
ssh admin@<crio> "chmod 755 /usr/local/lib/lib*.so /tmp/test_tempctl && /tmp/test_tempctl"   -> 1516 passed, 0 failed
```

The Raspberry Pi uses the `linux-arm64` files the same way (verified on the
bench: `docs/testlogs/`, and with the simulator: `sim/testlogs/`).
