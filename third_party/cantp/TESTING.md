# Testing Guide — CanTp

Release gate for v1.2.0: `build.bat all` ends with `440 passed, 0 failed` on
x64 **and** x86, all three Linux targets build, `tests\oracle_test.py` prints
`ALL OK` (needs cantools, pretty_j1939 and can-isotp), and the Linux test
binary has been run on at least one Linux machine. `package_dist.bat` re-runs the Windows gates and the oracle into
`TESTLOG.txt` and refuses to package a failing build.

## Gates

### `tests\test_main.c` — 440 checks, compiled with the sources (`test_release2.inc` = sessions + mux, `test_release3.inc` = flat cluster + Transfer)

- Flattened `J1939Msg(V4)` clusters (release 3): three real clusters
  extracted from `J1939_NGHD_V130.ecd` (`tests\flat_fixtures.h`: ZNVW,
  ET1, DM19) → `DefineFlat`, `GetDef` rows, `Defaults`, pack/unpack, SA
  placeholder and override, PDU1-with-placeholder → BAM, forced transports,
  `FlatSize` with trailing bytes / truncation; a synthetic flattener covering
  signed Motorola, float32/64, lookup tables, every auto-transport rule
  (11-bit, CAN FD, PDU1 to one node → classic / RTS, global → BAM, DBC bit
  31, id > 0x7FF with the flag clear), bad data type / byte order / start,
  129 channels → −9, 128 channels ok, zero channels, two clusters back to
  back; every argument error; table-defined slots round-trip through GetDef.
- Frame lengths and Transfer: a 7-frame BAM written with timestamps and
  lengths, `FrameLengths` count with a short array and on a truncated
  buffer, −6 with sizes when frames or lengths are too small, read back with
  and without the length array, the caller's list stopping the walk, a
  length disagreeing with the header → −8, live RxFeed state untouched, bad
  mode / args / slot, SGL variant, a 3-byte classic frame and a 20-byte
  CAN FD frame (lengths 3 and 20, record 40 bytes), a mixed FD + classic
  buffer.

- Raw records: sizes for every payload length (24 up to 80 bytes), field
  layout, extended flag, FD types 0x10/0x18, buffer/argument errors,
  `CanTp_RecordSize` on truncated input, `.ncl` header, version.
- Define: every rejection path (slot, lengths per transport, 29-bit
  requirement for BAM, BAM with DA ≠ 255 and RTS/CTS with DA = 255 → −4,
  SA override on 11-bit, id range, signal fit Intel and Motorola, factor 0,
  float lengths), and that a failed define leaves the slot empty.
- Multiplexing: selection by the multiplexor's raw value, extended
  multiplexing (a multiplexed multiplexor), pad on pack and NaN on unpack
  for unselected signals, table rejections (self, cycle, range, float
  multiplexor), a failed DefineMux keeping the old table, Define clearing it.
- J1939 RTS/CTS sessions between two slots in memory: RTS / CTS / DT /
  EndOfMsgAck bytes and ids, receiver window from the session config, sender
  limit from the RTS, lost packet re-requested at the end of the window and
  resent, out-of-order arrival inside a window, hold (CTS with 0 packets),
  DT timestamp spacing, buffer-size error, sender timeout with abort frame,
  peer abort, CTS past the end, frames for other nodes / PGNs ignored,
  receiver timeout with abort, twice-missing packet → abort, SA placeholder
  on the receiver, single-frame PDU1 message to a DA, `Unpack` over the
  interleaved log.
- ISO-TP sessions: FF / FC / CF bytes and ids, 11-bit default peer id
  (0x7E0 → 0x7E8), SingleFrame path through Pack and TxStart, block size 2 +
  STmin 20 ms (three flow controls, 20 ms CF spacing), sequence wrap over
  20 ConsecutiveFrames, flow control Wait then Overflow, sender and receiver
  timeouts, wrong CF sequence, stray CF, custom peer id and FC on other ids
  ignored, 29-bit normal fixed addressing, `Unpack` over the log, BAM
  through the session calls.
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
| (h) | every cluster of `J1939_NGHD_V130.ecd` through `CanTp_DefineFlat`: `GetDef` rows and `Defaults` vs the Python mirror in `tools\ecdflat.py`, `FlatSize` walking the whole flattened array; ECD vs DBC wire layout by channel name; 12 real messages packed through `CanTp_Transfer` with the length array, bit-for-bit vs cantools, read back through `Transfer` | 754 + 12 × 10 |

```bat
pip install cantools pretty_j1939 can-isotp
python tests\oracle_test.py [build\win-x64\cantp.dll] [--dbc path\to\file.dbc] [--ecd path\to\file.ecd] [--iters 3000] [--seed 1]
```

### Linux

`build\linux-x64\test_cantp` (cRIO), `build\linux-arm64\test_cantp`
(Raspberry Pi) and `build\linux-armhf\test_cantp` (32-bit ARM chroot) are the same 440 checks. Run once per target after copying
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

Two-node sessions (start the receiver first, on the other node):

```bash
python pi_session_loop.py --role rx --iface can0 --so /tmp/libcantp.so --transport rts   # or isotp
python pi_session_loop.py --role tx --iface can1 --so /tmp/libcantp.so --transport rts
```

A 60-byte message in three rounds; the receiver answers CTS / flow control
from `CanTp_RxStep`, the sender drives `CanTp_TxFeed` from the frames it
reads back. Both print `3/3 rounds OK`.

## Release record

v1.2.0, 2026-09-12: Windows x64/x86 440/440; oracle ALL OK (sections a–h,
(h): 753 of 754 `.ecd` clusters defined and matched, ETC2 rejected for its
Motorola start bit, 12 messages × 10 value sets through `Transfer`);
Raspberry Pi 5 aarch64 440/440 and a static 32-bit armhf build of the same
sources 440/440 under the 64-bit kernel (`docs\testlogs\pi-2026-09-12.txt`).
cRIO x86_64: built and ELF-inspected, not yet executed.

v1.1.0, 2026-09-04: Windows x64/x86 327/327; oracle ALL OK (sections a–g);
Raspberry Pi 5 (aarch64) 327/327; live RTS/CTS and ISO-TP sessions
pi-engine can1 → pi-trans can0 with the J1939 simulator running on the same
bus, 3/3 rounds each (`docs\testlogs\pi-sessions-2026-09-04.txt`). cRIO
x86_64: built and ELF-inspected, not yet executed.

v1.0.0, 2026-09-04: Windows x64/x86 177/177; oracle ALL OK (3000 + 300 +
100 + 40); Raspberry Pi 5 (aarch64, glibc 2.41) 177/177 and live bus loop
on can1 at 250 kbit/s ALL OK (3 rounds, 7 frames each, 351 ms per message
at 50 ms pacing). cRIO x86_64: built and ELF-inspected, not yet executed.
