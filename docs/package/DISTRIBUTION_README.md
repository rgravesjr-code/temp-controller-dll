# TempCtl - Distribution Package v3.0.0

Deadband temperature controller with sensor validation for LabVIEW (Call
Library Function Node) as a Windows DLL and Linux shared libraries. v3.0.0
is the API and behaviour revision decided with the system owner
(`TEMPCTL-SPEC-v3.0.0.md`, `TEMPCTL-CAPABILITY-v3.0.0.md`): `TcInit` /
`TcCheckTemp` / `TcReset` / `TcGetDiag` replace the single `TcStep` of v2.

The controller does no CAN. Its 25-value diagnostics array is defined as a
J1939 message in a DBC and can be transported by the separate **CanTp**
library (the subset TempCtl needs is included, unmodified). The closed-loop
simulator **TempSim** is a separate package (`TempSim_v2.0.0_win-x64.zip`,
`_linux-x64.zip`, `_linux-arm64.zip`); it is not needed to use the
controller.

## Files in this package

| File | Purpose |
|---|---|
| `tempctl.dll`, `tempctl.lib` | The controller, **Windows x64** (64-bit LabVIEW, Python, .NET) |
| `x86\tempctl.dll`, `x86\tempctl.lib` | The same library built **x86** for 32-bit LabVIEW |
| `linux-x64\libtempctl.so` | **NI Linux RT x86_64** (cRIO-904x/905x/906x, incl. cRIO-9045) |
| `linux-arm64\libtempctl.so` | **aarch64 Linux** (Raspberry Pi 4/5) |
| `linux-*\test_tempctl`, `test_tempctl.exe`, `x86\test_tempctl.exe` | Release-gate test for each target (`1516 passed, 0 failed`) |
| `tempctl.h` | C header for the LabVIEW Import Shared Library wizard. The same file for the Windows `.dll` and the cRIO `.so` (fixed-width integers and `double` only), so one set of wrapper VIs serves both; a second copy sits under `src\` |
| `third_party\cantp\` | **CanTp v1.3.1**, the subset TempCtl needs, unmodified: `cantp.h`, `cantp.dll` (x64, `x86\`), `libcantp.so` (`linux-x64\`, `linux-arm64\`, `linux-armhf\`), `tools\dbc2tables.py`, `LICENSE.txt`. `VENDORED.txt` has the version, the zip hash and the dependency rule (minimum CanTp 1.0.0) |
| `dbc\tempctl.dbc` | The diagnostics message: PGN 65280, 25 signals in `TcGetDiag` order, 55 bytes, J1939 BAM, with status and warning value tables |
| `dbc\tables\TempCtl.*.csv`, `.json`, `cantp_tables.h` | The same message as CanTp tables: CSV for LabVIEW (`Read Delimited Spreadsheet` -> `CanTp_Define`), JSON (what the simulator loads), C header |
| `TESTLOG.txt` | Windows gates, DBC check, Python oracle and the Raspberry Pi run, captured at package time |
| `DEPENDENCIES.txt` | Import report of the native libraries (no VC runtime; .so on libc only) |
| `MANIFEST.txt` | File list with SHA-256 hashes |
| `TEMPCTL_PACKAGE_GUIDE.md` | **API reference**: the four calls with CLFN tables, setup and diagnostics indexes, status and warning codes |
| `LABVIEW_INTEGRATION.md` | Import wizard notes, CLFN settings, RT loop sketch with CanTp, the `RelayFeedbackTimeout` latency rule, the leaky accumulator, deployment, troubleshooting |
| `TEMPCTL-SPEC-v3.0.0.md` | The controller requirements as implemented (R-numbered, with the implementation decisions listed for markup) |
| `TEMPCTL-CAPABILITY-v3.0.0.md` | The operating rules in prose (the capability document) |
| `TESTING.md` | What the gates cover and how to re-run them |
| `CHANGELOG.md`, `LICENSE.txt` | Release history, MIT license |
| `examples\oracle_test.py` | ctypes example of the full chain: `TcInit` / `TcCheckTemp` / `TcGetDiag` -> `CanTp_Pack` -> frames -> `CanTp_Unpack`, checked against cantools |
| `tools\make_tempctl_dbc.py` | Generator of `tempctl.dbc` (the only place the message layout is defined) |
| `tools\elfinfo.py` | ELF inspector used for `DEPENDENCIES.txt` |
| `src\` | Complete C source, the test program and the build script |

No runtime dependencies: the DLLs link the CRT statically and import only
`KERNEL32.dll`; the `.so` files import only `memset` from libc.

## Quick start

**Verify on your machine**

```bat
test_tempctl.exe                      -> TempCtl 3.0.0 unit tests: 1516 passed, 0 failed
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

## The controller calls

```c
int32_t TcInit(int32_t zone, uint32_t nowMs, const double* setupArray, int32_t setupLen,
               int32_t* status, int32_t* warning);                       /* setup: 17 DBL, once per zone + on change */
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs, double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB,
                    int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning);   /* every pass */
int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning);
int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);     /* 25 DBL, read-only */
```

`status` 0..5 are states (`TempCtrlDisabled`, `TempAtSetPt`, `HeaterON`,
`CoolerON`, `HeatPending`, `CoolPending`), 10..16 are faults (`Temp1FailHigh`,
`Temp1FailLow`, `BothSensorsFailed`, `TempDisagreeFault`, `ConfigFault`,
`HeaterFBFault`, `CoolerFBFault`); `warning` 0..7 is the lowest active
warning. Full tables in `TEMPCTL_PACKAGE_GUIDE.md`.

Sending the diagnostics on CAN is two CanTp calls: `CanTp_Define(slot, msg,
8, sig, 25)` once with the tables from `dbc\tables\`, then `CanTp_Pack(slot,
diag, 25, ts, spacing, frames, cap, &written)` -> 9 NI-XNET raw frame records
(one J1939 BAM) for XNET Write. `CanTp_Unpack` / `CanTp_RxFeed` do the
reverse on the receiving side.

## Design decisions in this release

- **No relay chatter (REQ-7).** Every relay transition is time-qualified:
  `DeadbandTimeout` to engage, `AtSetPtTimeout` to release. A countdown
  starts on the tick that first observes its condition and can expire only
  on a later tick.
- **Raw control, averaged comparison.** Limits and control act on the raw
  readings; the moving averages exist only for the sensor-1-vs-sensor-2
  comparison.
- **Leaky accumulator.** Out-of-range time accumulates and in-range time
  drains it at half rate (Amendment A of the handoff); a sensor fails when
  the accumulator reaches `ErrorTimeout`. A flickering sensor that is out of
  range more than a third of the time still fails (half the time: after
  about 4 x `ErrorTimeout`); an isolated glitch drains away. `ErrorTimeout`
  must be at least two loop periods.
- **Control pauses while the active sensor is out of range**: relays hold,
  countdowns freeze.
- **Two sensors, offset-corrected**: one failure with a healthy partner is a
  warning (`RunningOnTemp2`), not a fault; a sustained disagreement warns at
  a tenth of `TempCompareTimeout` and faults at the full time.
- **Per-relay DO feedback** with its own warning and fault; a one-pass
  mismatch at every transition is expected DO-loop lag.
- **Config check at Init**: `ConfigFault` (enabled) or `ConfigInvalid`
  (disabled); only a passing Init clears `ConfigFault`.
- **Faults are shutdown events**: relays off, status latched, warning and
  diagnostics frozen, first fault wins, until Reset or Init.
- **Never a DBC inside a DLL.** The diagnostics message is defined once
  (`tempctl.dbc`), converted to flat tables, loaded into CanTp with one call.
- **Separate packages.** The controller (this package), the simulator
  (TempSim) and the transport (CanTp) are released on their own version
  lines; each is re-released only when its own code changes.

## Known limits

- The x86_64 `.so` was cross-compiled and inspected (ELF machine, exports,
  GLIBC needs) but **not executed on a cRIO** in this release; the identical
  source was executed on the Raspberry Pi (aarch64) with the same 1516
  checks and the same simulator outputs as Windows. Run `linux-x64\test_tempctl`
  on the target as the first step.
- The coverage gaps of the specification (section 11): a physically stuck
  relay, a heater that never reaches the setpoint, a wrong-but-in-range
  survivor after a failover, a single visible warning, no separate process
  over-temperature limit.
- The LabVIEW Import Shared Library wizard run is the LabVIEW side's
  acceptance step; the header was written to its constraints but the wizard
  itself was not run here.
- CanTp transports used here: J1939 BAM only.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. No NI or Vector
code is used or linked; the CAN encoding follows the public J1939-21 and
NI-XNET raw-frame specifications through CanTp.
