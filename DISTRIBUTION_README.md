# TempCtl — Distribution Package v2.0.0

Temperature controller for LabVIEW (Call Library Function Node) as a Windows
DLL and Linux shared libraries, with its J1939 CAN message defined in a DBC
and transported by the separate **CanTp** library (vendored, unmodified), and
a closed-loop simulator for Windows and Linux.

## Files in this package

| File | Purpose |
|---|---|
| `tempctl.dll`, `tempctl.lib` | The controller, **Windows x64** (64-bit LabVIEW, Python, .NET) |
| `x86\tempctl.dll`, `x86\tempctl.lib` | The same library built **x86** for 32-bit LabVIEW |
| `linux-x64\libtempctl.so` | **NI Linux RT x86_64** (cRIO-904x/905x/906x, incl. cRIO-9045) |
| `linux-arm64\libtempctl.so` | **aarch64 Linux** (Raspberry Pi 4/5) |
| `linux-*\test_tempctl`, `test_tempctl.exe`, `x86\test_tempctl.exe` | Release-gate test for each target (`181 passed, 0 failed`) |
| `tempctl.h` | C header, one header for every build (the behavioural spec is in its comments) |
| `third_party\cantp\` | **CanTp v1.0.0** release package, unmodified: `cantp.dll` (x64, `x86\`), `libcantp.so` (`linux-x64\`, `linux-arm64\`), `cantp.h`, its guides, `tools\dbc2tables.py`. See `VENDORED.txt` |
| `dbc\tempctl.dbc` | The controller message: PGN 65280, 27 signals in `TcStep` output order, J1939 BAM |
| `dbc\tables\TempCtl.*.csv`, `.json`, `cantp_tables.h` | The same message as CanTp tables: CSV for LabVIEW (`Read Delimited Spreadsheet` → `CanTp_Define`), JSON for the simulator, C header |
| `simulator\win-x64\TempSim.exe` | Windows simulator (WPF): plant + sensors + relays around the real DLLs, live graph, frames panel |
| `simulator\win-x64\TempSim.Cli.exe`, `simulator\linux-x64\TempSim.Cli`, `simulator\linux-arm64\TempSim.Cli` | Console simulator: scripted scenarios → CSV + `.ncl`; Linux builds can drive a SocketCAN interface |
| `TESTLOG.txt` | Windows gates, DBC check, Python oracle, simulator gates, and the Raspberry Pi logs |
| `DEPENDENCIES.txt` | Import report of the native libraries (no VC runtime; .so on libc only) |
| `MANIFEST.txt` | File list with SHA-256 hashes |
| `TEMPCTL_PACKAGE_GUIDE.md` | **API reference**: `TcStep` parameters, the 27-signal array, status/error codes, semantics |
| `LABVIEW_INTEGRATION.md` | CLFN settings, RT loop sketch with CanTp, cRIO/Pi deployment, troubleshooting |
| `SIMULATOR.md` | Using the simulators, scenarios, config file, CSV columns, SocketCAN |
| `TESTING.md` | What the gates cover and how to re-run them |
| `CHANGELOG.md`, `LICENSE.txt` | Release history, MIT license |
| `docs\DESIGN-DECISIONS-2026-09-04.md` | The decisions taken with the owner and Scott that shaped v2 and CanTp |
| `docs\testlogs\` | Raw logs from the Raspberry Pi runs |
| `examples\oracle_test.py` | ctypes example of the full chain: `TcStep` → `CanTp_PackSgl` → frames → `CanTp_Unpack`, checked against cantools |
| `examples\out\sensor-failover.csv`, `.ncl` | Simulator output of the failover scenario (open the `.ncl` in NI-XNET Bus Monitor) |
| `examples\tempsim-screenshot.png` | The Windows simulator after 70 s of that scenario |
| `tools\make_tempctl_dbc.py` | Generator of `tempctl.dbc` (the only place the message layout is defined) |
| `src\` | Complete C source, build scripts, and the simulator's C# source |

No runtime dependencies for the libraries: the DLLs link the CRT statically
and import only `KERNEL32.dll`; the `.so` files import only `memcpy`/`memset`
from libc (GLIBC 2.14 symbols). The simulators are self-contained .NET 10
publishes (nothing to install).

## Quick start

**Verify on your machine**

```bat
test_tempctl.exe                      -> 181 passed, 0 failed
simulator\win-x64\TempSim.exe         -> the graph runs immediately; try Scenario > sensor-failover > Load
simulator\win-x64\TempSim.Cli.exe --scenario all --out out
```

**cRIO-9045 (x86_64)**

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio-ip>:/usr/local/lib/
scp linux-x64\test_tempctl admin@<crio-ip>:/home/admin/
ssh admin@<crio-ip> "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl && /home/admin/test_tempctl"
```

**Raspberry Pi (aarch64)**: same with the `linux-arm64\` files; the
`simulator\linux-arm64\TempSim.Cli` folder runs there directly
(`./TempSim.Cli --scenario all --out out`, add `--can can0` to put the frames
on a real bus).

## The one controller function

```c
int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
               const float* in, int32_t inLen, float* out, int32_t outLen);
```

`action` 0 Init / 1 Step / 2 Reset; `nowMs` = Tick Count (ms); `in` 17 SGL
(configuration, measurements, initial relay state); `out` 27 SGL (echo of the
inputs with the relay commands, then ErrorStatus, TempStatus, ControlTemp,
filtered temperatures, bands, countdowns, active sensor). Full table in
`TEMPCTL_PACKAGE_GUIDE.md`.

Sending the state on CAN is two CanTp calls: `CanTp_Define(slot, msg, 8,
sig, 27)` once with the tables from `dbc\tables\`, then `CanTp_PackSgl(slot,
out, 27, ts, spacing, frames, cap, &written)` every tick → 9 NI-XNET raw
frame records (one J1939 BAM) for XNET Write. `CanTp_Unpack` / `CanTp_RxFeed`
do the reverse on the receiving side.

## Design decisions in this release

- **Pure controller.** No CAN, no hardware, no file I/O in `tempctl`. The
  caller maps thermocouples and relays; the minimum system is one sensor and
  the heater/cooler outputs.
- **Deadbands are offsets** added to / subtracted from the setpoint.
- **Second sensor is optional.** With it enabled the controller keeps running
  on the surviving sensor when one fails (rationality check on each,
  disagreement check between them); both failed → stopped. Failed sensors
  stay failed until Reset (no flapping).
- **Feedback faults set a bit and keep going** (Scott's D4); failed sensors
  and feedback bits are latched until Reset (owner's defaults Q2/Q3).
- **ErrorStatus is a bit mask**; the low three bits keep the v1 meaning for
  a single-sensor system.
- **Never a DBC inside a DLL.** The message is defined once
  (`tempctl.dbc`), converted to flat tables, loaded into CanTp with one call.

## Known limits

- The x86_64 `.so` was cross-compiled and inspected (ELF machine, exports,
  GLIBC needs) but **not executed on a cRIO** in this release; the identical
  source was executed on the Raspberry Pi (aarch64) with the same 181 checks
  and the same simulator outputs as Windows. Run `linux-x64\test_tempctl` on
  the target as the first step.
- The Windows simulator is x64 only (the x86 DLL is covered by its own test
  executable).
- CanTp release 1 transports: classic CAN, CAN FD, J1939 BAM, receive.
  RTS/CTS and ISO-TP are CanTp release 2.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. No NI or Vector
code is used or linked; the CAN encoding follows the public J1939-21 and
NI-XNET raw-frame specifications through CanTp.
