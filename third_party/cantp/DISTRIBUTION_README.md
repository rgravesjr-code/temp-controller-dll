# CanTp — Distribution Package v1.0.0

Generic CAN transport-protocol packer/unpacker for LabVIEW (Call Library
Function Node) and any native caller: DBC-scaled signals in, NI-XNET raw
frames of a J1939 BAM / classic CAN / CAN FD sequence out, and back.

## Files in this package

| File | Purpose |
|---|---|
| `cantp.dll`, `cantp.lib` | The library, **Windows x64**, and its import library |
| `x86\cantp.dll`, `x86\cantp.lib` | The same library built **x86** (32-bit LabVIEW) |
| `linux-x64\libcantp.so` | **NI Linux RT x86_64** (cRIO-904x/905x/906x) |
| `linux-arm64\libcantp.so` | **aarch64 Linux** (Raspberry Pi 4/5 64-bit test bench) |
| `test_cantp.exe`, `x86\test_cantp.exe`, `linux-*\test_cantp` | Release-gate test for each target (`177 passed, 0 failed`) |
| `cantp.h` | C header, one for every build; the semantics are documented in it |
| `tools\dbc2tables.py` | `.dbc` → CanTp tables (JSON, CSV for LabVIEW, C header); the only DBC parser in the system |
| `tools\pi_bus_loop.py` | Live SocketCAN loop test for the .so (python-can) |
| `tools\elfinfo.py` | ELF inspector used for DEPENDENCIES.txt |
| `examples\oracle_test.py` | ctypes cross-check vs cantools + pretty_j1939; also an API usage example |
| `examples\tables\` | Tables generated from `J1939_NGHD_V130.dbc` for EC1, EEC1, RC (JSON/CSV/C) |
| `TESTLOG.txt` | Captured gates and oracle at package time |
| `DEPENDENCIES.txt` | PE imports (no VC runtime) and ELF exports/GLIBC needs |
| `MANIFEST.txt` | SHA-256 of every file |
| `CANTP_PACKAGE_GUIDE.md` | **API reference**: record format, table columns, every function with CLFN types |
| `DBC-CONVENTIONS.md` | How a DBC maps to CanTp (Vector J1939PG / CAN FD attributes, start bits, SA placeholder) |
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
3. In LabVIEW: `CanTp_Define(slot, msg.csv row, 8, sig.csv rows, nSig)` once;
   `CanTp_PackSgl(slot, values, n, 0, 0, out, CanTp_OutputSize(slot), &written)`
   every cycle; write the records with XNET Write (Frame Output Stream, Raw),
   50 ms apart for BAM. Receive with `CanTp_RxFeed` per record.

## Design decisions (owner + Scott, 2026-09-04)

- The library never loads a database. Scaling is loaded once per message
  (`Define`), values are converted per call (`Pack`). Multiple messages
  coexist in slots (temp controller, a simulated EC1, …).
- Values are generic floats (SGL and DBL entry points); the table decides
  the wire type.
- DBC conventions follow Vector's J1939 databases (EC1/RC in
  `J1939_NGHD_V130.dbc`): `VFrameFormat = J1939PG`, length > 8, SA 0xFE
  placeholder, linear Intel start bits.
- Release 1 transports: classic, CAN FD, J1939 BAM, plus receive. J1939
  RTS/CTS and ISO 15765-2 need a handshake session API and come in
  release 2.
- Output is the NI-XNET raw frame record so LabVIEW can hand the U8 array
  straight to XNET, log it as `.ncl`, or walk it with `CanTp_RecordSize`.

## Verified

| Check | Result |
|---|---|
| Unit tests, Windows x64 and x86 | 177 / 177 |
| cantools pack + unpack oracle | 3000 random layouts, 300 random multi-signal messages, 5 real Vector messages × 20 value sets |
| pretty_j1939 BAM reassembly + `RxFeed` | 40 payload lengths incl. 1785 |
| Raspberry Pi 5 (aarch64, glibc 2.41) | 177 / 177 and live bus loop on can1 ALL OK |
| cRIO x86_64 `.so` | built, ELF-inspected, **not yet executed on a cRIO** |

## Not in this release

- J1939 RTS/CTS, ISO-TP (release 2), multiplexed signals, overlapping-signal
  detection.
- The library does not transmit or receive from hardware; it produces and
  consumes records.

---

## License

MIT (see `LICENSE.txt`). Copyright (c) 2026 Roger Graves. Original
implementation from the public J1939-21, NI-XNET raw frame / logfile and
DBC format descriptions; no NI or Vector code is used or linked.
