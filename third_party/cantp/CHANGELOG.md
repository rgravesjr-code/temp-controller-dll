# CanTp — Changelog

---

## v1.2.0 - 2026-09-12

Release 3, from Scott's "CanParsing" notes: the ECD message cluster as the
definition input, a frame-length array, and one call for both directions.
Additive — tables, records and every earlier export are unchanged.

- **`CanTp_DefineFlat`**: define a slot from the bytes LabVIEW's Flatten To
  String produces for one `J1939Msg(V4).ctl` cluster (the per-message
  record of an Eaton `.ecd` database). Reads id / extended / NumDataBytes /
  UpdateRate and per channel start bit, bits, data type, byte order, factor,
  offset, min, max, default; skips names, units, descriptions and lookup
  tables. Transport (classic / J1939 single frame / BAM / RTS/CTS / CAN FD)
  and source address derived from the id and byte count, both overridable.
  `CanTp_FlatSize` walks a flattened array of clusters, `CanTp_GetDef` reads
  a slot back as table rows, `CanTp_Defaults` returns the channel defaults.
  New code `CANTP_ERR_FLAT` (−14). `tools/ecdflat.py` decodes `.ecd` files
  and builds the cluster bytes for non-LabVIEW callers.
- **Frame lengths**: `CanTp_FrameLengths` lists the payload length of every
  record in a U8 array (CAN FD strides, classic frames under 8 bytes).
- **`CanTp_Transfer` / `CanTp_TransferSgl`**: one entry point with a
  read/write mode, the signal array, the frame array and the length array
  (written on pack, checked and used as the walk on unpack).
- Transport 0 documented as "no transport protocol, one classic frame".
- New target `linux-armhf` (32-bit ARM, glibc 2.24) for a LabVIEW LINX /
  Hobbyist-toolkit chroot on a Raspberry Pi.
- `docs/RECORD-FORMAT.md`: byte-level breakdown of the record array with
  worked classic, J1939, BAM and CAN FD examples and the payload bit
  numbering.
- Verification: 440 unit checks (x64, x86, Pi 5 aarch64 and the armhf build
  under the 64-bit kernel); oracle section (h): all 754 clusters of
  `J1939_NGHD_V130.ecd` through `DefineFlat` (tables, defaults and
  `FlatSize` walk match a Python mirror; one message rejected for a Motorola
  start bit that does not fit the DBC rule, see DBC-CONVENTIONS.md §8),
  twelve real messages packed through `Transfer` bit-for-bit against
  cantools on the DBC and read back with the length array.


## v1.1.0 - 2026-09-04

Release 2: the handshake transports and multiplexed signals. Additive — the
8-column tables, the record format and every v1.0.0 export are unchanged.

- **J1939 RTS/CTS** (transport 4, DA ≠ 255): sender `CanTp_TxStart` /
  `CanTp_TxFeed` (RTS, DT windows on each CTS, EndOfMsgAck, abort on
  timeout T3 1250 ms), receiver `CanTp_RxStep` (CTS windows from the session
  config capped by the RTS limit, end-of-window re-request of missing packets
  up to twice, EndOfMsgAck, abort on T1 750 ms). Windows of any size up to 255.
- **ISO 15765-2** (transport 5, classic CAN, normal addressing, 11- and
  29-bit): SingleFrame, FirstFrame, ConsecutiveFrames with 4-bit wrap,
  FlowControl CTS / Wait / Overflow, block size and STmin from the receiver's
  session config (STmin honoured in the ConsecutiveFrame timestamps),
  N_Bs / N_Cr 1000 ms. Peer id for the flow control defaults to the swapped
  29-bit address bytes or id + 8.
- **Session API**: `CanTp_SessionConfig` (peer id, window/BS, STmin,
  timeout, RTS max packets), `CanTp_TxStart`/`TxStartSgl`, `CanTp_TxFeed`,
  `CanTp_TxState`, `CanTp_TxReset`, `CanTp_RxStep`/`RxStepSgl`,
  `CanTp_RxState`. Every transport works through these calls (BAM, classic
  and CAN FD complete at `TxStart`), so one loop serves all. `CanTp_Unpack`
  reassembles recorded RTS/CTS and ISO-TP transfers without responding.
- **Multiplexed signals**: `CanTp_DefineMux` with a 2-column table
  (multiplexor row, selector value), extended multiplexing, NaN for
  unselected signals on unpack; `dbc2tables.py` emits the table from `M` /
  `m<n>` / `SG_MUL_VAL_`, chooses transport 4 with `--da`, transport 5 from
  the `TpProtocol = ISOTP` attribute or `--isotp`.
- New return codes `CANTP_DONE` (2), `CANTP_ERR_TIMEOUT` (−10),
  `CANTP_ERR_ABORTED` (−11), `CANTP_ERR_BUSY` (−12), `CANTP_ERR_MUXDEF`
  (−13). `CanTp_Define` now accepts transports 4 and 5; a BAM definition
  with DA ≠ 255 or an RTS/CTS one with DA = 255 is `CANTP_ERR_MSGDEF`
  (was `CANTP_ERR_TRANSPORT`).
- Verification: 327 unit checks (x64, x86, Raspberry Pi 5); oracle
  sections (e) multiplexed VW vs cantools, (f) ISO-TP both directions vs the
  `isotp` package, (g) RTS/CTS both directions vs a reference peer with
  pretty_j1939 reassembly; live sessions between the two bench Pis with the
  J1939 simulator running on the same bus (`tools/pi_session_loop.py`,
  `docs/testlogs/pi-sessions-2026-09-04.txt`).


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
