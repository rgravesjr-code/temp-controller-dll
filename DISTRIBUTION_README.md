# tempctl — Distribution Package v1.0.0

Temperature controller + J1939 BAM / NI-XNET raw-frame encoder for LabVIEW
(Call Library Function Node), as a Windows DLL and an NI Linux RT shared
library for the cRIO-9045.

## Files in this package

| File | Purpose |
|---|---|
| `tempctl.dll` | The library, **Windows x64** (64-bit LabVIEW, Python, .NET) |
| `tempctl.lib` | x64 import library for C/C++ linking |
| `x86\tempctl.dll`, `x86\tempctl.lib` | The same library built **x86** for 32-bit LabVIEW |
| `linux-x64\libtempctl.so` | The library for **NI Linux RT x86_64** (cRIO-904x/905x/906x, incl. cRIO-9045) |
| `linux-x64\test_tempctl` | Release-gate test compiled for the cRIO; run once on the target |
| `tempctl.h` | C header, one header for every build |
| `test_tempctl.exe`, `x86\test_tempctl.exe` | Release-gate test, Windows x64 / x86 (`154 passed, 0 failed`) |
| `TESTLOG.txt` | Captured output of the Windows gates and the Python oracle at package time |
| `DEPENDENCIES.txt` | Import report: DLL/exe PE imports (no VC runtime) and .so ELF exports + GLIBC needs |
| `MANIFEST.txt` | File list with SHA-256 hashes |
| `TEMPCTL_PACKAGE_GUIDE.md` | **API reference** with CLFN parameter tables, frame formats, controller semantics |
| `LABVIEW_INTEGRATION.md` | Deploying to the cRIO, CLFN settings per function, RT loop sketch, XNET pacing, troubleshooting |
| `TESTING.md` | What the gates cover and how to re-run them |
| `CHANGELOG.md` | Release history |
| `LICENSE.txt` | MIT license |
| `examples\make_sample_ncl.py` | ctypes example: simulated plant → controller → BAM → `.ncl` log |
| `examples\sample_tempctl.ncl`, `examples\sample_tempctl.csv` | Its output: open the `.ncl` in NI-XNET Bus Monitor |
| `examples\oracle_test.py` | ctypes cross-check against cantools and pretty_j1939 (also an API usage example) |
| `src\` | Complete source (C99) and the build script, so the library can be rebuilt or audited |

No runtime dependencies: the DLLs link the CRT statically and import only
`KERNEL32.dll`; the .so imports only `memcpy`/`memset` from libc (GLIBC 2.14
symbols, present on every NI Linux RT release). `DEPENDENCIES.txt` is the
verification.

## Quick start

**cRIO-9045**

```bat
scp linux-x64\libtempctl.so admin@<crio-ip>:/usr/local/lib/
scp linux-x64\test_tempctl  admin@<crio-ip>:/home/admin/
ssh admin@<crio-ip> "chmod 755 /usr/local/lib/libtempctl.so /home/admin/test_tempctl && /home/admin/test_tempctl"
```

Then in the RT VI's Call Library Function Node: path
`/usr/local/lib/libtempctl.so`, calling convention **C**, return type I32.

**Windows** — put `tempctl.dll` (or `x86\tempctl.dll` for 32-bit LabVIEW)
next to the VI and use the same CLFN settings with path `tempctl.dll`.

**Verify on your machine**

```bat
test_tempctl.exe          -> 154 passed, 0 failed
```

## The three functions you will use

| Export | What it does |
|---|---|
| `TcStep(zone, action, nowMs, in[11], 11, out[13], 13)` | One controller tick. action 0 Init / 1 Step / 2 Reset; `nowMs` = Tick Count (ms). Reads the whole config every call; outputs relay commands, ErrorStatus, and the two countdowns |
| `TcEncodeFrames(signals, n, pgn, sa, prio, ts, spacing, out, cap, &written)` | The SGL array as raw floats in a J1939 BAM: 1 TP.CM + 7 TP.DT for 11 signals, 8 × 24-byte NI-XNET raw frames = 192 bytes, ready for XNET Write (Frame Output Stream, raw) or a `.ncl` file |
| `TcCanPack(sigDefs, nSig, frameDefs, nFrames, values, nValues, ts, out, cap, &written)` | Any signals into any classic CAN frames from a DBC-style table (start bit, length, byte order, type, factor, offset, min, max) |

Plus `TcJ1939Bam` (arbitrary byte payload), `TcJ1939BamFrameCount`,
`TcNclHeader` (12-byte logfile header) and `TcVersion`.

Signal order for `TcStep` and `TcEncodeFrames`: HiLimit, LoLimit,
HiDeadband, LoDeadband, Setpoint, ActualTemp, ErrorTimeout(ms),
DeadbandTimeout(ms), CoolingActive, HeatingActive, ErrorStatus
(0 none, 1 HiLimit, 2 LoLimit, 3 bad reading).

Defaults used for the controller message: PGN 65280 (0xFF00, Proprietary
B), source address 0x80, priority 6 (TP frames use 7 per J1939-21).

## Design decisions in this release

- Transport is **J1939 BAM** (broadcast, no handshake), so all frames of a
  message can be built up front. The caller paces them 50–200 ms apart.
- The controller keeps its state in the library (16 zones by index) and
  reads the full 11-element configuration on every call, so setpoint and
  limit changes apply on the next tick without a re-Init.
- Deadbands are absolute temperatures. A heat cycle ends at `≥ Setpoint`,
  a cool cycle at `≤ Setpoint`. Config must satisfy
  `LoLimit < LoDeadband ≤ Setpoint ≤ HiDeadband < HiLimit`; the library
  returns warning 1 otherwise (a setpoint outside the deadband chatters).
- Limit faults latch after ErrorTimeout with both relays off until Reset.
  NaN/Inf readings drop the relays immediately and fault (status 3) after
  the same timeout.
- Countdowns start at the first Step that observes a condition; elapsed
  time comes from the `nowMs` argument, not from an assumed loop period.

## Known limits / not yet verified

- The `.so` was cross-compiled and inspected (ELF machine, exports, GLIBC
  symbol versions) but **not executed on a cRIO before packaging**. Run
  `linux-x64\test_tempctl` on the target as the first step.
- Classic CAN only (DLC ≤ 8). CAN FD frames are not produced.
- `TcCanPack` does not detect overlapping signal definitions.
- The library does not transmit; it produces bytes for XNET Write.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. Original
implementation from the public J1939-21, NI-XNET raw frame and NI-XNET
logfile specifications; no NI or Vector code is used or linked.
