# TempCtl — Distribution Package v2.0.3

Temperature controller for LabVIEW (Call Library Function Node) as a Windows
DLL and Linux shared libraries, with its J1939 CAN message defined in a DBC
and transported by the separate **CanTp** library (the subset TempCtl needs
is included, unmodified).

The closed-loop simulator **TempSim** is a separate package
(`TempSim_v1.0.0_win-x64.zip`, `_linux-x64.zip`, `_linux-arm64.zip`); it is
not needed to use the controller.

## Files in this package

| File | Purpose |
|---|---|
| `tempctl.dll`, `tempctl.lib` | The controller, **Windows x64** (64-bit LabVIEW, Python, .NET) |
| `x86\tempctl.dll`, `x86\tempctl.lib` | The same library built **x86** for 32-bit LabVIEW |
| `linux-x64\libtempctl.so` | **NI Linux RT x86_64** (cRIO-904x/905x/906x, incl. cRIO-9045) |
| `linux-arm64\libtempctl.so` | **aarch64 Linux** (Raspberry Pi 4/5) |
| `linux-*\test_tempctl`, `test_tempctl.exe`, `x86\test_tempctl.exe` | Release-gate test for each target (`181 passed, 0 failed`) |
| `tempctl.h` | C header, one header for every build (the behavioural spec is in its comments) |
| `third_party\cantp\` | **CanTp v1.3.1**, the subset TempCtl needs, unmodified: `cantp.h`, `cantp.dll` (x64, `x86\`), `libcantp.so` (`linux-x64\`, `linux-arm64\`, `linux-armhf\`), `tools\dbc2tables.py`, `LICENSE.txt`. `VENDORED.txt` has the version, the zip hash and the dependency rule (minimum CanTp 1.0.0). CanTp's guides, source, tests and `test_cantp` are in the CanTp package |
| `dbc\tempctl.dbc` | The controller message: PGN 65280, 27 signals in `TcStep` output order, J1939 BAM |
| `dbc\tables\TempCtl.*.csv`, `.json`, `cantp_tables.h` | The same message as CanTp tables: CSV for LabVIEW (`Read Delimited Spreadsheet` → `CanTp_Define`), JSON (what the simulator loads), C header |
| `TESTLOG.txt` | Windows gates, DBC check and the Python oracle, captured at package time |
| `DEPENDENCIES.txt` | Import report of the native libraries (no VC runtime; .so on libc only) |
| `MANIFEST.txt` | File list with SHA-256 hashes |
| `TEMPCTL_PACKAGE_GUIDE.md` | **API reference**: `TcStep` parameters, the 27-signal array, status/error codes, semantics |
| `LABVIEW_INTEGRATION.md` | CLFN settings, RT loop sketch with CanTp, cRIO/Pi deployment, troubleshooting |
| `TEMPCTL-SPEC-v2.0.1.md` | The controller requirements as implemented (R-numbered, sent for markup 2026-09-14) |
| `TESTING.md` | What the gates cover and how to re-run them |
| `CHANGELOG.md`, `LICENSE.txt` | Release history, MIT license |
| `examples\oracle_test.py` | ctypes example of the full chain: `TcStep` → `CanTp_PackSgl` → frames → `CanTp_Unpack`, checked against cantools |
| `tools\make_tempctl_dbc.py` | Generator of `tempctl.dbc` (the only place the message layout is defined) |
| `tools\elfinfo.py` | ELF inspector used for `DEPENDENCIES.txt` |
| `src\` | Complete C source, the test program and the build script |

No runtime dependencies: the DLLs link the CRT statically and import only
`KERNEL32.dll`; the `.so` files import only `memcpy`/`memset` from libc
(GLIBC 2.14 symbols).

## Quick start

**Verify on your machine**

```bat
test_tempctl.exe                      -> 181 passed, 0 failed
```

**cRIO-9045 (x86_64)**

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio-ip>:/usr/local/lib/
scp linux-x64\test_tempctl admin@<crio-ip>:/home/admin/
ssh admin@<crio-ip> "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl && /home/admin/test_tempctl"
```

**Raspberry Pi (aarch64)**: same with the `linux-arm64\` files. The TempSim
package's `linux-arm64` console simulator runs there directly and can put
the frames on a real CAN bus.

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
do the reverse on the receiving side. The CanTp package guide has the CLFN
tables; the LabVIEW side may also use `CanTp_TransferXnet` (CanTp ≥ 1.3.0)
to unflatten straight into XNET Write.

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
- **Separate packages.** The controller (this package), the simulator
  (TempSim) and the transport (CanTp) are released on their own version
  lines; each is re-released only when its own code changes.

## Known limits

- The x86_64 `.so` was cross-compiled and inspected (ELF machine, exports,
  GLIBC needs) but **not executed on a cRIO** in this release; the identical
  source was executed on the Raspberry Pi (aarch64) with the same 181 checks
  and the same simulator outputs as Windows. Run `linux-x64\test_tempctl` on
  the target as the first step.
- CanTp transports used here: J1939 BAM. The controller message needs none
  of CanTp's RTS/CTS, ISO-TP, CAN FD, ECD-cluster or XNET-frame features.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. No NI or Vector
code is used or linked; the CAN encoding follows the public J1939-21 and
NI-XNET raw-frame specifications through CanTp.
