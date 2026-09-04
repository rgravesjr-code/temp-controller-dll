# CanTp — generic CAN transport-protocol packer/unpacker (DLL / .so)

Signals in (DBC-scaled floats), CAN frames out (NI-XNET raw records of a
J1939 BAM, classic CAN or CAN FD sequence), and back. Built for LabVIEW's
Call Library Function Node on Windows and NI Linux RT (cRIO), tested on a
Raspberry Pi bench. Plain C99, no dependencies, one source for every target.

```
src/cantp.h           public API + semantics (read this first)
src/cantp.c           slots, define, pack, unpack, live receive (BAM reassembly)
src/bits.c            DBC bit placement/extraction, raw<->physical
src/xnet.c            NI-XNET raw record read/write, .ncl header
tests/test_main.c     177 unit checks (compiled with the sources)
tests/oracle_test.py  cantools + pretty_j1939 cross-check incl. real Vector J1939 messages
tools/dbc2tables.py   .dbc -> CanTp tables (JSON / CSV for LabVIEW / C header)
tools/pi_bus_loop.py  live SocketCAN loop test for the .so
tools/elfinfo.py      ELF inspector (DEPENDENCIES.txt)
docs/DBC-CONVENTIONS.md   how a DBC maps onto CanTp (Vector J1939PG / CAN FD)
CANTP_PACKAGE_GUIDE.md    API reference with CLFN tables
LABVIEW_INTEGRATION.md    deploy, CLFN settings, RT loop, troubleshooting
build.bat / package_dist.bat / scripts/   build all targets, stage + zip a release
```

## Build

MSVC Build Tools (VS 2022+, C++ workload) for the DLLs; zig for the Linux
cross-builds (portable zip at `..\tools\zig-x86_64-windows-*\zig.exe` or
`%ZIG_HOME%`).

```bat
build.bat            :: all: dll x64+x86, run gates, .so linux-x64 + linux-arm64
build.bat test       :: dll + gates only
build.bat linux      :: .so only
python tests\oracle_test.py            (pip install cantools pretty_j1939)
package_dist.bat 1.0.0 [zip-password]  -> dist\CanTp_v1.0.0*
```

Outputs: `build\win-x64\cantp.dll`, `build\win-x86\cantp.dll`,
`build\linux-x64\libcantp.so` (cRIO), `build\linux-arm64\libcantp.so` (Pi),
plus `test_cantp` for each.

## Ten-line tour

```c
double msg[8] = { 0x18FF00FE, 1, 60, 1, 0x80, 255, 255, 100 };   // 29-bit, 60 bytes, J1939 BAM, SA 0x80
double sig[2][8] = { { 0, 16, 0, 0, 0.03125, -273, -273, 1735 },   // Intel u16 temperature
                     { 16, 2, 0, 0, 1, 0, 0, 3 } };                 // 2-bit flag
CanTp_Define(0, msg, 8, &sig[0][0], 2);
uint8_t out[24 * 10]; int32_t n;
double values[2] = { 21.5, 1 };
CanTp_Pack(0, values, 2, 0, 500000, out, sizeof out, &n);         // 1 TP.CM + 9 TP.DT records, 50 ms apart
double back[2]; int32_t used;
CanTp_Unpack(0, out, n, back, 2, &used);                          // -> 1, back = { 21.5, 1 }
```

See `docs/DBC-CONVENTIONS.md` for where those numbers come from and
`tools/dbc2tables.py` to produce them from a `.dbc`.
