# CanTp — Distribution Package v1.2.0

Generic CAN transport-protocol packer/unpacker for LabVIEW (Call Library
Function Node) and any native caller: DBC-scaled signals in, NI-XNET raw
frames of a J1939 BAM / classic CAN / CAN FD sequence out, and back. Message
definitions come as flat DBL tables or, since v1.2.0, as the flattened
LabVIEW `J1939Msg(V4)` cluster of an ECD database.

## Files in this package

| File | Purpose |
|---|---|
| `cantp.dll`, `cantp.lib` | The library, **Windows x64**, and its import library |
| `x86\cantp.dll`, `x86\cantp.lib` | The same library built **x86** (32-bit LabVIEW) |
| `linux-x64\libcantp.so` | **NI Linux RT x86_64** (cRIO-904x/905x/906x) |
| `linux-arm64\libcantp.so` | **aarch64 Linux** (Raspberry Pi 4/5 64-bit test bench) |
| `linux-armhf\libcantp.so` | **32-bit ARM Linux** (LabVIEW LINX / Hobbyist toolkit chroot on a Raspberry Pi) |
| `test_cantp.exe`, `x86\test_cantp.exe`, `linux-*\test_cantp` | Release-gate test for each target (`440 passed, 0 failed`) |
| `cantp.h` | C header, one for every build; the semantics are documented in it |
| `tools\dbc2tables.py` | `.dbc` → CanTp tables (JSON, CSV for LabVIEW, C header); the only DBC parser in the system |
| `tools\ecdflat.py` | `.ecd` database → per-message flattened `J1939Msg(V4)` cluster bytes for `CanTp_DefineFlat` (and the cluster ↔ Python round trip) |
| `tools\pi_bus_loop.py` | Live SocketCAN loop test for the .so (python-can) |
| `tools\elfinfo.py` | ELF inspector used for DEPENDENCIES.txt |
| `examples\oracle_test.py` | ctypes cross-check vs cantools, pretty_j1939 and the `isotp` package; also an API usage example |
| `tools\pi_session_loop.py` | Live two-node RTS/CTS / ISO-TP session test over SocketCAN (Linux) |
| `examples\tables\` | Tables generated from `J1939_NGHD_V130.dbc` for EC1, EEC1, RC (JSON/CSV/C) |
| `TESTLOG.txt` | Captured gates and oracle at package time |
| `DEPENDENCIES.txt` | PE imports (no VC runtime) and ELF exports/GLIBC needs |
| `MANIFEST.txt` | SHA-256 of every file |
| `CANTP_PACKAGE_GUIDE.md` | **API reference**: record format, table columns, every function with CLFN types |
| `DBC-CONVENTIONS.md` | How a DBC (and §8: the ECD cluster) maps to CanTp (Vector J1939PG / CAN FD attributes, start bits, SA placeholder) |
| `RECORD-FORMAT.md` | Byte-level breakdown of the U8 frame array: record fields, sizes, worked examples, payload bit numbering |
| `LABVIEW_INTEGRATION.md` | Deploying, CLFN settings, RT loop, timestamps, troubleshooting |
| `TESTING.md`, `CHANGELOG.md`, `LICENSE.txt` (MIT) | |
| `src\` | Complete C99 source + build script |

No runtime dependencies: the DLLs import only `KERNEL32.dll`; the `.so`
files import only `memcpy`/`memset` from libc (GLIBC 2.14 symbols).

## Quick start

1. On the PC: `pip install cantools`, then
   `python tools\dbc2tables.py MyBus.dbc --message MYMSG --sa 0x80 --out tables`.
2. On the target: copy the matching `libcantp.so` to `/usr/local/lib/`,
   `chmod 755`, run `test_cantp` once.
3. In LabVIEW: `CanTp_Define(slot, msg.csv row, 8, sig.csv rows, nSig)` once
   (or `CanTp_DefineFlat(slot, Flatten To String of the J1939Msg cluster → bytes, len, -1, -1)`);
   `CanTp_PackSgl(slot, values, n, 0, 0, out, CanTp_OutputSize(slot), &written)`
   every cycle, or `CanTp_Transfer(slot, 0/1, values, n, frames, size, lens, nFrames, 0, 0, &used, &nf)`
   for both directions from one CLFN; write the records with XNET Write
   (Frame Output Stream, Raw), 50 ms apart for BAM. Receive with
   `CanTp_RxFeed` per record or `CanTp_Transfer` in read mode.

## Design decisions (owner + Scott, 2026-09-04)

- The library never loads a database. Scaling is loaded once per message
  (`Define`), values are converted per call (`Pack`). Multiple messages
  coexist in slots (temp controller, a simulated EC1, …).
- Values are generic floats (SGL and DBL entry points); the table decides
  the wire type.
- DBC conventions follow Vector's J1939 databases (EC1/RC in
  `J1939_NGHD_V130.dbc`): `VFrameFormat = J1939PG`, length > 8, SA 0xFE
  placeholder, linear Intel start bits.
- Transports: classic, CAN FD, J1939 BAM, and (since v1.1.0) J1939
  RTS/CTS and ISO 15765-2 as **sessions** driven by the caller's receive
  loop (`CanTp_TxStart` / `CanTp_TxFeed` / `CanTp_RxStep`), plus
  multiplexed signals (`CanTp_DefineMux`).
- Output is the NI-XNET raw frame record so LabVIEW can hand the U8 array
  straight to XNET, log it as `.ncl`, or walk it with `CanTp_RecordSize`.

## Design decisions (Scott's CanParsing notes, 2026-09-12)

- Separate Pack/Unpack calls stay; `CanTp_Transfer` adds one CLFN with a
  read/write mode for both directions.
- DBL is the primary signal type (a raw U32 does not survive SGL); SGL
  variants remain.
- The flattened `J1939Msg(V4).ctl` cluster is the primary LabVIEW definition
  input (`CanTp_DefineFlat`); the DBL tables remain for non-LabVIEW callers
  and for DBC-derived definitions. No JSON / XML parser.
- A U8 frame-length array accompanies the frame array (CAN FD, short frames).
- A single-frame message uses no transport protocol: transport 0 is "none".
- Signal order = channel order in the cluster.

## Verified

| Check | Result |
|---|---|
| Unit tests, Windows x64 and x86 | 440 / 440 |
| cantools pack + unpack oracle | 3000 random layouts, 300 random multi-signal messages, 5 real Vector messages × 20 value sets, multiplexed VW × 12 selector values |
| pretty_j1939 BAM reassembly + `RxFeed` | 40 payload lengths incl. 1785 |
| ISO-TP vs the `isotp` package, both directions | 7 sizes × block size / STmin variants, 11- and 29-bit |
| J1939 RTS/CTS vs a python reference peer, both directions | 7 sizes × windows 255/3, dropped-packet re-request, pretty_j1939 reassembly |
| `.ecd` clusters through `CanTp_DefineFlat` | all 754 messages of `J1939_NGHD_V130.ecd`: tables, defaults and `FlatSize` walk vs a Python mirror; 12 real messages (EEC1, EC1, RC, ET1, TCFG, CCVS, EEC2, AMB, LFE, VD, DM1, HOURS) packed through `CanTp_Transfer` bit-for-bit vs cantools on the DBC, read back with the length array; one message (ETC2) rejected for a Motorola start bit (DBC-CONVENTIONS.md §8) |
| Raspberry Pi 5 (aarch64, glibc 2.41) | 440 / 440 (v1.2.0), plus the 32-bit `armhf` build of the same sources run under the 64-bit kernel 440 / 440; v1.1.0: live BAM loop, live RTS/CTS and ISO-TP sessions between the two bench Pis (`docs\testlogs\`) |
| cRIO x86_64 `.so` | built, ELF-inspected, **not yet executed on a cRIO** |

## Not in this release

- ISO-TP on CAN FD (frames above 8 bytes, the 32-bit length escape above
  4095 bytes), extended / mixed ISO-TP addressing, overlapping-signal
  detection.
- The library does not transmit or receive from hardware; it produces and
  consumes records.
- Motorola channels in an ECD cluster: the library applies the DBC start-bit
  rule; the convention the ECD editor uses is unconfirmed (open question to
  Scott, DBC-CONVENTIONS.md §8).

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. Original
implementation from the public J1939-21, NI-XNET raw frame / logfile and
DBC format descriptions; no NI or Vector code is used or linked.
