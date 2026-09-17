# TempCtl v3 - LabVIEW and cRIO integration guide

How to wire the controller into a LabVIEW RT application on a cRIO-9045 (or
any x64 NI Linux RT target) and into a Windows host VI, and how to put its
diagnostics on CAN with CanTp. The API tables are in
`TEMPCTL_PACKAGE_GUIDE.md`; CanTp's own CLFN tables are in the CanTp package
(`CANTP_PACKAGE_GUIDE.md`, `LABVIEW_INTEGRATION.md`); this package carries
only CanTp's header and binaries under `third_party\cantp\`.

## 1. Files

| Target | File in this package | Where it goes |
|---|---|---|
| cRIO-904x/905x/906x (x64 NI Linux RT) | `linux-x64\libtempctl.so`, `third_party\cantp\linux-x64\libcantp.so` | `/usr/local/lib/` on the target |
| cRIO self-test | `linux-x64\test_tempctl` (and `linux-x64\test_cantp` from the CanTp package) | anywhere on the target, run once each |
| Raspberry Pi 4/5 (aarch64) | `linux-arm64\...`, `third_party\cantp\linux-arm64\...` | same |
| Windows 64-bit LabVIEW | `tempctl.dll`, `third_party\cantp\cantp.dll` | next to the VI/EXE, or a folder on PATH |
| Windows 32-bit LabVIEW | `x86\tempctl.dll`, `third_party\cantp\x86\cantp.dll` | same |
| Message tables | `dbc\tables\TempCtl.msg.csv`, `TempCtl.sig.csv`, `TempCtl.names.txt` | with the VI (read at start-up) |
| C/C++ development | `tempctl.h`, `tempctl.lib`, `third_party\cantp\cantp.h`, `cantp.lib` | your project |

Nothing else is needed at runtime: no VC++ redistributable, no other DLLs,
no libraries on the target beyond the OS's libc.

## 2. Deploy to the cRIO

From a Windows command prompt with the target's IP address (SSH enabled in
NI MAX -> System Settings -> Enable Secure Shell):

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@192.168.1.10:/usr/local/lib/
scp linux-x64\test_tempctl admin@192.168.1.10:/home/admin/
ssh admin@192.168.1.10 "chmod 755 /usr/local/lib/lib*.so /home/admin/test_tempctl; /home/admin/test_tempctl"
```

Expected last line: `TempCtl 3.0.0 unit tests: 1516 passed, 0 failed`. The
CanTp package has the matching `linux-x64\test_cantp`; run it the same way.

**Which .so?** cRIO-904x/905x/906x are x86_64 -> `linux-x64`. A Raspberry
Pi is aarch64 -> `linux-arm64`. Both are built from the same source and the
same 1516 checks pass on both.

## 3. Importing the header (Import Shared Library wizard)

`tempctl.h` is written for **Tools -> Import -> Shared Library (.dll)**: the
signatures use only `int32_t`, `uint32_t` and `double`, arrays are a pointer
plus an `int32_t` length, and every index and code is a plain `#define`. Point
the wizard at `tempctl.dll` and `tempctl.h`, select all seven functions,
and on the parameter pages mark `setupArray` (TcInit) and `diagArray`
(TcGetDiag) as **Array, 1 dimension, Array Data Pointer** with `setupLen` /
`diagLen` as their length; every other parameter is imported as the wizard
proposes (`int32_t*` outputs as *Pointer to Value*). No other correction is
needed. `tempctl.h` is the same file for the Windows `.dll` and the cRIO
`.so` (only fixed-width integers and `double`, nothing platform-specific),
so the one set of wrapper VIs the wizard generates serves both: test on the
desktop DLL, deploy the `.so` with the library path changed to
`/usr/local/lib/libtempctl.so`, nothing regenerated.

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
| 4 | setupLen | Numeric | Signed 32-bit | Value (17) |
| 5 | status | Numeric | Signed 32-bit | Pointer to Value |
| 6 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.2 TcCheckTemp

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | nowMs | Numeric | Unsigned 32-bit | Value |
| 3 | temp1 | Numeric | 8-byte Double | Value |
| 4 | temp2 | Numeric | 8-byte Double | Value |
| 5 | diHeaterFB | Numeric | Signed 32-bit | Value |
| 6 | diCoolerFB | Numeric | Signed 32-bit | Value |
| 7 | doHeater | Numeric | Signed 32-bit | Pointer to Value |
| 8 | doCooler | Numeric | Signed 32-bit | Pointer to Value |
| 9 | status | Numeric | Signed 32-bit | Pointer to Value |
| 10 | warning | Numeric | Signed 32-bit | Pointer to Value |

### 3.3 TcReset

zone (I32, Value), nowMs (U32, Value), status (I32, Pointer to Value),
warning (I32, Pointer to Value).

### 3.4 TcGetDiag

zone (I32, Value), diagArray (Array, 8-byte Double, 1-D, Array Data
Pointer, pre-sized with `Initialize Array` to 25), diagLen (I32, Value, 25).

### 3.5 TcVersion, TcSetupCount, TcDiagCount

No parameters, return I32. Use them at start-up to size the arrays and to
display the library version.

`nowMs`: the **Tick Count (ms)** primitive. Do not pass a timed-loop
iteration count multiplied by the period; the library needs real elapsed
time because other states run between visits. A wrong or restarted timer
that steps backwards is harmless (counts as 0 ms elapsed).

## 4. RT loop sketch

```
[Init, once per zone]
    setup = 17-element DBL array from the zone's configuration cluster:
            TempCtrlEnable, TempUnits, Setpoint, DeadbandHi, DeadbandLo, HiLimit, LoLimit,
            ErrorTimeout, DeadbandTimeout, AtSetPtTimeout, Temp2Enable, Temp2Offset,
            Temp2Tolerance, TempCompareTimeout, FilterPoints, FeedbackEnable, RelayFeedbackTimeout
    TcInit(zone, Tick Count, setup, 17, &status, &warning)
    status = 14 -> ConfigFault: fix the setup and Init again (Reset does not clear it)
    msg = Read Delimited Spreadsheet(TempCtl.msg.csv) -> 1x8 DBL -> Reshape (8)
    msg[4] = own source address (e.g. 0x80)          ; replaces the DBC's 0xFE placeholder
    sig = Read Delimited Spreadsheet(TempCtl.sig.csv) -> 25x8 DBL -> Reshape (200)
    CanTp_Define(0, msg, 8, sig, 25)
    cap = CanTp_OutputSize(0)                         ; 216

[Controller state, every pass of the main loop (~100 ms)]
    per zone:
      TcCheckTemp(zone, Tick Count, thermocouple 1 (DBL; NaN if open), thermocouple 2,
                  heater DO read-back (0/1), cooler DO read-back (0/1),
                  &doHeater, &doCooler, &status, &warning)
      heater DO <- doHeater ; cooler DO <- doCooler      (parallel DO loop)
      status >= 10 -> fault -> stand shutdown; status text from the table
      warning     -> HMI text (0 = none)
    display / logging / CAN, any rate:
      TcGetDiag(zone, diag, 25)
      CanTp_Pack(0, diag, 25, ts, spacing, frames, cap, &n)
      XNET Write (Frame Output Stream, raw)  <- frames[0..n)

[Parameter change from the HMI]
    TcInit(zone, Tick Count, new setup, 17, &status, &warning)   ; a running zone keeps its relays

[Operator reset after a fault]
    TcReset(zone, Tick Count, &status, &warning)
```

**Zones.** One call per zone per pass; zones share nothing. An unused zone is
simply never initialised (it returns status 0 if called).

**`RelayFeedbackTimeout` and the DO loop.** The read-back compares with the
command of the *previous* `TcCheckTemp`. Because the DO is driven by a
parallel loop, the read-back lags the command by that loop's period plus the
module's update time; every relay transition therefore shows a one-pass
`HeaterFBMismatch` / `CoolerFBMismatch` warning, which is normal. Set
`RelayFeedbackTimeout` longer than the worst-case latency of the DO loop
(several main-loop periods is a safe choice) so that lag never becomes a
`HeaterFBFault` / `CoolerFBFault`.

**The leaky accumulator.** A sensor does not fail because it was out of
range for `ErrorTimeout` in one go; every out-of-range pass adds its elapsed
time to `TempxOorAccumMs` and every in-range pass subtracts half of its
elapsed time. A thermocouple that flickers in and out of range therefore
still fails once it is out more than a third of the time (out all the time:
at `ErrorTimeout`; three quarters: 1.6 x; half: about 4 x), instead of
resetting a countdown each time it recovers; an isolated glitch drains away
within two passes and never gets near the limit. `TcGetDiag` shows the
accumulator and the `TempxOorEventsPerHour` transition count so a noisy
sensor can be spotted before it fails.

**`ErrorTimeout` floor.** The accumulator charges on the very first
out-of-range pass. Set `ErrorTimeout` to at least **two loop periods**
(200 ms at a 100 ms loop): a shorter value lets one out-of-range reading
reach the threshold on the pass it is first seen, failing the sensor and, in
single-sensor mode, dropping both relays from a single sample. TempCtl does
not know the loop rate and cannot check this for you.

**Pacing the BAM.** J1939-21 expects 50-200 ms between TP.DT packets. Either
enable XNET Session -> Intf:Output Stream Timing = Replay Exclusive and give
the frames real timestamps via `timestamp100ns`/`spacing100ns`, or write one
24-byte record per 50 ms tick from a small queue. With a 50 ms spacing a
message takes about 450 ms, so a send period of 1 s (`GenMsgCycleTime` in
the DBC) is a sensible default; the controller itself runs at the loop rate.

**XNET timestamp.** The raw-frame timestamp counts 100 ns ticks since
1601-01-01 00:00 UTC. LabVIEW's absolute timestamp counts seconds since
1904-01-01, so `timestamp100ns = (LabVIEW timestamp as DBL + 9561628800) x 1e7`.
Pass 0 when you only stream to the bus.

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
reassembles BAM and the 25 diagnostics appear by name with units and value
tables (status and warning names). On a LabVIEW or C receiver use CanTp with
the same tables: `CanTp_RxFeed(slot, record, 24, values, 25)` per received
frame returns 1 when a message completed.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| LabVIEW error 7 (file not found) on RT | .so not at the path in the CLFN, or not executable. `ls -l /usr/local/lib/libtempctl.so`, `chmod 755` |
| Error 13 / "not a valid Win32 application" | 32-bit LabVIEW loading the x64 DLL or vice versa. Use `x86\` for 32-bit LabVIEW |
| Return -1 | `setupLen` not 17, `diagLen` below 25, or an unwired pointer output. Size from `TcSetupCount` / `TcDiagCount` |
| Return -2 | zone outside 0..15 |
| Status 0 although the zone should run | `TempCtrlEnable` is 0, or `TcInit` was never called for that zone (`ZoneInitialized` in the diagnostics) |
| Status 14 right after Init | Config check: units not 0/1, negative deadband, both deadbands 0, setpoint or band outside the limits, a timeout below 1 ms or NaN. Only a passing Init clears it |
| Warning 7 | The same with `TempCtrlEnable = 0`: the setup is wrong but the zone is off anyway |
| Status 10/11 | Single-sensor mode: sensor 1 failed (accumulated out-of-range time reached `ErrorTimeout`). Fix the sensor, then Reset |
| Status 12 | Two-sensor mode: both sensors failed. Reset after repair |
| Status 13 | The two averages disagreed by more than `Temp2Tolerance` for `TempCompareTimeout`; check `Temp2Offset` and the sensor placement |
| Status 15/16 | DO read-back disagreed with the command for `RelayFeedbackTimeout`: wiring, module, or the timeout is shorter than the DO-loop latency |
| Warning 3/4 for one pass at every relay change | Normal DO-loop lag (section 4); raise `RelayFeedbackTimeout` if it ever turns into a fault |
| Warning 6 | Sensor 1 failed and control runs on sensor 2. No fault; the survivor is not cross-checked. Reset after repair |
| Relays hold while a sensor reads NaN | Expected: control pauses (R5.7) until the reading returns or the sensor fails |
| Heater never turns on | ControlTemp not below `LoBand` for the full `DeadbandTimeout`, a latched fault, or the zone is disabled |
| Relay stays on past the setpoint | It releases only after `AtSetPtTimeout` of continuous at-setpoint; with a fast plant lower the timeout or widen the band |
| Frames appear but the receiver shows no PGN 65280 | The receiver does not reassemble BAM. Check that both `1CECFFxx` and `1CEBFFxx` frames arrive 50-200 ms apart |
| CanTp returns -6 | `out` too small: use `CanTp_OutputSize(slot)` (216) |
