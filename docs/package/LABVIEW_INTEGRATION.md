# TempCtl v4 - LabVIEW, cRIO and myRIO integration guide

How to wire the controller into a LabVIEW RT application on an Intel cRIO
(x64 NI Linux RT) or a myRIO-1900 (32-bit ARM NI Linux RT) and into a
Windows host VI, and how to put its diagnostics on CAN with CanTp. The API
tables are in `TEMPCTL_PACKAGE_GUIDE.md`, the lifecycle, the wizard settings
and the deployment matrix in `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`;
CanTp's own CLFN tables are in the CanTp package. This package carries only
CanTp's header, binaries and two tools under `third_party\cantp\`.

## 1. Files

| Target | File in this package | Where it goes |
|---|---|---|
| Intel cRIO-904x/905x (x64 NI Linux RT) | `linux-x64\libtempctl.so`, `third_party\cantp\linux-x64\libcantp.so` | `/usr/local/lib/` on the target |
| NI myRIO-1900 (32-bit ARM NI Linux RT) | `linux-armhf\libtempctl.so`, `third_party\cantp\linux-armhf\libcantp.so` | `/usr/local/lib/` on the target |
| Target self-test | `linux-x64\test_tempctl` or `linux-armhf\test_tempctl` | anywhere on the target, run once |
| Raspberry Pi 4/5 (aarch64) | `linux-arm64\...`, `third_party\cantp\linux-arm64\...` | same |
| Windows 64-bit LabVIEW | `tempctl.dll`, `third_party\cantp\cantp.dll` | next to the VI/EXE, or a folder on PATH |
| Windows 32-bit LabVIEW | `x86\tempctl.dll`, `third_party\cantp\x86\cantp.dll` | same |
| Message definition | `dbc\tables\TempCtl.msg.csv`, `TempCtl.sig.csv`, `TempCtl.names.txt`, or `dbc\tempctl.ecd` | with the VI (read at start-up) |
| C/C++ development | `tempctl.h`, `tempctl.lib`, `third_party\cantp\cantp.h`, `cantp.lib` | your project |

Nothing else is needed at runtime: no VC++ redistributable, no other DLLs,
no libraries on the target beyond the OS's libc. The same `tempctl.h` sits
in every binary folder.

## 2. Deploy to the target

From a Windows command prompt with the target's IP address (SSH enabled in
NI MAX -> System Settings -> Enable Secure Shell). Intel cRIO:

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@192.168.1.10:/usr/local/lib/
scp linux-x64\test_tempctl admin@192.168.1.10:/home/admin/
ssh admin@192.168.1.10 "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl; /home/admin/test_tempctl"
```

myRIO-1900: the same three lines with `linux-armhf\` in place of
`linux-x64\`. Expected last line on either: `TempCtl 4.0.0 unit tests: 2194
passed, 0 failed`. The CanTp package has the matching `test_cantp`.

**Which .so?** cRIO-904x/905x are x86_64 -> `linux-x64`. The
myRIO-1900 is a Xilinx Zynq-7010 (dual Cortex-A9, 32-bit ARM, hard float)
-> `linux-armhf`. A Raspberry Pi is aarch64 -> `linux-arm64`. All are built
from the same source; `file libtempctl.so` on the target confirms the
class. `TESTLOG.txt` says for each Linux target whether the test was
executed on real hardware in this release or only built and inspected.

The **cRIO-906x family is 32-bit ARM**, not Intel x86-64; do not deploy
`linux-x64` or the x64-only TempSim CLI to those controllers. The supplied
`linux-armhf` build targets myRIO-1900. Its compatibility with a particular
906x image/ABI still requires validation on that target; it is not certified
by the myRIO or Pi results. Check the exact model and target architecture.
NI documents the [cRIO-9068 ARM Cortex-A9 processor](https://www.ni.com/en/shop/compactrio/what-are-compactrio-controllers/ni-compactrio-performance-controller--performance-and-throughput.html).

## 3. Importing the header (Import Shared Library wizard)

`tempctl.h` is written for **Tools -> Import -> Shared Library (.dll)**: the
signatures use only `int32_t`, `uint32_t` and `double`, arrays are a pointer
plus an `int32_t` length, and every index and code is a plain `#define`. Point
the wizard at `x86\tempctl.dll` (32-bit LabVIEW) or `tempctl.dll` and
`tempctl.h`, with the include paths
`C:\Program Files (x86)\National Instruments\Shared\LVDB 2015\include\ansi`
then `...\include` and the preprocessor definition `_WIN32`, select all nine
functions, and on the parameter pages mark `setupArray` (TcInit) and
`diagArray` (TcGetDiag) as **Array, 1 dimension, Array Data Pointer** with
`setupLen` / `diagLen` as their length; every other parameter is imported as
the wizard proposes (`int32_t*` outputs as *Pointer to Value*). No other
correction is needed. `tempctl.h` is the same file for the Windows `.dll`
and every `.so`, so the one set of wrapper VIs the wizard generates serves
all targets: test on the desktop DLL, deploy the `.so` with the library
path changed to `/usr/local/lib/libtempctl.so`, nothing regenerated.

Manual CLFN settings, common to every function:

| Setting | Value |
|---|---|
| Library name or path (RT target) | `/usr/local/lib/libtempctl.so` (and `libcantp.so`) |
| Library name or path (Windows) | `tempctl.dll` / `cantp.dll` (relative to the VI) or a full path |
| Thread | Run in any thread (no allocation, I/O or blocking inside) |
| Calling convention | **C** |
| Error checking | Default |
| Return type | Numeric, Signed 32-bit Integer (`CanTp_Version`: Unsigned 32-bit) |

### 3.1 TcInit

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | nowMs | Numeric | Unsigned 32-bit | Value |
| 3 | setupArray | Array | 8-byte Double, 1 dimension | Array Data Pointer |
| 4 | setupLen | Numeric | Signed 32-bit | Value (18) |
| 5 | status | Numeric | Signed 32-bit | Pointer to Value |
| 6 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.2 TcStart

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | nowMs | Numeric | Unsigned 32-bit | Value |
| 3 | runPermissive | Numeric | Signed 32-bit | Value (Boolean To (0,1)) |
| 4 | status | Numeric | Signed 32-bit | Pointer to Value |
| 5 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.3 TcStop

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | nowMs | Numeric | Unsigned 32-bit | Value |
| 3 | doHeater | Numeric | Signed 32-bit | Pointer to Value |
| 4 | doCooler | Numeric | Signed 32-bit | Pointer to Value |
| 5 | status | Numeric | Signed 32-bit | Pointer to Value |
| 6 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.4 TcCheckTemp

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | nowMs | Numeric | Unsigned 32-bit | Value |
| 3 | temp1 | Numeric | 8-byte Double | Value |
| 4 | temp2 | Numeric | 8-byte Double | Value |
| 5 | diHeaterFB | Numeric | Signed 32-bit | Value |
| 6 | diCoolerFB | Numeric | Signed 32-bit | Value |
| 7 | runPermissive | Numeric | Signed 32-bit | Value (Boolean To (0,1)) |
| 8 | doHeater | Numeric | Signed 32-bit | Pointer to Value |
| 9 | doCooler | Numeric | Signed 32-bit | Pointer to Value |
| 10 | status | Numeric | Signed 32-bit | Pointer to Value |
| 11 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.5 TcReset

zone (I32, Value), nowMs (U32, Value), status (I32, Pointer to Value),
warning (I32, Pointer to Value).

### 3.6 TcGetDiag

zone (I32, Value), diagArray (Array, 8-byte Double, 1-D, Array Data
Pointer, pre-sized with `Initialize Array` to 28), diagLen (I32, Value, 28).

### 3.7 TcVersion, TcSetupCount, TcDiagCount

No parameters, return I32. Use them at start-up to size the arrays and to
refuse a mismatched library (a v3 DLL returns 0x030000 / 17 / 25).

`nowMs`: the **Tick Count (ms)** primitive. Do not pass a timed-loop
iteration count multiplied by the period; the library needs real elapsed
time because other states run between visits. A wrong or restarted timer
that steps backwards is harmless (counts as 0 ms elapsed).

**Serialize per zone.** Chain the wrapper VIs of one zone with dataflow
(error cluster or a sequence) so `TcGetDiag` never overlaps that zone's
`TcCheckTemp`, `TcStart` or `TcStop`. Different zones may run in parallel.

## 4. RT loop sketch

```
[Init, once per zone]
    setup = 18-element DBL array from the zone's configuration cluster:
            TempCtrlEnable, TempUnits, Setpoint, DeadbandHi, DeadbandLo, HiLimit, LoLimit,
            ErrorTimeout, DeadbandTimeout, AtSetPtTimeout, Temp2Enable, Temp2Offset,
            Temp2Tolerance, TempCompareTimeout, FilterPoints, FeedbackEnable, RelayFeedbackTimeout,
            OperatingConditionTimeout
    TcInit(zone, Tick Count, setup, 18, &status, &warning)          ; status 6 IdleStopped
    status = 14 -> ConfigFault: fix the setup and Init again (Reset does not clear it)
    msg = Read Delimited Spreadsheet(TempCtl.msg.csv) -> 1x8 DBL -> Reshape (8)
    msg[4] = own source address (e.g. 0x80)          ; replaces the DBC's 0xFE placeholder
    sig = Read Delimited Spreadsheet(TempCtl.sig.csv) -> 28x8 DBL -> Reshape (224)
    CanTp_Define(0, msg, 8, sig, 28)                  ; or CanTp_DefineFlat with the TempCtl cluster of tempctl.ecd
    cap = CanTp_OutputSize(0)                         ; 192

[Operator / application Start]
    perm = AND of the plant's operating conditions (pump running, flow ok, doors closed, mode ...)
    TcStart(zone, Tick Count, perm, &status, &warning)
    status 1 -> running; 7 -> blocked (perm false): show warning 8, Start again when ready

[Controller state, every pass of the main loop (~100 ms), started or not]
    per zone:
      perm = the same combined operating condition, live
      TcCheckTemp(zone, Tick Count, thermocouple 1 (DBL; NaN if open), thermocouple 2,
                  heater DO read-back (0/1), cooler DO read-back (0/1), perm,
                  &doHeater, &doCooler, &status, &warning)
      heater DO <- doHeater ; cooler DO <- doCooler      (parallel DO loop)
      status >= 10 -> fault -> stand shutdown; status text from the table
      status 8     -> permissive lost, relays off, countdown running (OperatingConditionRemainMs)
      status 9     -> permissive loss ended, waiting for Start
      warning      -> HMI text (0 = none)
    display / logging / CAN, any rate, same-zone serialized:
      TcGetDiag(zone, diag, 28)
      CanTp_Pack(0, diag, 28, ts, spacing, frames, cap, &n)
      XNET Write (Frame Output Stream, raw)  <- frames[0..n)

[Operator Stop]
    TcStop(zone, Tick Count, &doHeater, &doCooler, &status, &warning)   ; 0, 0: write them to the DOs
    status 6 (or 9 when a permissive trip was pending: the cause is kept)

[Parameter change from the HMI, zone possibly running]
    TcStop -> write 0/0 to the DOs -> TcInit(zone, Tick Count, new setup, 18, ...) -> TcStart(...)

[Operator reset after a fault]
    (the fault tick already returned 0/0) TcReset(zone, Tick Count, &status, &warning)   ; status 6
    fix the cause -> TcStart
[Operator reset of a running, non-faulted zone]
    TcStop -> write 0/0 -> TcReset -> TcStart
```

**Zones.** One call per zone per pass; zones share nothing. An unused zone is
simply never initialised (it returns status 0 if called).

**The run permissive.** Combine every plant condition on the host side into
one Boolean; TempCtl neither knows nor needs the individual conditions. The
first pass that sees it false turns both relays off and starts
`OperatingConditionTimeout`; if it stays false that long the zone faults
(17), otherwise the zone waits as status 9 until you Start it again. Nothing
restarts by itself. Set the timeout longer than the longest transient your
conditions can show (a pump spin-up, a door check) and at least two loop
periods.

**`RelayFeedbackTimeout` and the DO loop.** The read-back compares with the
command of the *previous* `TcCheckTemp`. Because the DO is driven by a
parallel loop, the read-back lags the command by that loop's period plus the
module's update time; every relay transition therefore shows a one-pass
`HeaterFBMismatch` / `CoolerFBMismatch` warning, which is normal. Set
`RelayFeedbackTimeout` longer than the worst-case latency of the DO loop
(several main-loop periods is a safe choice). The off transitions of Stop,
Init, Reset and a permissive loss are never a mismatch; a relay that is still
physically closed when you Start again is caught on the first pass.

**The leaky accumulator.** A sensor does not fail because it was out of
range for `ErrorTimeout` in one go; every out-of-range pass adds its elapsed
time to `TempxOorAccumMs` and every in-range pass subtracts half of its
elapsed time. A thermocouple that flickers in and out of range therefore
still fails once it is out more than a third of the time (out all the time:
at `ErrorTimeout`; three quarters: 1.6 x; half: about 4 x); an isolated
glitch drains away within two passes. Nothing accumulates while the zone
is stopped. `TcGetDiag` shows the accumulator and the
`TempxOorEventsPerHour` transition count so a noisy sensor can be spotted
before it fails.

**`ErrorTimeout` floor.** The accumulator charges on the very first
out-of-range pass. Set `ErrorTimeout` to at least **two loop periods**
(200 ms at a 100 ms loop): a shorter value lets one out-of-range reading
reach the threshold on the pass it is first seen.

**Pacing the BAM.** J1939-21 expects 50-200 ms between TP.DT packets. Either
enable XNET Session -> Intf:Output Stream Timing = Replay Exclusive and give
the frames real timestamps via `timestamp100ns`/`spacing100ns`, or write one
24-byte record per 50 ms tick from a small queue. With a 50 ms spacing the
8-frame message takes about 400 ms, so a send period of 1 s
(`GenMsgCycleTime` in the DBC) is a sensible default; the controller itself
runs at the loop rate.

**XNET timestamp.** The raw-frame timestamp counts 100 ns ticks since
1601-01-01 00:00 UTC. LabVIEW's absolute timestamp counts seconds since
1904-01-01, so `timestamp100ns = (LabVIEW timestamp as DBL + 9561628800) x 1e7`.
Pass 0 when you only stream to the bus.

**Safe state.** TempCtl acts only when called. Watchdogs and fail-safe
outputs when the application, the RT process or the loop stops remain the
host's and the I/O layer's job.

## 5. Writing a .ncl log from the RT side

```
open file, write CanTp_NclHeader() (12 bytes)
per message: write the U8 array from CanTp_Pack (with real timestamps)
```

The result opens in NI-XNET Bus Monitor (File -> Open Log). The TempSim
package's `examples\failover.ncl` is such a file, produced by the simulator,
with the matching `.csv` trace.

## 6. Receiving side

Load `dbc\tempctl.dbc` into CANalyzer / CANoe / Kvaser / any J1939 tool that
reassembles BAM and the 28 diagnostics appear by name with units and value
tables (status and warning names). On a LabVIEW or C receiver use CanTp with
the same tables or the ECD cluster: `CanTp_RxFeed(slot, record, 24, values,
28)` per received frame returns 1 when a message completed. A v3 receiver
(55-byte layout) cannot decode v4 frames.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| LabVIEW error 7 (file not found) on RT | .so not at the path in the CLFN, or not executable. `ls -l /usr/local/lib/libtempctl.so`, `chmod 755` |
| Error 13 / "not a valid Win32 application" | 32-bit LabVIEW loading the x64 DLL or vice versa. Use `x86\` for 32-bit LabVIEW |
| "wrong ELF class" / cannot execute on RT | x86-64 library on the myRIO or ARM library on the cRIO: `file libtempctl.so` |
| Return -1 | `setupLen` not 18, `diagLen` below 28, or an unwired pointer output. Size from `TcSetupCount` / `TcDiagCount`; a v3 wrapper (17 / 25) is incompatible |
| Return -2 | zone outside 0..15 |
| Status 0 although the zone should run | `TempCtrlEnable` is 0, or `TcInit` was never called for that zone (`ZoneInitialized` in the diagnostics) |
| Status 6 and no control | the zone is stopped (after Init / Reset / Stop): call `TcStart` |
| Status 7 | Start refused: the permissive was false. Fix the conditions and Start again; nothing auto-starts |
| Status 8 | the permissive dropped while running; both relays off; `OperatingConditionRemainMs` counts down to status 17 |
| Status 9 | the permissive loss ended (or you stopped); relays off; Start again |
| Status 17 | the permissive stayed false for `OperatingConditionTimeout` after a loss while running. Reset, then Start; lengthen the timeout if brief transients are expected |
| Status 14 right after Init | Config check: units not 0/1, negative deadband, both deadbands 0, setpoint or band outside the limits, a timeout below 1 ms or NaN (`OperatingConditionTimeout` included). Only a passing Init clears it |
| Warning 7 | The same with `TempCtrlEnable = 0`: the setup is wrong but the zone is off anyway |
| Warning 8 | The permissive is false while Start is blocked or the zone is pending / tripped; clears when it is true again |
| Status 10/11 | Single-sensor mode: sensor 1 failed (accumulated out-of-range time reached `ErrorTimeout`). Fix the sensor, then Reset and Start |
| Status 12 | Two-sensor mode: both sensors failed. Reset after repair |
| Status 13 | The two averages disagreed by more than `Temp2Tolerance` for `TempCompareTimeout`; check `Temp2Offset` and the sensor placement |
| Status 15/16 | DO read-back disagreed with the command for `RelayFeedbackTimeout`: wiring, module, or the timeout is shorter than the DO-loop latency |
| Warning 3/4 for one pass at every relay change | Normal DO-loop lag (section 4); raise `RelayFeedbackTimeout` if it ever turns into a fault |
| Warning 6 | Sensor 1 failed and control runs on sensor 2. No fault; the survivor is not cross-checked. Reset after repair (Stop / Start does not clear it) |
| Relays hold while a sensor reads NaN | Expected: control pauses (R5.7) until the reading returns or the sensor fails |
| Heater never turns on | ControlTemp not below `LoBand` for the full `DeadbandTimeout`, a latched fault, the zone is disabled, or it was never started |
| Relay stays on past the setpoint | It releases only after `AtSetPtTimeout` of continuous at-setpoint; with a fast plant lower the timeout or widen the band |
| Frames appear but the receiver shows no PGN 65280 | The receiver does not reassemble BAM. Check that both `1CECFFxx` and `1CEBFFxx` frames arrive 50-200 ms apart |
| Receiver decodes garbage after the temperatures | A v3 database (55 bytes) on v4 frames (44 bytes): load the v4 `tempctl.dbc` / `tempctl.ecd` |
| CanTp returns -6 | `out` too small: use `CanTp_OutputSize(slot)` (192) |
