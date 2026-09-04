# tempctl — Changelog

---

## v1.0.0 - 2026-09-04

First release.

- **Temperature controller** `TcStep`: per-zone (16 slots) single-step state
  machine on an 11-element SGL array. Live configuration every tick, latched
  limit faults with ErrorTimeout countdown, deadband-timed heat/cool
  engagement that runs to setpoint, NaN/Inf reading handling (ErrorStatus 3),
  wrap-safe millisecond tick input, optional 13-element output with the two
  remaining-countdown values, config-order warning on Init and Step.
- **J1939 BAM transport** `TcJ1939Bam` / `TcEncodeFrames` /
  `TcJ1939BamFrameCount`: TP.CM BAM + TP.DT packets per J1939-21 for payloads
  up to 1785 bytes, single-frame path for ≤ 8 bytes, PDU1 destination
  handling, 0xFF padding, per-frame timestamp spacing. Output is NI-XNET raw
  frame format (24 bytes per classic CAN frame), directly writable with XNET
  Write (Frame Output Stream, raw) or appendable to a `.ncl` file after
  `TcNclHeader`.
- **Generic DBC-style packer** `TcCanPack`: Intel/Motorola start-bit
  semantics identical to a `.dbc`, unsigned/signed/float32/float64, factor,
  offset, min/max clamp, saturation, DLC overflow detection. Database passed
  as flat DBL tables (9 columns per signal, 4 per frame).
- Builds: `tempctl.dll` x64 and x86 (MSVC v145, static CRT, version
  resource), `libtempctl.so` x86_64 for NI Linux RT (zig cross-compile,
  glibc ≥ 2.14 symbols, libc only).
- Verification: 154 unit checks on x64 and x86; ctypes oracle test that
  reassembles the BAM traffic with `pretty_j1939` and compares `TcCanPack`
  bit-for-bit with `cantools` over 3000 random layouts.
