# TempCtl - Distribution Package v4.0.0

Deadband temperature controller with sensor validation and an explicit
Start / Stop / run-permissive lifecycle for LabVIEW (Call Library Function
Node) as a Windows DLL and Linux shared libraries. v4.0.0 implements the
requirements-change handoff of 2026-09-22 (`TEMPCTL-SPEC-v4.0.0.md`,
`TEMPCTL-CAPABILITY-v4.0.0.md`): `TcStart` and `TcStop` are new, `TcCheckTemp`
takes the live `runPermissive`, the setup array grows to 18 values and the
diagnostics array to 28, Init and Reset leave the zone stopped. **Not
compatible with v3** wrapper VIs, array constants or CAN databases.

The controller does no CAN. Its 28-value diagnostics array is defined as a
44-byte J1939 message in a DBC and an ECD and can be transported by the
separate **CanTp** library (the subset TempCtl needs is included,
unmodified). The closed-loop simulator **TempSim** is a separate package
(`TempSim_v3.0.0_win-x64.zip`, `_linux-x64.zip`, `_linux-arm64.zip`); it is
not needed to use the controller.

## Files in this package

| File | Purpose |
|---|---|
| `tempctl.dll`, `tempctl.lib`, `tempctl.h` | The controller, **Windows x64** (64-bit LabVIEW, Python, .NET) |
| `x86\tempctl.dll`, `x86\tempctl.lib`, `x86\tempctl.h` | The same library built **x86** for 32-bit LabVIEW 2026 |
| `linux-x64\libtempctl.so`, `linux-x64\tempctl.h` | **NI Linux RT x86_64** (Intel cRIO-904x/905x) |
| `linux-armhf\libtempctl.so`, `linux-armhf\tempctl.h` | **NI Linux RT 32-bit ARM** (myRIO-1900, Cortex-A9, hard float) |
| `linux-arm64\libtempctl.so`, `linux-arm64\tempctl.h` | **aarch64 Linux** (Raspberry Pi 4/5) |
| `test_tempctl.exe`, `x86\test_tempctl.exe`, `linux-*\test_tempctl` | Release-gate test for each target (`2194 passed, 0 failed`) |
| `tempctl.h` (every folder, and `src\`) | C header for the LabVIEW Import Shared Library wizard. The same file next to every binary, byte-identical (fixed-width integers and `double` only), so one set of wrapper VIs serves all targets |
| `third_party\cantp\` | **CanTp v1.3.1**, the subset TempCtl needs, unmodified: `cantp.h`, `cantp.dll` (x64, `x86\`), `libcantp.so` (`linux-x64\`, `linux-armhf\`, `linux-arm64\`), `tools\dbc2tables.py`, `tools\ecdflat.py`, `LICENSE.txt`. `VENDORED.txt` has the version, the zip hash and the dependency rule (minimum CanTp 1.2.0 for `CanTp_DefineFlat`) |
| `dbc\tempctl.dbc` | The diagnostics message: PGN 65280, id `0x18FF00FE`, 28 signals in `TcGetDiag` order, 44 bytes, J1939 BAM, U16 millisecond signals, status and warning value tables |
| `dbc\tempctl.ecd` | The same message as an ECD database (one `J1939Msg(V4)` cluster, channels in `TcGetDiag` order) for `CanTp_DefineFlat`; generated from the same signal model and cross-checked |
| `dbc\tables\TempCtl.*.csv`, `.json`, `cantp_tables.h` | The same message as CanTp tables: CSV for LabVIEW (`Read Delimited Spreadsheet` -> `CanTp_Define`), JSON (what the simulator loads), C header |
| `TESTLOG.txt` | Windows gates, DBC / table / ECD regeneration check, Python oracle and the Linux target logs, captured at package time; states per Linux target whether the test was executed on hardware or only inspected |
| `DEPENDENCIES.txt` | Import report of the native libraries (no VC runtime; the `.so` files need at most libc) |
| `MANIFEST.txt` | File list with SHA-256 hashes (every header copy included) |
| `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md` | **Start here**: target matrix, every export, the arrays and codes, the lifecycle transition table, the LabVIEW import settings, deployment per target, common errors, call sequences |
| `TEMPCTL_PACKAGE_GUIDE.md` | API reference: the nine calls with CLFN tables, setup and diagnostics indexes, status and warning codes, CanTp packing |
| `LABVIEW_INTEGRATION.md` | Per-function CLFN tables, RT loop sketch with Start / Stop / permissive and CanTp, the `RelayFeedbackTimeout` latency rule, the leaky accumulator, deployment, troubleshooting |
| `TEMPCTL-SPEC-v4.0.0.md` | The controller requirements as implemented (R-numbered, R10 = the lifecycle, with the implementation decisions listed for markup) |
| `TEMPCTL-CAPABILITY-v4.0.0.md` | The operating rules in prose (the capability document) |
| `TESTING.md` | What the gates cover, the owner-executed LabVIEW / target gates, and how to re-run everything |
| `CHANGELOG.md`, `LICENSE.txt` | Release history with the v3 -> v4 migration section, MIT license |
| `examples\oracle_test.py` | ctypes example of the full chain: `TcInit` / `TcStart` / `TcCheckTemp` / `TcStop` / `TcGetDiag` -> `CanTp_Pack` -> frames -> `CanTp_Unpack`, tables and ECD, checked against cantools |
| `tools\make_tempctl_dbc.py` | Generator of `tempctl.dbc`, the tables and `tempctl.ecd` (the only place the message layout is defined) |
| `tools\elfinfo.py` | ELF inspector used for `DEPENDENCIES.txt`; run it on the target too |
| `src\` | Complete C source, the test program and the build script |

No runtime dependencies: the DLLs link the CRT statically and import only
`KERNEL32.dll`; the `.so` files import at most `memset` / `memcpy` from
libc (the 32-bit ARM library imports nothing).

## Quick start

**Verify on your machine**

```bat
test_tempctl.exe                      -> TempCtl 4.0.0 unit tests: 2194 passed, 0 failed
```

**Intel cRIO (x86_64)**

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@<crio-ip>:/usr/local/lib/
scp linux-x64\test_tempctl admin@<crio-ip>:/home/admin/
ssh admin@<crio-ip> "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl && /home/admin/test_tempctl"
```

**myRIO-1900 (32-bit ARM)**: the same with the `linux-armhf\` files.
**Raspberry Pi (aarch64)**: the same with the `linux-arm64\` files; the
TempSim package's `linux-arm64` console simulator runs there directly.

## The controller calls

```c
int32_t TcInit(int32_t zone, uint32_t nowMs, const double* setupArray, int32_t setupLen,
               int32_t* status, int32_t* warning);                       /* setup: 18 DBL; leaves the zone IdleStopped */
int32_t TcStart(int32_t zone, uint32_t nowMs, int32_t runPermissive,
                int32_t* status, int32_t* warning);                      /* accepted, or refused (IdleStartBlocked) */
int32_t TcStop(int32_t zone, uint32_t nowMs, int32_t* doHeater, int32_t* doCooler,
               int32_t* status, int32_t* warning);                       /* 0, 0 at once, no fault */
int32_t TcCheckTemp(int32_t zone, uint32_t nowMs, double temp1, double temp2,
                    int32_t diHeaterFB, int32_t diCoolerFB, int32_t runPermissive,
                    int32_t* doHeater, int32_t* doCooler, int32_t* status, int32_t* warning);   /* every pass */
int32_t TcReset(int32_t zone, uint32_t nowMs, int32_t* status, int32_t* warning);   /* faults cleared, IdleStopped */
int32_t TcGetDiag(int32_t zone, double* diagArray, int32_t diagLen);     /* 28 DBL, read-only */
```

`status` 0..5 are the inert and running states (`TempCtrlDisabled`,
`TempAtSetPt`, `HeaterON`, `CoolerON`, `HeatPending`, `CoolPending`), 6..9
the stopped states (`IdleStopped`, `IdleStartBlocked`,
`OperatingConditionPending`, `IdleOperatingConditionTripped`), 10..17 the
faults (`Temp1FailHigh`, `Temp1FailLow`, `BothSensorsFailed`,
`TempDisagreeFault`, `ConfigFault`, `HeaterFBFault`, `CoolerFBFault`,
`OperatingConditionFault`); `warning` 0..8 is the lowest active warning.
Full tables in `TEMPCTL_PACKAGE_GUIDE.md`.

Sending the diagnostics on CAN is two CanTp calls: `CanTp_Define(slot, msg,
8, sig, 28)` once with the tables from `dbc\tables\` (or `CanTp_DefineFlat`
with the `tempctl.ecd` cluster), then `CanTp_Pack(slot, diag, 28, ts,
spacing, frames, cap, &written)` -> 8 NI-XNET raw frame records (one J1939
BAM) for XNET Write. `CanTp_Unpack` / `CanTp_RxFeed` do the reverse on the
receiving side.

## Design decisions in this release

- **Explicit lifecycle.** `TempCtrlEnable` permits a Start but starts
  nothing; `TcStart` and `TcStop` are the run commands, without faults or a
  configuration reload; Init and Reset leave the zone `IdleStopped`. Nothing
  restarts control except `TcStart`.
- **Run permissive.** The host combines its own operating conditions into
  one Boolean. A false permissive refuses a Start (`IdleStartBlocked`,
  warning 8, no countdown) and, while running, de-energizes both relays on
  the first sample that sees it (`OperatingConditionPending`); if it stays
  false for `OperatingConditionTimeout` the zone faults
  (`OperatingConditionFault`), otherwise it waits as
  `IdleOperatingConditionTripped` with the cause latched until a new Start.
  The timeout qualifies the fault, never the shutdown.
- **No relay chatter (REQ-7), narrowed.** No relay energizes, reverses or
  makes a process transition on one sample; only the de-energization by
  Stop or a lost permissive is immediate.
- **Stop before Init / Reset.** Both return no relay commands, so on a zone
  that may be active the host calls `TcStop` first and applies its zeros.
  The v3 relay keeping across a re-Init is gone.
- **Existing faults keep priority.** Same-tick order: config, sensor range,
  disagreement, heater feedback, cooler feedback, operating condition; a
  latched fault is never overwritten.
- **Raw control, averaged comparison; leaky accumulator (Amendment A);
  control pauses while the active sensor is out of range; two
  offset-corrected sensors with failover and a two-stage disagreement;
  per-relay DO feedback; config check at Init; faults latch and freeze the
  diagnostics** as in v3. Nothing is evaluated while a zone is stopped
  (the raw readings are still mirrored).
- **Compact wire format.** The 28 diagnostics are a 44-byte message; the
  eight millisecond values are U16 and saturate at 64255 on the wire while
  the array keeps the full value. DBC, CanTp tables and ECD come from one
  generator and are cross-checked.
- **One header everywhere.** The same `tempctl.h` next to every binary,
  hashed in the manifest.
- **Separate packages.** The controller (this package), the simulator
  (TempSim) and the transport (CanTp) are released on their own version
  lines; each is re-released only when its own code changes.

## Known limits

- cRIO-906x is 32-bit ARM, not x86-64. Do not use `linux-x64` there;
  the supplied myRIO `linux-armhf` build needs separate validation on a
  906x image. TempSim has no 32-bit ARM package.

- The Intel cRIO and the myRIO-1900 binaries were cross-compiled and
  inspected (ELF class, exports, imports, SONAME); `TESTLOG.txt` states
  whether each was executed on its target in this release. The identical
  source was executed on Windows x64 / x86 and on the Raspberry Pi
  (aarch64) with the same 2194 checks and the same simulator outputs as
  Windows. Run `linux-*\test_tempctl` on your target as the first step.
- The LabVIEW Import Shared Library wizard run and the LabVIEW RT wrapper
  tests are the LabVIEW side's acceptance steps (parser settings and the
  smoke sequence are documented); they were not run here.
- Nothing is evaluated while a zone is stopped: a sensor that fails while
  `IdleStopped` is found after the next Start.
- The coverage gaps of the specification (section 11): a physically stuck
  relay, a heater that never reaches the setpoint, a wrong-but-in-range
  survivor after a failover, a single visible warning, no separate process
  over-temperature limit, no wall-clock fault time, no per-condition reason
  bits, no automatic restart.
- CanTp transports used here: J1939 BAM only.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. No NI or Vector
code is used or linked; the CAN encoding follows the public J1939-21 and
NI-XNET raw-frame specifications through CanTp.
