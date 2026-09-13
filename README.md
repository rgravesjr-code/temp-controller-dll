# TempCtl — temperature controller for LabVIEW (DLL / .so) + simulator

A small C99 library for LabVIEW's Call Library Function Node: a dual-sensor
temperature controller with deadband timing, sensor rationality and
failover, relay feedback checks and a moving-average filter. One source tree
builds `tempctl.dll` (Windows x64 and x86) and `libtempctl.so` (Linux
x86_64 for the cRIO-904x/905x/906x, aarch64 for the Raspberry Pi). No
runtime dependencies.

The controller is **signals in, signals out**. Its CAN message is a DBC
(`dbc\tempctl.dbc`, PGN 65280 as a J1939 BAM) transported by the separate
**CanTp** library — [`rgravesjr-code/can-tp-dll`](https://github.com/rgravesjr-code/can-tp-dll),
vendored unmodified in `third_party\cantp\`. A .NET simulator (WPF app for
Windows, console for Windows/Linux) closes the loop around the real
binaries.

```
src/tempctl.h          public API + full semantics (the spec)
src/tempctl.c          controller state machine, 16 zone slots
tests/test_main.c      181 unit checks, compiled with the source (no DLL needed)
tests/oracle_test.py   ctypes: TcStep -> CanTp_PackSgl -> BAM, bit-for-bit vs cantools + tempctl.dbc
tools/make_tempctl_dbc.py   generates dbc/tempctl.dbc and (via CanTp's dbc2tables) dbc/tables/
dbc/                   tempctl.dbc + CanTp tables (JSON, CSV for LabVIEW, C header)
third_party/cantp/     CanTp v1.2.0 release package (see VENDORED.txt)
sim/TempSim.Core       plant / sensor / relay models, P/Invoke, scenarios, CSV + .ncl, SocketCAN
sim/TempSim.Cli        console simulator (win-x64, linux-x64, linux-arm64)
sim/TempSim.Wpf        Windows simulator with graph, lamps, fault injection, frames panel
build.bat / build_sim.bat / package_dist.bat   builds, simulator publish, distribution package
docs/                  design decisions (HANDOFF-2026-09-04.md), Pi test logs
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
build_sim.bat all                            :: build\sim\{win-x64,linux-x64,linux-arm64} + headless checks
package_dist.bat 2.0.1 [zip-password]        :: dist\TempCtl_v2.0.1*
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
tables: `TEMPCTL_PACKAGE_GUIDE.md`, LabVIEW wiring: `LABVIEW_INTEGRATION.md`,
simulator: `SIMULATOR.md`.

## Deploy to the cRIO-9045

```
scp build\linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio>:/usr/local/lib/
scp build\linux-x64\test_tempctl admin@<crio>:/tmp/
ssh admin@<crio> "chmod 755 /usr/local/lib/lib*.so /tmp/test_tempctl && /tmp/test_tempctl"   -> 181 passed, 0 failed
```

The Raspberry Pi uses the `linux-arm64` files the same way (verified on the
bench, see `docs/testlogs/`).
