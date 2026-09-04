# CanTp — Changelog

---

## v1.0.0 - 2026-09-04

First release. Split out of temp-controller-dll v1.0.0 after Scott's review.

- **Two-call model**: `CanTp_Define` loads one message definition (DBC
  `BO_` + `SG_` rows as flat DBL tables) into one of 32 slots; `CanTp_Pack`
  / `CanTp_PackSgl` convert values every cycle. No DBC parsing in the
  library; `tools/dbc2tables.py` converts on the host (JSON, CSV for
  LabVIEW, C header).
- **Transports**: classic CAN, CAN FD (with and without BRS, DLC padding),
  J1939-21 BAM up to 1785 bytes with SA placeholder/override and PDU1
  destination handling. RTS/CTS and ISO-TP reserved (release 2).
- **Receive**: stateless `CanTp_Unpack` over a buffer and stateful
  `CanTp_RxFeed` per frame with BAM reassembly, foreign-frame rejection and
  lost-packet recovery. `CanTp_MakeRecord` for non-XNET drivers.
- **Records**: NI-XNET raw frame / `.ncl` layout, variable length for
  CAN FD, `CanTp_RecordSize` to walk mixed arrays, `CanTp_NclHeader`.
- **Values**: DBC semantics (Intel/Motorola start bits over any payload
  length, signed, float32/64, factor/offset, min/max clamp, saturation,
  NaN → not-available).
- **Builds**: `cantp.dll` x64 + x86 (MSVC, static CRT, version resource),
  `libcantp.so` x86_64 (cRIO) and aarch64 (Raspberry Pi) via zig.
- **Verification**: 177 unit checks; cantools/pretty_j1939 oracle incl.
  real EC1/RC/TCFG messages from the Vector J1939 DBC; Raspberry Pi 5 run
  and live SocketCAN loop.
