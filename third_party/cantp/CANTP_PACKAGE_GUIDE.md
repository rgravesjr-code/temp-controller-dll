# CanTp — Package Guide (API reference)

`CanTp` is a **pure C library with flat `extern "C"` exports** that turns
physical signal values into CAN frames of a transport sequence and back. It is
built for LabVIEW's Call Library Function Node (CLFN) and callable from
C/C++, Python (ctypes), .NET P/Invoke, MATLAB. Files: `cantp.dll` (Windows
x64, x86), `libcantp.so` (Linux x86_64 for cRIO-904x/905x/906x, aarch64 for
Raspberry Pi 4/5, 32-bit ARM for a LabVIEW LINX chroot). One header, one ABI, every build.

**Model.** A message definition (one DBC `BO_` with its `SG_` rows, or one
flattened LabVIEW `J1939Msg(V4)` cluster) is loaded once into a slot with
`CanTp_Define` / `CanTp_DefineFlat`. Every later `CanTp_Pack` on that slot
converts a value array into the frames of one complete transport sequence;
`CanTp_Unpack` / `CanTp_RxFeed` do the reverse. Transports that need a
handshake (J1939 RTS/CTS, ISO-TP) run as sessions through `CanTp_TxStart` /
`CanTp_TxFeed` and `CanTp_RxStep`, fed from the caller's receive loop. The
library never reads a `.dbc`; `tools/dbc2tables.py` produces the tables on
the host (including the multiplex table for `m<n>` signals).

**Calling convention:** C (cdecl). **Types:** `int32_t`, `uint32_t`,
`uint64_t`, `uint8_t`, `float`, `double`; arrays as pointer + `int32_t`
length; no structs, strings, callbacks or allocation. **Return:** `int32_t`,
0 ok, 1 = "message decoded" for the receive calls, negative = error.
**State:** 32 slots × up to 128 signals, static, plus per-slot receive and
transmit buffers (about 400 KB in total). No locks: use a slot from one thread at a time;
different slots are independent. **Dependencies:** none (DLL imports only
KERNEL32; .so imports memcpy/memset from libc, GLIBC 2.14 symbols).

## Return codes

| Code | Value | Meaning |
|---|---|---|
| `CANTP_OK` | 0 | Success; for Unpack/RxFeed: no complete message (yet) |
| `CANTP_FOUND` | 1 | Unpack/RxFeed: a message was decoded into `values` |
| `CANTP_ERR_ARG` | −1 | Null pointer or bad length |
| `CANTP_ERR_SLOT` | −2 | Slot outside 0..31 |
| `CANTP_ERR_NOT_DEFINED` | −3 | Slot has no definition |
| `CANTP_ERR_MSGDEF` | −4 | Bad message row (length for the transport, id range, SA/DA/pad range, SA override on an 11-bit id) |
| `CANTP_ERR_SIGDEF` | −5 | Bad signal row (length, start, factor 0, float type with wrong length, does not fit in the payload) |
| `CANTP_ERR_BUFFER` | −6 | `out` too small; `bytesWritten` holds the size needed |
| `CANTP_ERR_TRANSPORT` | −7 | `CanTp_Pack` on a multi-frame RTS/CTS or ISO-TP message (use the session calls) |
| `CANTP_ERR_RECORD` | −8 | Malformed raw record in the input (fewer than 24 bytes, PayloadLength > 64, record longer than the buffer) |
| `CANTP_ERR_TOO_MANY` | −9 | More than 128 signals |
| `CANTP_DONE` | 2 | TxStart/TxFeed: the transmit session completed |
| `CANTP_ERR_TIMEOUT` | −10 | Session: the peer did not answer within the timeout; the session is over (J1939: an abort frame was written to `out`) |
| `CANTP_ERR_ABORTED` | −11 | Session: the peer aborted, sent an overflow / bad flow control, or the packet sequence could not be recovered |
| `CANTP_ERR_BUSY` | −12 | TxStart while a transmit session is still waiting for the peer (TxReset first) |
| `CANTP_ERR_MUXDEF` | −13 | Bad multiplex row (self-reference, cycle, row out of range, float multiplexor) |
| `CANTP_ERR_FLAT` | −14 | `CanTp_DefineFlat` / `CanTp_FlatSize`: the bytes are not a flattened `J1939Msg(V4)` cluster |

## Raw frame record (input and output format)

Every frame is one NI-XNET raw frame record, identical to an NI-XNET
logfile (`.ncl`) event, little-endian, records concatenated in one U8 array:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Timestamp, U64, 100 ns since 1601-01-01 UTC (0 = none) |
| 8 | 4 | Identifier; bit 29 (`0x20000000`) set = 29-bit extended |
| 12 | 1 | Type: `0x00` CAN data, `0x10` CAN FD data, `0x18` CAN FD BRS data |
| 13 | 1 | Flags = 0 |
| 14 | 1 | Info = 0 |
| 15 | 1 | PayloadLength 0..64 |
| 16 | 8+ | Payload; the record is 24 bytes when PayloadLength ≤ 8, else 24 + ((PayloadLength − 1) & ~7) |

Classic CAN and J1939 records are always 24 bytes. CAN FD records are 24,
32, 40, 48, 56, 64, 72 or 80 bytes. `CanTp_RecordSize` tells the size of the record
at a given offset and `CanTp_FrameLengths` lists every record's payload
length, so a caller can walk a mixed array. Byte-level worked examples
(classic, J1939 PG, BAM, CAN FD) and the bit numbering inside the payload
are in `RECORD-FORMAT.md`.

## Message definition row — `CANTP_MSGDEF_COLS = 8` doubles

| Col | Name | Meaning |
|---|---|---|
| 0 | id | CAN id as in the DBC (11-bit, or 29-bit priority/PGN/SA; DBC bit 31 ignored) |
| 1 | extended | 0 = 11-bit, 1 = 29-bit. Also implied by a DBC id with bit 31 set |
| 2 | length | Payload bytes: 0..8 classic; 0..64 CAN FD; 0..1785 J1939 BAM / RTS/CTS / ISO-TP |
| 3 | transport | 0 none (one classic frame, no transport protocol), 1 J1939 BAM, 2 CAN FD, 3 CAN FD BRS, 4 J1939 RTS/CTS, 5 ISO 15765-2 (ISO-TP). 4 and 5 are sessions (see below) once the message needs more than one frame |
| 4 | sa | Source address override 0..253, or −1 = use the id's low byte. A 29-bit id whose low byte is the Vector placeholder 0xFE with no override means "any sender" on receive |
| 5 | da | Destination address, 255 = broadcast (BAM requires 255; RTS/CTS requires ≠ 255: the receiving node answers from this address). Applied to the PS byte of PDU1 PGNs |
| 6 | pad | Fill byte for bits no signal covers and for unused frame bytes (J1939: 255, ISO-TP: 0xCC) |
| 7 | cycleMs | Informational |

## Signal definition row — `CANTP_SIGDEF_COLS = 8` doubles, one per DBC `SG_`

| Col | Name | Meaning |
|---|---|---|
| 0 | startBit | DBC start bit. Intel: LSB position, linear over the whole payload. Motorola: MSB position, sawtooth |
| 1 | bitLength | 1..64 |
| 2 | byteOrder | 0 Intel (`@1`), 1 Motorola (`@0`) |
| 3 | valueType | 0 unsigned, 1 signed, 2 float32 (length 32), 3 float64 (length 64) |
| 4 | factor | `physical = raw × factor + offset`, non-zero |
| 5 | offset | |
| 6 | min | Physical clamp on pack, ignored when max ≤ min |
| 7 | max | |

See `docs/DBC-CONVENTIONS.md` for the mapping rules and the reference
J1939 example.

## Functions

### Define / queries

```c
uint32_t CanTp_Version(void);                 // 0x010200 = 1.2.0
int32_t  CanTp_Define(int32_t slot, const double* msgDef, int32_t msgDefLen,
                      const double* sigDefs, int32_t nSig);
int32_t  CanTp_Clear(int32_t slot);
int32_t  CanTp_SignalCount(int32_t slot);
int32_t  CanTp_PayloadLength(int32_t slot);
int32_t  CanTp_FrameCount(int32_t slot);      // frames per Pack
int32_t  CanTp_OutputSize(int32_t slot);      // bytes per Pack
```

| CLFN param | Type | Pass |
|---|---|---|
| slot | I32 | Value |
| msgDef | Array DBL 1-D (8) | Array Data Pointer |
| msgDefLen | I32 = 8 | Value |
| sigDefs | Array DBL 1-D (nSig × 8, reshape a 2-D `nSig × 8` array) | Array Data Pointer |
| nSig | I32 | Value |

`CanTp_Define` validates everything and leaves the slot untouched on error.
Redefining a slot replaces it and resets its receive state.

### Define from a flattened LabVIEW cluster (v1.2.0)

```c
int32_t CanTp_DefineFlat(int32_t slot, const uint8_t* flat, int32_t flatLen,
                         int32_t transport, int32_t sa);
int32_t CanTp_FlatSize  (const uint8_t* flat, int32_t flatLen);          // bytes of the first cluster in flat
int32_t CanTp_GetDef    (int32_t slot, double* msgDef, int32_t msgDefLen,
                         double* sigDefs, int32_t nSigMax);              // -> nSig; the rows Define would take
int32_t CanTp_Defaults  (int32_t slot, double* values, int32_t nValues);  // -> nSig; channel defaults
```

`CanTp_DefineFlat` is `CanTp_Define` fed with the bytes LabVIEW's **Flatten
To String** produces for one `J1939Msg(V4).ctl` cluster (the Eaton CAN
database message cluster; big-endian, array and string sizes prepended,
which are the Flatten defaults). The same bytes are the per-message record
inside an `.ecd` database file, which `tools\ecdflat.py` extracts for
non-LabVIEW callers and for the oracle. The library reads the numeric
fields and skips names, descriptions, units and lookup tables:

| Cluster field | CanTp | Rule |
|---|---|---|
| message ID | msg col 0 `id` | Low 29 bits. A value above 0x7FF, or bit 31 set, is treated as extended even when the flag says 0 |
| message ID extended? | msg col 1 | |
| NumDataBytes | msg col 2 `length` | |
| UpdateRate | msg col 7 `cycleMs` | When positive |
| PGN, Description, Tolerance, EatonIPY | — | Ignored (the PGN is in the id) |
| channel start bit | sig col 0 | Verbatim. Intel: LSB position, linear. Motorola: **DBC sawtooth MSB position**; a Motorola start bit that does not fit under that rule is rejected with −5 (see DBC-CONVENTIONS.md §8) |
| channel number of bits | sig col 1 | |
| channel byte order | sig col 2 | 0 Intel → 0, 1 Motorola → 1 |
| channel data type | sig col 3 | 0 Signed → 1, 1 Unsigned → 0, 2 IEEE Float → 2 (32 bits) or 3 (64 bits) |
| scaling factor, offset, min, max | sig cols 4..7 | Verbatim |
| default value | `CanTp_Defaults` | Not used on the wire |
| channel name, unit, description, lookup table | — | Ignored; signal order = channel order |

The cluster has no transport, source-address or destination fields, so:

- `transport` = −1 derives it: 11-bit id → 0 (classic; CAN FD when
  NumDataBytes > 8). 29-bit id → 1 (one frame under the PGN up to 8 bytes,
  BAM above), except a PDU1 PGN addressed to one node (PS not 0xFF / 0xFE)
  → 0 with the id verbatim up to 8 bytes, 4 (RTS/CTS to that node) above.
  Any other value 0..5 forces that transport (e.g. 3 for CAN FD BRS, 5 for
  ISO-TP) and is validated like a table row.
- `sa` = −1 keeps the id's low byte (0xFE = "any sender" on receive, as in
  the Vector DBCs); 0..253 substitutes that source address.
- priority and destination are whatever the id carries; pad is 0xFF for
  29-bit ids, 0 for 11-bit, 0xCC for ISO-TP.

Returns 0, `CANTP_ERR_FLAT` (−14) when the bytes do not parse as the
cluster (truncated, negative string length, garbage), `CANTP_ERR_TOO_MANY`
above 128 channels, or the `CanTp_Define` codes for content the tables would
reject too. `CanTp_FlatSize` returns how many bytes the first cluster
occupies, so a flattened **array** of clusters (the whole database) can be
walked; `CanTp_GetDef` returns the table rows a slot was defined with (for
either define call; the SA column shows the effective source address, −1
for the placeholder), which is how the oracle proves the cluster parser
against `dbc2tables.py`.

| CLFN param (DefineFlat) | Type | Pass |
|---|---|---|
| slot | I32 | Value |
| flat | Array U8 1-D (String To Byte Array of the flattened cluster) | Array Data Pointer |
| flatLen | I32 | Value |
| transport | I32, −1 = derive | Value |
| sa | I32, −1 = from the id | Value |

### Pack

```c
int32_t CanTp_Pack   (int32_t slot, const double* values, int32_t nValues,
                      uint64_t timestamp100ns, uint64_t spacing100ns,
                      uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t CanTp_PackSgl(int32_t slot, const float*  values, int32_t nValues, ... same ...);
```

| CLFN param | Type | Pass |
|---|---|---|
| values | Array DBL (or SGL) 1-D, one physical value per signal row | Array Data Pointer |
| nValues | I32 ≥ nSig | Value |
| timestamp100ns | U64, stamp of the first frame (0 = none) | Value |
| spacing100ns | U64, added per following frame (500000 = 50 ms) | Value |
| out | Array U8 1-D pre-sized to `CanTp_OutputSize(slot)` | Array Data Pointer |
| outLen | I32 | Value |
| bytesWritten | I32 | Pointer to Value |

NaN packs "not available" (all ones / NaN). Values are clamped to min/max
and raw values saturated to the bit width. For J1939 BAM the caller paces
the frames 50..200 ms apart on the bus (J1939-21); XNET Frame Output Stream
ignores the timestamps unless replay timing is enabled.

### Unpack (stateless, over a buffer)

```c
int32_t CanTp_Unpack   (int32_t slot, const uint8_t* frames, int32_t framesLen,
                        double* values, int32_t nValues, int32_t* bytesConsumed);
int32_t CanTp_UnpackSgl(... float* values ...);
```

Scans records from the start of `frames`, ignores those that do not belong
to the slot's message, reassembles a BAM sequence, and on completion writes
one value per signal row. Returns 1 with `bytesConsumed` just past the
completing record (call again from there for the next message), 0 if the
buffer holds no complete message, or an error. Does not disturb the live
receive state of the slot.

### Live receive (stateful, one record per call)

```c
int32_t CanTp_RxFeed (int32_t slot, const uint8_t* frame, int32_t frameLen,
                      double* values, int32_t nValues);
int32_t CanTp_RxReset(int32_t slot);
```

Feed every record from XNET Read (Frame Raw) into every slot you listen
for; a slot returns 1 on the record that completes its message. A new TP.CM
restarts reassembly; a missing packet abandons the sequence until the next
TP.CM. Foreign frames are ignored.


### Frame lengths and the one-call read/write (v1.2.0)

```c
int32_t CanTp_FrameLengths(const uint8_t* frames, int32_t framesLen, uint8_t* lens, int32_t lensLen);
int32_t CanTp_Transfer   (int32_t slot, int32_t mode, double* values, int32_t nValues,
                          uint8_t* frames, int32_t framesLen, uint8_t* frameLens, int32_t frameLensLen,
                          uint64_t timestamp100ns, uint64_t spacing100ns,
                          int32_t* bytesUsed, int32_t* nFrames);
int32_t CanTp_TransferSgl(... float* values ...);
```

`CanTp_FrameLengths` walks a record array and writes the PayloadLength
(DLC in bytes, 0..64) of every record into `lens`; it returns the record
count even when `lens` is shorter (only the first `lensLen` are written),
or −8 on a malformed buffer. For classic CAN and J1939 every record is 24
bytes anyway; the array matters for CAN FD and for classic frames shorter
than 8 bytes.

`CanTp_Transfer` is `CanTp_Pack` and `CanTp_Unpack` behind one CLFN with a
`mode` input, plus the length array:

| mode | values | frames | frameLens | `*bytesUsed` | `*nFrames` | returns |
|---|---|---|---|---|---|---|
| 0 `CANTP_MODE_WRITE` | in | out: the records of one sequence, TP.CM first | out: DLC per record (optional: NULL/0) | bytes written | records written | 0, or −6 when `frames` or `frameLens` is too small (`*bytesUsed` / `*nFrames` = needed) |
| 1 `CANTP_MODE_READ` | out | in: records, TP.CM first | in (optional): when `frameLensLen` > 0 the records are walked with these lengths, which must agree with the headers (else −8), and at most `frameLensLen` records are read | bytes consumed | records consumed | 1 message decoded, 0 nothing complete, negative error |

Read mode is stateless like `CanTp_Unpack` (the live `RxFeed` state is
untouched) and skips records that do not belong to the slot. Multi-frame
RTS/CTS and ISO-TP messages still need the session calls in write mode.

| CLFN param (Transfer) | Type | Pass |
|---|---|---|
| slot | I32 | Value |
| mode | I32 (0 write, 1 read) | Value |
| values | Array DBL 1-D (SGL for TransferSgl), nSig elements | Array Data Pointer |
| nValues | I32 | Value |
| frames | Array U8 1-D, pre-sized to `CanTp_OutputSize` for writing | Array Data Pointer |
| framesLen | I32 | Value |
| frameLens | Array U8 1-D, pre-sized to `CanTp_FrameCount` for writing | Array Data Pointer |
| frameLensLen | I32 (0 to skip) | Value |
| timestamp100ns, spacing100ns | U64 | Value |
| bytesUsed | I32 | Pointer to Value |
| nFrames | I32 | Pointer to Value |

### Multiplexed signals

```c
int32_t CanTp_DefineMux(int32_t slot, const double* muxDefs, int32_t nSig);   // nSig x CANTP_MUXDEF_COLS (2)
```

Optional second table, one row per signal row of the definition, loaded
after `CanTp_Define` (which clears it): column 0 is the row index of the
multiplexor signal that selects this one (−1 = always present, also for the
multiplexor itself), column 1 the selector value (the DBC `m<n>`, compared
with the multiplexor's **raw** value). Pack leaves unselected signals as pad;
Unpack/RxFeed/RxStep return NaN for them. A multiplexor may itself be
multiplexed (one level per row; cycles are rejected). `dbc2tables.py` writes
the table as `<Message>.mux.csv` / `"muxdefs"` / `<Message>_muxdefs` whenever
a message has `m<n>` signals; a signal that exists for several selector
values becomes one row per value.

### Sessions: J1939 RTS/CTS and ISO-TP

Transports 4 and 5 need frames from the peer (CTS / flow control) and
timers, so a multi-frame message is a **session** driven by the caller's
receive loop. Frames the library wants sent come back in `out`; frames
received from the bus go in through `frame`; `nowMs` is a free-running
millisecond tick (LabVIEW Tick Count) used only as differences.

```c
int32_t CanTp_SessionConfig(int32_t slot, const double* cfg, int32_t cfgLen);   // CANTP_SESSION_COLS (6)

int32_t CanTp_TxStart   (int32_t slot, const double* values, int32_t nValues, uint32_t nowMs,
                         uint64_t timestamp100ns, uint64_t spacing100ns,
                         uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t CanTp_TxStartSgl(...same with const float* values...);
int32_t CanTp_TxFeed    (int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                         uint64_t timestamp100ns, uint64_t spacing100ns,
                         uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t CanTp_TxState   (int32_t slot);      // 0 idle, 1 waiting for the peer, 2 done, negative = last error
int32_t CanTp_TxReset   (int32_t slot);

int32_t CanTp_RxStep    (int32_t slot, const uint8_t* frame, int32_t frameLen, uint32_t nowMs,
                         uint64_t timestamp100ns, double* values, int32_t nValues,
                         uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t CanTp_RxStepSgl (...same with float* values...);
int32_t CanTp_RxState   (int32_t slot);      // 0 idle, 1 a transfer is in progress
```

**Sender.** `TxStart` packs the values and writes the RTS (J1939) or the
FirstFrame (ISO-TP); returns 0 (waiting) — or `CANTP_DONE` right away for
single-frame messages and for BAM / classic / CAN FD definitions, which it
emits whole. Then call `TxFeed` for **every** record received from the bus
(and with `frame = NULL` now and then, e.g. once per loop iteration, so the
timers run). When the peer's CTS / flow control arrives the call writes the
next window of TP.DT / ConsecutiveFrames into `out`; send whatever comes
back regardless of the return code. Returns `CANTP_DONE` on the
EndOfMsgAck (J1939) or after the last ConsecutiveFrame (ISO-TP), a negative
code when the peer aborted or the timeout expired (an abort frame is in
`out` for J1939). `TxStart` while a session is waiting returns
`CANTP_ERR_BUSY`; `TxReset` clears it.

**Receiver.** Call `RxStep` for every received record (and with `frame =
NULL` for the timers). It answers the RTS with a CTS window, each completed
window with the next CTS, the last packet with the EndOfMsgAck, and an
ISO-TP FirstFrame / block with a flow control — all in `out`. It returns
`CANTP_FOUND` when the message is complete (`values` filled), a negative
code on abort / timeout (J1939: an abort frame in `out`). Missing packets
inside a CTS window are re-requested at the end of the window (J1939-21),
twice at most. `RxStep` also handles BAM, classic and CAN FD definitions
exactly like `RxFeed`, so one receive loop serves every transport.

**Addresses.** J1939 RTS/CTS uses the definition's SA (sender) and DA
(receiver): both nodes load the same table; the receiver answers from DA to
the sender. With the SA placeholder 0xFE the receiver accepts any sender.
ISO-TP data travels on the definition's id; the peer's flow control is
expected on the *peer id* (column 0 of the session config), which defaults
to the id with its two address bytes swapped for 29-bit ids (normal fixed
addressing, 0x18DA**tt**ss ↔ 0x18DA**ss**tt) and to id + 8 for 11-bit ids
(the UDS 0x7E0/0x7E8 convention). A receiver listens on the id and answers
on the peer id; a sender sends on the id and listens on the peer id.

**Session configuration** (`CANTP_SESSION_COLS = 6` doubles, defaults
apply until set; `CanTp_Define` restores the defaults):

| Col | Meaning | Default |
|---|---|---|
| 0 | peer id (ISO-TP flow control), −1 = default | see above |
| 1 | peer id extended (0/1), −1 = same as the definition | definition |
| 2 | receiver window: packets per CTS (J1939, capped by the sender's RTS limit) / ISO-TP block size BS; 0 = all | 0 |
| 3 | receiver STmin (ms, 0..127) announced in the ISO-TP flow control | 0 |
| 4 | timeout ms waiting for the peer; 0 = protocol default | J1939 T3 1250 (sender), T1 750 (receiver waiting for a DT); ISO-TP N_Bs / N_Cr 1000 |
| 5 | sender: max packets per CTS offered in the RTS (1..255); 0 = 255 | 255 |

**Buffers and timestamps.** `CanTp_OutputSize(slot)` = (1 + packets) × 24
is always enough for one call (the largest thing a call writes is a whole
CTS window). The first record of a call gets `timestamp100ns`, each further
one `spacing100ns` later; for ISO-TP ConsecutiveFrames the spacing is at
least the STmin the peer asked for, so a caller that replays the timestamps
(XNET Replay Exclusive) honours it automatically.

**Logs.** `CanTp_Unpack` over a recorded buffer reassembles RTS/CTS and
ISO-TP transfers too (it needs no responses), so `.ncl` files of a handshake
decode like any other.

### Raw record helpers

```c
int32_t CanTp_RecordSize   (const uint8_t* rec, int32_t avail);   // size of the record at rec
int32_t CanTp_RecordSizeFor(int32_t payloadLen);                  // 24 for <= 8, 32.. for FD
int32_t CanTp_MakeRecord   (uint32_t id, int32_t extended, int32_t frameType,
                            const uint8_t* payload, int32_t payloadLen,
                            uint64_t timestamp100ns, uint8_t* out, int32_t outLen);
int32_t CanTp_NclHeader    (uint8_t* out, int32_t outLen);        // 12-byte .ncl header
```

`CanTp_MakeRecord` builds a record from an id/payload pair, for callers that
receive frames from something other than XNET (SocketCAN, a Kvaser or PEAK
API) and want to feed `CanTp_RxFeed`. `frameType` is 0x00 / 0x10 / 0x18.

## Worked example: a 40-byte J1939 PG (EC1)

Tables from `dbc2tables.py J1939_NGHD_V130.dbc --message EC1 --sa 0x00`:

```
msgDef  = { 419357694, 1, 40, 1, 0, 255, 255, 0 }      // 0x18FEE3FE, BAM, SA 0x00
sigDefs = { {  0, 16, 0, 0, 0.125, 0, 0, 8031.875 },   // EngSpeedAtIdlePoint1
            { 16,  8, 0, 0, 1, -125, -125, 125 },       // ...
            ... 28 rows ... }
CanTp_Define(1, msgDef, 8, sigDefs, 28);
CanTp_OutputSize(1)  -> 168  (1 TP.CM + 6 TP.DT, 24 bytes each)
CanTp_Pack(1, values, 28, 0, 500000, out, 168, &n)
```

Output records: `1CECFF00  20 28 00 06 FF E3 FE 00` then `1CEBFF00  01 …`
to `06 …`, last packet padded 0xFF. Verified byte-for-byte against cantools
and reassembled by pretty_j1939 (see TESTING.md).

## Calling from other languages

Python (`tests/oracle_test.py`, `tools/pi_bus_loop.py` and
`tools/pi_session_loop.py` are complete examples):

```python
import ctypes as C
lib = C.CDLL("cantp.dll")                     # or "./libcantp.so"
lib.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
```

C/C++: include `cantp.h`, link `cantp.lib` (x64/x86) or `-lcantp`. .NET:
`[DllImport("cantp", CallingConvention = CallingConvention.Cdecl)]`.
