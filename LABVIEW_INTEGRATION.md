# TempCtl v2 — LabVIEW and cRIO integration guide

How to wire the controller into a LabVIEW RT application on a cRIO-9045 (or
any x64 NI Linux RT target) and into a Windows host VI, and how to put its
state on CAN with CanTp. The API details are in `TEMPCTL_PACKAGE_GUIDE.md`;
CanTp's own CLFN tables are in `third_party\cantp\CANTP_PACKAGE_GUIDE.md`
and `LABVIEW_INTEGRATION.md`. This document is the "which file goes where
and what do I type in the CLFN" companion.

## 1. Files

| Target | File in this package | Where it goes |
|---|---|---|
| cRIO-904x/905x/906x (x64 NI Linux RT) | `linux-x64\libtempctl.so`, `third_party\cantp\linux-x64\libcantp.so` | `/usr/local/lib/` on the target |
| cRIO self-test | `linux-x64\test_tempctl`, `third_party\cantp\linux-x64\test_cantp` | anywhere on the target, run once each |
| Raspberry Pi 4/5 (aarch64) | `linux-arm64\...`, `third_party\cantp\linux-arm64\...` | same |
| Windows 64-bit LabVIEW | `tempctl.dll`, `third_party\cantp\cantp.dll` | next to the VI/EXE, or a folder on PATH |
| Windows 32-bit LabVIEW | `x86\tempctl.dll`, `third_party\cantp\x86\cantp.dll` | same |
| Message tables | `dbc\tables\TempCtl.msg.csv`, `TempCtl.sig.csv`, `TempCtl.names.txt` | with the VI (read at start-up) |
| C/C++ development | `tempctl.h`, `tempctl.lib`, `third_party\cantp\cantp.h`, `cantp.lib` | your project |

Nothing else is needed at runtime: no VC++ redistributable, no other DLLs,
no libraries on the target beyond the OS's libc.

## 2. Deploy to the cRIO

From a Windows command prompt with the target's IP address (SSH must be
enabled on the target in NI MAX → System Settings → Enable Secure Shell):

```bat
scp linux-x64\libtempctl.so third_party\cantp\linux-x64\libcantp.so admin@192.168.1.10:/usr/local/lib/
scp linux-x64\test_tempctl third_party\cantp\linux-x64\test_cantp admin@192.168.1.10:/home/admin/
ssh admin@192.168.1.10 "chmod 755 /usr/local/lib/lib*.so /home/admin/test_*; /home/admin/test_tempctl; /home/admin/test_cantp"
```

Expected last lines: `181 passed, 0 failed` and `177 passed, 0 failed`.
That proves both binaries load and behave identically on the target.

Alternatives: WebDAV (`\\192.168.1.10\files`) or the LabVIEW project's
"Files" view can copy the .so too; `chmod 755` is then done over SSH.
`/usr/local/lib` is on the default loader path; another folder works if you
give the CLFN the full path.

**Which .so?** cRIO-904x/905x/906x are x86_64 → `linux-x64`. A Raspberry Pi
is aarch64 → `linux-arm64`. They are not interchangeable; both are built
from the same source and the same test suite passes on both.

## 3. Call Library Function Node settings

Common to every function:

| Setting | Value |
|---|---|
| Library name or path (RT target) | `/usr/local/lib/libtempctl.so` (and `libcantp.so`) |
| Library name or path (Windows) | `tempctl.dll` / `cantp.dll` (relative to the VI) or a full path |
| Thread | Run in any thread (re-entrant; one zone per loop) |
| Calling convention | **C** |
| Error checking | Default |
| Return type | Numeric, **Signed 32-bit Integer** (`TcVersion`, `CanTp_Version`: Unsigned 32-bit) |

The RT and Windows copies of the VI differ only in the path. A common
pattern is one CLFN with a *Conditional Disable* structure, or simply
`tempctl.*` as the path with the .so also installed as
`/usr/local/lib/tempctl.so` (LabVIEW substitutes `.so` on Linux).

### 3.1 TcStep

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | zone | Numeric | Signed 32-bit | Value |
| 2 | action | Numeric | Signed 32-bit | Value |
| 3 | nowMs | Numeric | Unsigned 32-bit | Value |
| 4 | in | Array | 4-byte Single, 1 dimension | Array Data Pointer |
| 5 | inLen | Numeric | Signed 32-bit | Value |
| 6 | out | Array | 4-byte Single, 1 dimension | Array Data Pointer |
| 7 | outLen | Numeric | Signed 32-bit | Value |

Wire `Array Size` of `in` (17) to `inLen`. For `out`, wire an `Initialize
Array` (SGL 0, 27 elements) into the input terminal, `27` into `outLen`, and
read the output terminal. Simplest: keep **one 27-element SGL array in a
shift register**, write the 17 inputs into elements 0..16 with `Replace
Array Subset`, wire it into both `in` (inLen 27) and `out` (outLen 27); the
whole state then flows through the loop and the same wire feeds
`CanTp_PackSgl`.

`nowMs`: the **Tick Count (ms)** primitive. Do not pass a timed-loop
iteration count multiplied by the period; the library needs real elapsed
time because other states run between visits.

### 3.2 TcInputCount, TcSignalCount, TcVersion

No parameters. Return types I32, I32, U32. Use them at start-up to size
the arrays and to display the library version.

### 3.3 CanTp (from the CanTp package guide)

`CanTp_Define(slot I32, msgDef DBL 1-D ptr, msgDefLen I32, sigDefs DBL 1-D
ptr, nSig I32)` once; `CanTp_PackSgl(slot I32, values SGL 1-D ptr, nValues
I32, timestamp100ns U64, spacing100ns U64, out U8 1-D ptr, outLen I32,
bytesWritten I32 pointer-to-value)` every tick with the 27-element state
array as `values`. Pre-size `out` with `CanTp_OutputSize(slot)` (216 bytes
for the TempCtl message). See `third_party\cantp\LABVIEW_INTEGRATION.md`
for the full tables, `CanTp_Unpack` / `CanTp_RxFeed` and the raw-frame
XNET wiring.

## 4. RT loop sketch

```
[Init]
    state = Initialize Array (SGL 0, 27)
    state[0..10] = Setpoint, DeadbandHi, DeadbandLo, HiLimit, LoLimit,
                   ErrorTimeout, DeadbandTimeout, FilterPoints, Temp2Enable,
                   Temp2Tolerance, FeedbackEnable
    state[11..14] = first readings (Temp1, Temp2, HeaterFeedback, CoolerFeedback)
    TcStep(zone 0, action 0 Init, Tick Count, state, 27, state, 27)
    msg = Read Delimited Spreadsheet(TempCtl.msg.csv) -> 1x8 DBL -> Reshape (8)
    msg[4] = own source address (e.g. 0x80)          ; replaces the DBC's 0xFE placeholder
    sig = Read Delimited Spreadsheet(TempCtl.sig.csv) -> 27x8 DBL -> Reshape (216)
    CanTp_Define(0, msg, 8, sig, 27)
    cap = CanTp_OutputSize(0)                         ; 216

[Controller state, visited every N ms]
    state[11] = thermocouple 1 (SGL; NaN if open)
    state[12] = thermocouple 2 (or anything when Temp2Enable = 0)
    state[13] = heater relay auxiliary contact (0/1)
    state[14] = cooler relay auxiliary contact (0/1)
    rc = TcStep(0, 1 Step, Tick Count, state, 27, state, 27)
    heater DO = state[15] > 0.5
    cooler DO = state[16] > 0.5
    errors    = U16(state[17])        -> bits to HMI / interlocks (bit 6 disagree, 7/8 feedback ...)
    status    = I32(state[18])        -> HMI text (6 = stopped: needs operator Reset)
    if rc == 1 -> configuration warning (bit 9) -> HMI
    CanTp_PackSgl(0, state, 27, ts, spacing, frames, cap, &n)
    XNET Write (Frame Output Stream, raw)  <- frames[0..n)

[Operator reset]
    TcStep(0, 2 Reset, Tick Count, state, 27, state, 27)
```

Change the setpoint by writing `state[0]`; the bands follow it (offsets).
It takes effect at the next Step, no re-Init needed. Any of elements 0..10
may change at any time.

**Pacing the BAM.** J1939-21 expects 50–200 ms between TP.DT packets. Either
enable **XNET Session → Intf:Output Stream Timing = Replay Exclusive** and
give the frames real timestamps via `timestamp100ns`/`spacing100ns`, or write
one 24-byte record per 50 ms tick from a small queue. Do not blast all 9
frames back-to-back on a bus with other J1939 nodes. With a 50 ms spacing a
message takes about 450 ms, so a send period of 1 s (`GenMsgCycleTime` in
the DBC) is a sensible default; the controller itself may run much faster.

**XNET timestamp.** The raw-frame timestamp counts 100 ns ticks since
1601-01-01 00:00 UTC. LabVIEW's absolute timestamp counts seconds since
1904-01-01, so `timestamp100ns = (LabVIEW timestamp as DBL + 9561628800) × 1e7`.
Pass 0 when you only stream to the bus; XNET stamps received frames itself.

## 5. Writing a .ncl log from the RT side

```
open file, write CanTp_NclHeader() (12 bytes)
per tick: write the U8 array from CanTp_PackSgl (with real timestamps)
```

The result opens in **NI-XNET Bus Monitor** (File → Open Log).
`examples\out\sensor-failover.ncl` is such a file, produced by the
simulator, with the matching `.csv` trace.

## 6. Receiving side

Load `dbc\tempctl.dbc` into CANalyzer / CANoe / Kvaser / any J1939 tool that
reassembles BAM and the 27 signals appear by name with units and value
tables. On a LabVIEW or C receiver use CanTp with the same tables:
`CanTp_RxFeed(slot, record, 24, values, 27)` per received frame returns 1
when a message completed. The DBC's source address 0xFE means "any source":
the receiver decodes the message from every node unless it sets its own SA
override in the msgdef.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| LabVIEW error 7 (file not found) on RT | .so not at the path in the CLFN, or not executable. `ls -l /usr/local/lib/libtempctl.so`, `chmod 755` |
| Error 13 / "not a valid Win32 application" | 32-bit LabVIEW loading the x64 DLL or vice versa. Use `x86\` for 32-bit LabVIEW |
| Return −1 | `inLen < 17` or `outLen < 27`; size the arrays from `TcInputCount` / `TcSignalCount` |
| Return −4 | Step before Init on that zone. Init once per zone at startup |
| Return 1 / bit 9 | Configuration: negative deadband or timeout, `LoLimit < Setpoint − DeadbandLo` or `Setpoint + DeadbandHi < HiLimit` violated, FilterPoints > 64 |
| Status 6 (Stopped) | A sensor failed (Temp2 disabled) or both failed. Fix the sensor, then Reset (action 2) |
| Status 7 (Degraded) | Running on the backup sensor, or the backup is lost. The bits say which; Reset after repair |
| Status 8 (FilterWarmup) at start | Normal for the first FilterPoints ticks |
| Relays drop for one tick | A NaN reading on the active sensor (open thermocouple, module fault). The error countdown decides whether it is a fault |
| Heater never turns on | ControlTemp (filtered!) not below LoBand for the full DeadbandTimeout, a latched fault, or the plant already above Setpoint |
| Bit 7/8 set although the relay works | Feedback compared with the previous command: a slow contact plus a short ErrorTimeout. Raise ErrorTimeout above the relay's answer time |
| Frames appear but the receiver shows no PGN 65280 | The receiver does not reassemble BAM. Check that both `1CECFFxx` and `1CEBFFxx` frames arrive 50–200 ms apart |
| CanTp returns −6 | `out` too small: use `CanTp_OutputSize(slot)` (216) |
