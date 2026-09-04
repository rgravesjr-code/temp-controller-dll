# tempctl — LabVIEW and cRIO integration guide

This guide walks through wiring the library into a LabVIEW RT application on
a cRIO-9045 (or any x64 NI Linux RT target) and into a Windows host VI. The
API details are in `TEMPCTL_PACKAGE_GUIDE.md`; this document is the
"which file goes where and what do I type in the CLFN" companion.

## 1. Files

| Target | File in this package | Where it goes |
|---|---|---|
| cRIO-904x/905x/906x (x64 NI Linux RT) | `linux-x64\libtempctl.so` | `/usr/local/lib/libtempctl.so` on the target |
| cRIO self-test | `linux-x64\test_tempctl` | anywhere on the target, run once |
| Windows 64-bit LabVIEW | `tempctl.dll` | next to the VI/EXE, or a folder on PATH |
| Windows 32-bit LabVIEW | `x86\tempctl.dll` | same |
| C/C++ development | `tempctl.h`, `tempctl.lib`, `x86\tempctl.lib` | your project |

Nothing else is needed at runtime: no VC++ redistributable, no other DLLs,
no libraries on the cRIO beyond the OS's libc.

## 2. Deploy to the cRIO

From a Windows command prompt with the target's IP address (SSH must be
enabled on the target in NI MAX → System Settings → Enable Secure Shell):

```bat
scp linux-x64\libtempctl.so admin@192.168.1.10:/usr/local/lib/
scp linux-x64\test_tempctl  admin@192.168.1.10:/home/admin/
ssh admin@192.168.1.10 "chmod 755 /usr/local/lib/libtempctl.so /home/admin/test_tempctl && /home/admin/test_tempctl"
```

Expected last line: `154 passed, 0 failed`. That proves the binary loads and
the controller, BAM builder and packer behave identically on the target.

Alternatives: WebDAV (`\\192.168.1.10\files`) or the LabVIEW project's
"Files" view can copy the .so too; `chmod 755` is then done over SSH.
`/usr/local/lib` is on the default loader path; another folder works if you
give the CLFN the full path.

## 3. Call Library Function Node settings

Common to every function:

| Setting | Value |
|---|---|
| Library name or path (RT target) | `/usr/local/lib/libtempctl.so` |
| Library name or path (Windows) | `tempctl.dll` (relative to the VI) or a full path |
| Thread | Run in any thread (re-entrant; one zone per loop) |
| Calling convention | **C** |
| Error checking | Default |
| Return type | Numeric, **Signed 32-bit Integer** |

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

Wire `Array Size` of `in` to `inLen`. For `out`, wire an `Initialize Array`
(SGL 0, 13 elements) into the input terminal, `13` into `outLen`, and read
the output terminal. You may also wire the same array into both `in` and
`out` (in-place update), then the whole 11-element state simply flows
through a shift register.

`nowMs`: the **Tick Count (ms)** primitive. Do not pass a timed-loop
iteration count multiplied by the period; the library needs real elapsed
time because other states run between visits.

### 3.2 TcEncodeFrames

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | signals | Array | 4-byte Single, 1-D | Array Data Pointer |
| 2 | n | Numeric | Signed 32-bit | Value |
| 3 | pgn | Numeric | Unsigned 32-bit | Value |
| 4 | sa | Numeric | Unsigned 8-bit | Value |
| 5 | priority | Numeric | Unsigned 8-bit | Value |
| 6 | timestamp100ns | Numeric | Unsigned 64-bit | Value |
| 7 | spacing100ns | Numeric | Unsigned 64-bit | Value |
| 8 | out | Array | Unsigned 8-bit, 1-D | Array Data Pointer |
| 9 | outLen | Numeric | Signed 32-bit | Value |
| 10 | bytesWritten | Numeric | Signed 32-bit | Pointer to Value |

Pre-size `out` with `Initialize Array` (U8 0, 192 elements for 11 signals;
in general `TcJ1939BamFrameCount(n*4) * 24`). After the call use
`Array Subset` (0, bytesWritten) to trim.

`TcJ1939Bam` is identical except parameter 1 is `Unsigned 8-bit, 1-D` and
parameter 2 is its byte count. `TcJ1939BamFrameCount` takes one I32 and
returns I32.

### 3.3 TcCanPack

| Param | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | sigDefs | Array | 8-byte Double, 1-D | Array Data Pointer |
| 2 | nSig | Numeric | Signed 32-bit | Value |
| 3 | frameDefs | Array | 8-byte Double, 1-D | Array Data Pointer |
| 4 | nFrames | Numeric | Signed 32-bit | Value |
| 5 | values | Array | 4-byte Single, 1-D | Array Data Pointer |
| 6 | nValues | Numeric | Signed 32-bit | Value |
| 7 | timestamp100ns | Numeric | Unsigned 64-bit | Value |
| 8 | out | Array | Unsigned 8-bit, 1-D | Array Data Pointer |
| 9 | outLen | Numeric | Signed 32-bit | Value |
| 10 | bytesWritten | Numeric | Signed 32-bit | Pointer to Value |

Keep the database as a 2-D DBL array (`nSig × 9`) on the front panel or in
a config file and `Reshape Array` it to 1-D before the node; `nSig` is the
number of rows.

### 3.4 TcNclHeader, TcVersion

`TcNclHeader`: one `Unsigned 8-bit, 1-D` Array Data Pointer pre-sized to 12,
and an I32 `12`. `TcVersion`: no parameters, return type Unsigned 32-bit.

## 4. RT loop sketch

```
[Init state]
    cfg = {HiLimit, LoLimit, HiDb, LoDb, Setpoint, 0, ErrTO, DbTO, 0, 0, 0}
    TcStep(zone=0, action=0 Init, Tick Count, cfg, 11, state, 13)

[Temp controller state, visited every N ms]
    cfg[5] = thermocouple reading (SGL)
    rc = TcStep(0, 1 Step, Tick Count, cfg, 11, state, 13)
    heater DO  = state[9] > 0.5
    cooler DO  = state[8] > 0.5
    fault      = state[10]           (0/1/2/3)  -> HMI, interlocks
    if rc == 1 -> config error (setpoint outside deadband etc.) -> HMI
    TcEncodeFrames(state, 11, 0xFF00, 0x80, 6, 0, 0, frames, 192, &n)
    XNET Write (Frame Output Stream, raw)  <- frames[0..n)

[Operator reset]
    TcStep(0, 2 Reset, Tick Count, cfg, 11, state, 13)
```

Change the setpoint by writing `cfg[4]` (and moving `cfg[2]`/`cfg[3]` with
it); it takes effect at the next Step. No re-Init needed.

**Pacing the BAM.** J1939-21 expects 50–200 ms between TP.DT packets. Either
enable **XNET Session → Intf:Output Stream Timing = Replay Exclusive** and
give the frames real timestamps via `timestamp100ns`/`spacing100ns`, or write
one 24-byte record per 50 ms tick from a small queue. Do not blast all 8
frames back-to-back on a bus with other J1939 nodes.

**XNET timestamp.** The raw-frame timestamp counts 100 ns ticks since
1601-01-01 00:00 UTC. LabVIEW's absolute timestamp counts seconds since
1904-01-01, so `timestamp100ns = (LabVIEW timestamp as DBL + 9561628800) × 1e7`
(9 561 628 800 s separate the two epochs). Pass 0 when you only stream to the
bus; XNET stamps received frames itself.

## 5. Writing a .ncl log from the RT side

```
open file, write TcNclHeader() (12 bytes)
per tick: write the U8 array from TcEncodeFrames (with real timestamps)
```

The result opens in **NI-XNET Bus Monitor** (File → Open Log) and in any
tool that reads the NI-XNET logfile format. `examples\sample_tempctl.ncl` is
such a file, produced from a simulated plant, with the matching
`sample_tempctl.csv` trace.

## 6. Receiving side (optional)

If a PC with an XNET or other CAN interface listens, a DBC can describe the
controller message as PGN 65280, 44 bytes, 11 Intel float32 signals at start
bits 0, 32, 64, …, 320. Any J1939 stack that reassembles BAM (Vector CANoe,
python-j1939, pretty_j1939, Kvaser tools) will then show HiLimit,
LoLimit, … ErrorStatus by name. The DBC line for one signal:

```
 SG_ ActualTemp : 160|32@1- (1,0) [-1000|1000] "degC" Vector__XXX
```

with the signal marked as IEEE float in `SIG_VALTYPE_` (`SIG_VALTYPE_ 2364539904 ActualTemp : 1;`).

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| LabVIEW error 7 (file not found) on RT | .so not at the path in the CLFN, or not executable. `ls -l /usr/local/lib/libtempctl.so`, `chmod 755` |
| Error 13 / "not a valid Win32 application" | 32-bit LabVIEW loading the x64 DLL or vice versa. Use `x86\tempctl.dll` for 32-bit LabVIEW |
| Return −4 | Step before Init on that zone. Init once per zone at startup |
| Return −5 | `out` array too small. Size it from `TcJ1939BamFrameCount` × 24 (or `bytesWritten` after the failed call) |
| Return 1 | Config order violated. Check `LoLimit < LoDeadband ≤ Setpoint ≤ HiDeadband < HiLimit` and non-negative timeouts |
| Relay chatters every DeadbandTimeout | Setpoint outside the deadband (return 1). Move the deadbands with the setpoint |
| Heater never turns on | Temperature not below LoDeadband for the full DeadbandTimeout, or a latched fault (ErrorStatus ≠ 0, needs Reset) |
| Frames appear but the receiver shows no PGN 65280 | The receiver does not reassemble BAM. Check that both `1CECFFxx` and `1CEBFFxx` frames arrive 50–200 ms apart |
| All countdowns take one tick longer than expected | By design: the countdown starts at the first observation and does not charge the preceding interval |
