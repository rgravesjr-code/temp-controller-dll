# CanTp — Package Guide (API reference)

`CanTp` is a **pure C library with flat `extern "C"` exports** that turns
physical signal values into CAN frames of a transport sequence and back. It is
built for LabVIEW's Call Library Function Node (CLFN) and callable from
C/C++, Python (ctypes), .NET P/Invoke, MATLAB. Files: `cantp.dll` (Windows
x64, x86), `libcantp.so` (Linux x86_64 for cRIO-904x/905x/906x, aarch64 for
Raspberry Pi 4/5). One header, one ABI, every build.

**Model.** A message definition (one DBC `BO_` with its `SG_` rows) is loaded
once into a slot with `CanTp_Define`. Every later `CanTp_Pack` on that slot
converts a value array into the frames of one complete transport sequence;
`CanTp_Unpack` / `CanTp_RxFeed` do the reverse. The library never reads a
`.dbc`; `tools/dbc2tables.py` produces the tables on the host.

**Calling convention:** C (cdecl). **Types:** `int32_t`, `uint32_t`,
`uint64_t`, `uint8_t`, `float`, `double`; arrays as pointer + `int32_t`
length; no structs, strings, callbacks or allocation. **Return:** `int32_t`,
0 ok, 1 = "message decoded" for the receive calls, negative = error.
**State:** 32 slots × up to 128 signals, static (about 256 KB), plus a
per-slot receive buffer. No locks: use a slot from one thread at a time;
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
| `CANTP_ERR_TRANSPORT` | −7 | Transport reserved for release 2 (RTS/CTS, ISO-TP), or BAM with DA ≠ 255 |
| `CANTP_ERR_RECORD` | −8 | Malformed raw record in the input (fewer than 24 bytes, PayloadLength > 64, record longer than the buffer) |
| `CANTP_ERR_TOO_MANY` | −9 | More than 128 signals |

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
32, 40, 48, 56 or 80 bytes. `CanTp_RecordSize` tells the size of the record
at a given offset, so a caller can walk a mixed array.

## Message definition row — `CANTP_MSGDEF_COLS = 8` doubles

| Col | Name | Meaning |
|---|---|---|
| 0 | id | CAN id as in the DBC (11-bit, or 29-bit priority/PGN/SA; DBC bit 31 ignored) |
| 1 | extended | 0 = 11-bit, 1 = 29-bit. Also implied by a DBC id with bit 31 set |
| 2 | length | Payload bytes: 0..8 classic; 0..64 CAN FD; 0..1785 J1939 BAM |
| 3 | transport | 0 classic, 1 J1939 BAM, 2 CAN FD, 3 CAN FD BRS (4 RTS/CTS and 5 ISO-TP reserved) |
| 4 | sa | Source address override 0..253, or −1 = use the id's low byte. A 29-bit id whose low byte is the Vector placeholder 0xFE with no override means "any sender" on receive |
| 5 | da | Destination address, 255 = broadcast (BAM requires 255). Applied to the PS byte of PDU1 PGNs |
| 6 | pad | Fill byte for bits no signal covers (J1939: 255) |
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
uint32_t CanTp_Version(void);                 // 0x010000 = 1.0.0
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

Python (`tests/oracle_test.py` and `tools/pi_bus_loop.py` are complete
examples):

```python
import ctypes as C
lib = C.CDLL("cantp.dll")                     # or "./libcantp.so"
lib.CanTp_Define.argtypes = [C.c_int32, C.POINTER(C.c_double), C.c_int32, C.POINTER(C.c_double), C.c_int32]
```

C/C++: include `cantp.h`, link `cantp.lib` (x64/x86) or `-lcantp`. .NET:
`[DllImport("cantp", CallingConvention = CallingConvention.Cdecl)]`.
