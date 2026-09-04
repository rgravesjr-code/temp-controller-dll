# Testing Guide — CanTp

Release gate for v1.0.0: `build.bat all` ends with `177 passed, 0 failed` on
x64 **and** x86, both Linux targets build, `tests\oracle_test.py` prints
`ALL OK`, and the Linux test binary has been run on at least one Linux
machine. `package_dist.bat` re-runs the Windows gates and the oracle into
`TESTLOG.txt` and refuses to package a failing build.

## Gates

### `tests\test_main.c` — 177 checks, compiled with the sources

- Raw records: sizes for every payload length (24 up to 80 bytes), field
  layout, extended flag, FD types 0x10/0x18, buffer/argument errors,
  `CanTp_RecordSize` on truncated input, `.ncl` header, version.
- Define: every rejection path (slot, lengths per transport, 29-bit
  requirement for BAM, DA ≠ 255 → −7, reserved transports, SA override on
  11-bit, id range, signal fit Intel and Motorola, factor 0, float lengths),
  and that a failed define leaves the slot empty.
- Classic frames: EEC1-like layout with nibble, offset, scaled, signed,
  Motorola 12-bit and pad bytes; clamp, saturation and NaN → all ones; SGL
  and DBL variants identical; Unpack skipping foreign ids, resuming after
  `bytesConsumed`, rejecting corrupt records, short records, extended-flag
  mismatch; 11-bit exact-id match.
- CAN FD: float32/float64/Motorola signals in a 20-byte frame, pad byte,
  length padding to valid DLCs (13 → 16, 49 → 64), BRS type.
- J1939 BAM: the 44-byte 11-float message (8 frames), TP.CM bytes, TP.DT ids
  and sequence, timestamp spacing, 0xFF padding, stateless Unpack of a full
  and a partial buffer, SA placeholder 0xFE accepting any sender, SA
  override accepting only that sender and stamping it into every frame,
  live `RxFeed` with foreign traffic interleaved, a lost packet abandoning
  the sequence, `Unpack` not disturbing a live session, short BAM (≤ 8
  bytes) as a single frame with PDU1 destination, 1785-byte maximum (256
  frames) and 1786 rejected.
- EC1 (40 bytes) from the Vector DBC with four of its signals: frame count,
  TP.CM contents, byte positions after reassembly, round trip.

### `tests\oracle_test.py` — independent implementations

Loads the DLL/.so through ctypes and compares with **cantools** (pack and
unpack) and **pretty_j1939** (BAM reassembly):

| Section | What | Count |
|---|---|---|
| (a) | random single-signal layouts, 8-byte frames | 3000 |
| (b) | random non-overlapping multi-signal messages, 8..64 bytes, classic and CAN FD | 300 |
| (c) | real messages from `J1939_NGHD_V130.dbc` converted by `dbc2tables.py`: EEC1 (8), EC1 (40), RC (19), ET1 (8), TCFG (50), 20 random value sets each, bytes and decoded values, TP ids and frame counts | 100 |
| (d) | BAM payload lengths 9..39, 63..65, 100, 255, 256, 700, 1784, 1785: reassembly by pretty_j1939 and by `CanTp_RxFeed` with a foreign frame after every packet | 40 |

```bat
pip install cantools pretty_j1939
python tests\oracle_test.py [build\win-x64\cantp.dll] [--dbc path\to\file.dbc] [--iters 3000] [--seed 1]
```

### Linux

`build\linux-x64\test_cantp` (cRIO) and `build\linux-arm64\test_cantp`
(Raspberry Pi) are the same 177 checks. Run once per target after copying
the .so. The ELF report in `DEPENDENCIES.txt` (machine, exports, GLIBC
versions) is produced at package time by `tools\elfinfo.py`.

### Live bus (Raspberry Pi with a CAN interface)

```bash
/home/pi/j1939sim-venv/bin/python pi_bus_loop.py --so /tmp/libcantp.so --iface can1 --rounds 3
```

Packs a 40-byte mixed-signal BAM with the .so, sends the frames on the
interface with python-can 50 ms apart, receives them through a second
socket, feeds each frame to `CanTp_RxFeed`, and compares the decoded values.
Other traffic on the bus is ignored by the decoder. Prints `ALL OK`.

## Release record

v1.0.0, 2026-09-04: Windows x64/x86 177/177; oracle ALL OK (3000 + 300 +
100 + 40); Raspberry Pi 5 (aarch64, glibc 2.41) 177/177 and live bus loop
on can1 at 250 kbit/s ALL OK (3 rounds, 7 frames each, 351 ms per message
at 50 ms pacing). cRIO x86_64: built and ELF-inspected, not yet executed.
