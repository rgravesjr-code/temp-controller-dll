# tempctl — temperature controller + J1939 BAM encoder (DLL / .so)

A small C99 library for LabVIEW's Call Library Function Node. One source tree
builds `tempctl.dll` (Windows x64 and x86) and `libtempctl.so` (NI Linux RT x64,
cRIO-904x/905x). No runtime dependencies: the .so imports only `memcpy` and
`memset` from libc (GLIBC 2.14), the .dll links the CRT statically.

Three jobs:

1. **Temperature controller** — `TcStep`: single-step, per-zone state machine
   driven by an 11-element SGL array (limits, deadbands, setpoint, actual,
   timeouts, relay states, error status). Same array comes back out.
2. **J1939 BAM transport** — `TcEncodeFrames` / `TcJ1939Bam`: turns the SGL
   array (or any byte payload up to 1785 bytes) into TP.CM + TP.DT frames,
   emitted as a 1-D U8 array in NI-XNET raw-frame format (24 bytes per frame,
   the same record layout as an `.ncl` logfile).
3. **Generic DBC-style packer** — `TcCanPack`: any signals into any frames,
   with the database (start bit, length, byte order, factor, offset...) passed
   in as flat DBL tables.

```
src/tempctl.h        public API (read this for exact semantics)
src/tempctl.c        controller state machine
src/j1939.c          BAM builder, XNET raw frame writer, .ncl header
src/canpack.c        DBC-style packer
src/tempctl.def      Windows export list
tests/test_main.c    154 unit checks, compiled with the sources (no DLL needed)
tests/oracle_test.py ctypes cross-check vs pretty_j1939 (BAM) and cantools (packing)
examples/make_sample_ncl.py   simulated plant -> sample_tempctl.ncl + .csv trace
tools/elfinfo.py     shows machine / exports / glibc needs of the .so
build.bat            builds everything (see below)
package_dist.bat     stages + zips dist\TempCtl_vX.Y.Z with gates and manifests
scripts/             packaging helpers (version check, DEPENDENCIES/MANIFEST)
```

## Build

Requires Visual Studio 2022+ Build Tools (C++ workload) for the DLL, and a
zig toolchain for the Linux cross-build (portable zip, no admin; expected at
`..\tools\zig-x86_64-windows-*\zig.exe` or `%ZIG_HOME%\zig.exe`).

```bat
build.bat            :: all: dll + tests (run) + so
build.bat win        :: dll + test exe only
build.bat test       :: dll + run tests
build.bat linux      :: so only
```

Outputs: `build\win-x64\tempctl.dll` (+ `build\win-x86\`), `test_tempctl.exe`
for both, `build\linux-x64\libtempctl.so`, `build\linux-x64\test_tempctl` (Linux
test binary, run it on the cRIO once: `./test_tempctl` prints `154 passed, 0 failed`).

Distribution package (encrypted + plain zip, TESTLOG/DEPENDENCIES/MANIFEST,
guides), same flow as the TdmsNative / Mf4FileCopy packages:

```bat
package_dist.bat 1.0.0 [zip-password]     :: -> dist\TempCtl_v1.0.0*
```

Guides: `DISTRIBUTION_README.md` (package contents), `TEMPCTL_PACKAGE_GUIDE.md`
(API reference), `LABVIEW_INTEGRATION.md` (CLFN settings, cRIO deployment),
`TESTING.md`, `CHANGELOG.md`.

Oracle test (optional, needs `pip install cantools pretty_j1939`):

```bat
python tests\oracle_test.py
```

## Deploy to the cRIO-9045

```
scp build\linux-x64\libtempctl.so admin@<crio>:/usr/local/lib/
scp build\linux-x64\test_tempctl  admin@<crio>:/tmp/
ssh admin@<crio> "chmod 755 /usr/local/lib/libtempctl.so /tmp/test_tempctl && /tmp/test_tempctl"
```

In the RT VI's Call Library Function Node use the path
`/usr/local/lib/libtempctl.so`. On the Windows host use `tempctl.dll`.
(If you prefer LabVIEW's `tempctl.*` wildcard path, copy the .so as
`tempctl.so` instead; the SONAME does not matter to LabVIEW.)

All functions: calling convention **C**, return type **Numeric I32**, thread
setting **Run in any thread** is fine (zones are independent; do not call the
same zone from two loops).

## TcStep — the controller

```c
int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
               const float* in, int32_t inLen, float* out, int32_t outLen);
```

| CLFN param | Type | Notes |
|---|---|---|
| zone | I32 | 0..15, independent controllers |
| action | I32 | 0 = Init, 1 = Step, 2 = Reset |
| nowMs | U32 | `Tick Count (ms)`; the library differences it, wrap is handled |
| in | Array Data Pointer, SGL 1-D | 11 elements, order below |
| inLen | I32 | 11 |
| out | Array Data Pointer, SGL 1-D | 11 or 13 elements (pre-size the array) |
| outLen | I32 | 11 or 13 |

Signal order (in and out):

| # | Signal | In | Out |
|---|---|---|---|
| 0 | HiLimit | config | echo |
| 1 | LoLimit | config | echo |
| 2 | HiDeadband (absolute °) | config | echo |
| 3 | LoDeadband (absolute °) | config | echo |
| 4 | Setpoint | config | echo |
| 5 | ActualTemp | measured | echo |
| 6 | ErrorTimeout (ms) | config | echo |
| 7 | DeadbandTimeout (ms) | config | echo |
| 8 | CoolingActive | initial state (Init only) | relay 0/1 |
| 9 | HeatingActive | initial state (Init only) | relay 0/1 |
| 10 | ErrorStatus | ignored | 0 none, 1 hi limit, 2 lo limit, 3 bad reading |
| 11 | (optional) ErrorRemainMs | – | remaining error countdown |
| 12 | (optional) DbRemainMs | – | remaining deadband countdown |

Rules:

- **Pass the whole array every call.** Config is read live, so a setpoint or
  limit change on the cRIO applies at the next Step. Init just clears the
  timers and fault and takes the initial relay state from elements 8/9.
  `in` and `out` may be the same array.
- **Config must satisfy** `LoLimit < LoDeadband ≤ Setpoint ≤ HiDeadband < HiLimit`
  and non-negative timeouts. Otherwise Init/Step return `1` (TC_WARN_CONFIG)
  and still run; the visible symptom of a setpoint outside the deadband is a
  relay that chatters, so treat the warning as an error in the RT code.
  Move the deadbands together with the setpoint.
- **Deadband → relay:** idle and temp above HiDeadband for DeadbandTimeout ms
  turns cooling on; below LoDeadband for DeadbandTimeout ms turns heating on.
  Heating runs until temp ≥ Setpoint, cooling until temp ≤ Setpoint. Never
  both at once. The countdown starts at the first Step that sees the
  condition and restarts from full if the condition clears.
- **Limit → fault:** temp above HiLimit (or below LoLimit, or NaN/Inf) for
  ErrorTimeout ms latches ErrorStatus 1/2/3 and forces both relays off until
  Reset (or Init). A relay that is running keeps running during the countdown
  (a failing heater with a falling temperature is the realistic case).
  A NaN/Inf reading drops both relays immediately; the fault still waits for
  the timeout so a single glitch does not latch.
- **Zero timeout** = act on the first observation.
- Return codes: 0 ok, 1 config warning, −1 bad args/length, −2 bad zone,
  −3 bad action, −4 Step before Init.

## Frames out — NI-XNET raw frame / .ncl record

Every frame is 24 bytes, little-endian:

| offset | size | field |
|---|---|---|
| 0 | 8 | Timestamp, 100 ns units since 1601-01-01 UTC (0 = none) |
| 8 | 4 | Identifier; bit 29 (0x20000000) set for 29-bit IDs |
| 12 | 1 | Type: 0x00 CAN data |
| 13 | 1 | Flags: 0 |
| 14 | 1 | Info: 0 |
| 15 | 1 | PayloadLength 0..8 |
| 16 | 8 | Payload, zero padded |

Feed the U8 array straight into **XNET Write (Frame Output Stream, raw)**, or
prepend the 12-byte header from `TcNclHeader` and write to a `.ncl` file
(NI-XNET Bus Monitor opens it). `examples/sample_tempctl.ncl` is such a file.

## TcEncodeFrames / TcJ1939Bam — J1939 BAM

```c
int32_t TcEncodeFrames(const float* signals, int32_t n, uint32_t pgn, uint8_t sa, uint8_t priority,
                       uint64_t timestamp100ns, uint64_t spacing100ns,
                       uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t TcJ1939Bam(uint32_t pgn, uint8_t sa, uint8_t priority, const uint8_t* payload, int32_t payloadLen,
                   uint64_t timestamp100ns, uint64_t spacing100ns,
                   uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t TcJ1939BamFrameCount(int32_t payloadLen);
```

| CLFN param | Type |
|---|---|
| signals / payload | Array Data Pointer (SGL / U8) + I32 length |
| pgn | U32 (default 65280 = 0xFF00, Proprietary B) |
| sa | U8 (default 0x80) |
| priority | U8 (default 6; only used for the single-frame case, TP frames use 7 per J1939-21) |
| timestamp100ns, spacing100ns | U64 (0, 0 to leave timestamps zero) |
| out | Array Data Pointer U8, pre-sized to `TcJ1939BamFrameCount(len) * 24` |
| outLen | I32 |
| bytesWritten | Pointer to Value, I32 |

The 11 controller signals go across as raw IEEE-754 single floats,
little-endian, 44 bytes → 1 TP.CM + 7 TP.DT = 8 frames = 192 bytes:

```
ID 1CECFF80  20 2C 00 07 FF 00 FF 00     TP.CM BAM: 44 bytes, 7 packets, PGN 00FF00
ID 1CEBFF80  01 <bytes 0..6>              TP.DT seq 1
...
ID 1CEBFF80  07 <bytes 42..43> FF FF FF FF FF   last packet padded 0xFF
```

Payloads of 8 bytes or less go out as a single frame under the PGN itself
(PDU1 PGNs get destination 0xFF). Frame *i* gets timestamp
`timestamp + i * spacing`; J1939-21 wants 50–200 ms between BAM packets, so
pace the XNET writes accordingly (XNET ignores the timestamps for normal
stream output). `TC_ERR_BUFFER` (−5) means `out` is too small and
`bytesWritten` holds the size needed; −6 means payload > 1785 bytes.

## TcCanPack — generic packer

```c
int32_t TcCanPack(const double* sigDefs, int32_t nSig, const double* frameDefs, int32_t nFrames,
                  const float* values, int32_t nValues, uint64_t timestamp100ns,
                  uint8_t* out, int32_t outLen, int32_t* bytesWritten);
```

`sigDefs` is a flat DBL array, 9 numbers per signal (a 2-D LabVIEW array
`nSig × 9` reshaped to 1-D works directly):

| col | meaning |
|---|---|
| 0 | frame index (row in frameDefs) |
| 1 | start bit, DBC convention (Intel: LSB; Motorola: MSB, sawtooth numbering, exactly the number in the .dbc) |
| 2 | bit length 1..64 |
| 3 | byte order: 0 Intel/LE, 1 Motorola/BE |
| 4 | value type: 0 unsigned, 1 signed, 2 float32 (len 32), 3 float64 (len 64) |
| 5 | factor (`phys = raw*factor + offset`) |
| 6 | offset |
| 7 | min (clamp, ignored when max ≤ min) |
| 8 | max |

`frameDefs`: 4 numbers per frame: arbitration ID, extended (0/1), DLC, cycle
ms (reserved, pass 0). `values`: one physical value per signal, same order.
Every frame is emitted in order, one 24-byte record each. Raw values are
rounded half-away-from-zero and saturated to the bit width. Verified
bit-for-bit against cantools over 3000 random layouts (`tests/oracle_test.py`).

## Design decisions (owner-confirmed 2026-09-04)

- Transport: J1939 BAM (broadcast, no handshake). Target: cRIO-9045 (x64).
- Raw SGL over the wire for the controller message; scaled integers are
  available through `TcCanPack` if a DBC consumer needs them.
- Absolute deadbands; `>= Setpoint` ends a heat cycle (no explicit overshoot).
- Optional 13-element output adds the two remaining-countdown values.
- NaN/Inf reading → ErrorStatus 3 after ErrorTimeout, relays off at once.
- Time is passed in (`Tick Count (ms)`), not assumed from a loop period.
- Controller state lives in the library, 16 zone slots, selected by index.
