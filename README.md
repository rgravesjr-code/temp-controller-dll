# TempCtl — temperature controller for LabVIEW (DLL / .so), with the TempSim simulator

Two products in one repository, released as separate packages:

- **TempCtl** (`src/`): a small C99 library for LabVIEW's Call Library
  Function Node. A dual-sensor temperature controller with deadband timing,
  sensor rationality and failover, relay feedback checks and a moving-average
  filter. One source tree builds `tempctl.dll` (Windows x64 and x86) and
  `libtempctl.so` (Linux x86_64 for the cRIO-904x/905x/906x, aarch64 for the
  Raspberry Pi). No runtime dependencies. **Signals in, signals out**: its
  CAN message is a DBC (`dbc/tempctl.dbc`, PGN 65280 as a J1939 BAM)
  transported by the separate **CanTp** library,
  [`rgravesjr-code/can-tp-dll`](https://github.com/rgravesjr-code/can-tp-dll).
- **TempSim** (`sim/`): a .NET 10 simulator (WPF app for Windows, console for
  Windows/Linux) that closes the loop around the real `tempctl` and `cantp`
  binaries: plant, sensors, relays, fault injection, scripted scenarios, CSV
  and NI-XNET `.ncl` logs, SocketCAN on Linux. It is the controller's
  closed-loop test bench and a demo for the LabVIEW side.

TempCtl depends on CanTp (minimum v1.0.0, see `third_party/cantp/VENDORED.txt`);
CanTp depends on nothing here. A CanTp release does not by itself trigger a
TempCtl release.

```
src/tempctl.h            public API + full semantics (the spec)
src/tempctl.c            controller state machine, 16 zone slots
tests/test_main.c        181 unit checks, compiled with the source (no DLL needed)
tests/oracle_test.py     ctypes: TcStep -> CanTp_PackSgl -> BAM, bit-for-bit vs cantools + tempctl.dbc
tools/make_tempctl_dbc.py   generates dbc/tempctl.dbc and (via CanTp's dbc2tables) dbc/tables/
dbc/                     tempctl.dbc + CanTp tables (JSON, CSV for LabVIEW, C header)
third_party/cantp/       the CanTp subset TempCtl needs: header, binaries, dbc2tables.py, license (VENDORED.txt)
docs/package/            every document that ships in the TempCtl package (flat at its root):
                         DISTRIBUTION_README, TEMPCTL_PACKAGE_GUIDE (API), LABVIEW_INTEGRATION, TESTING, TEMPCTL-SPEC
docs/notes/              repo-only: 2026-09-04 handoff (design record), cover notes, Word files
sim/TempSim.Core         plant / sensor / relay models, P/Invoke, scenarios, CSV + .ncl, SocketCAN
sim/TempSim.Cli          console simulator (win-x64, linux-x64, linux-arm64)
sim/TempSim.Wpf          Windows simulator with graph, lamps, fault injection, frames panel
sim/SIMULATOR.md, sim/CHANGELOG.md, sim/Directory.Build.props (TempSim version), sim/testlogs/ (Pi runs)
build.bat                tempctl for every target + the 181-check gates
build_sim.bat            publishes build/sim/{win-x64,linux-x64,linux-arm64} + headless checks
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
build.bat all                                :: dll x64+x86, 181-check gates, .so linux-x64 + linux-arm64
python tools\make_tempctl_dbc.py --tables    :: dbc\tempctl.dbc + dbc\tables\
python tests\oracle_test.py                  :: ALL OK
package_dist.bat 2.0.3 [zip-password]        :: dist\TempCtl_v2.0.3*          (controller)
build_sim.bat all                            :: build\sim\{win-x64,linux-x64,linux-arm64} + headless checks
package_sim.bat 1.0.0 [zip-password]         :: dist\TempSim_v1.0.0_{win-x64,linux-x64,linux-arm64}*
```

## Use

```c
int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
               const float* in, int32_t inLen, float* out, int32_t outLen);
```

`in`: 17 SGL — Setpoint, DeadbandHi, DeadbandLo, HiLimit, LoLimit,
ErrorTimeout, DeadbandTimeout, FilterPoints, Temp2Enable, Temp2Tolerance,
FeedbackEnable, Temp1, Temp2, HeaterFeedback, CoolerFeedback, HeatingCmd,
CoolingCmd. `out`: those 17 (with the relay commands) + ErrorStatus (bit
mask), TempStatus, ControlTemp, Temp1Filtered, Temp2Filtered, HiBand,
LoBand, ErrorRemainMs, DbRemainMs, ActiveSensor = 27 SGL, which is exactly
the CAN message order for `CanTp_PackSgl`. Semantics: `src/tempctl.h`,
tables: `docs/package/TEMPCTL_PACKAGE_GUIDE.md`, LabVIEW wiring:
`docs/package/LABVIEW_INTEGRATION.md`, simulator: `sim/SIMULATOR.md`.

## Deploy to the cRIO-9045

```
scp build\linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio>:/usr/local/lib/
scp build\linux-x64\test_tempctl admin@<crio>:/tmp/
ssh admin@<crio> "chmod 755 /usr/local/lib/lib*.so /tmp/test_tempctl && /tmp/test_tempctl"   -> 181 passed, 0 failed
```

The Raspberry Pi uses the `linux-arm64` files the same way (verified on the
bench with the simulator, see `sim/testlogs/`).
