# CanTp frame array: byte-level format

The U8 array that `CanTp_Pack`, `CanTp_Transfer`, `CanTp_TxStart` and friends
write, and that `CanTp_Unpack`, `CanTp_RxFeed`, `CanTp_RxStep` and
`CanTp_Transfer` read, is a sequence of **NI-XNET raw frame records**. This is
the record NI-XNET uses for *Frame Input/Output Stream* sessions in raw mode
and for every event in an `.ncl` logfile, so the array can be handed to XNET
Write, appended to a log, or produced by XNET Read without conversion.

## 1. One record

All fields little-endian.

| Offset | Size | Field | Contents |
|---|---|---|---|
| 0 | 8 | Timestamp | U64, 100 ns ticks since 1601-01-01 00:00 UTC. 0 = none. `CanTp_Pack` stamps frame *i* with `timestamp100ns + i × spacing100ns` |
| 8 | 4 | Identifier | U32. Bits 0..28 = CAN id (11-bit ids use bits 0..10). Bit 29 (`0x20000000`) set = 29-bit extended id. Bits 30, 31 = 0 |
| 12 | 1 | Type | `0x00` CAN data frame, `0x10` CAN FD data frame, `0x18` CAN FD data frame with bit-rate switch. Other XNET types (remote, error, log trigger …) are skipped on input |
| 13 | 1 | Flags | 0 |
| 14 | 1 | Info | 0 |
| 15 | 1 | PayloadLength | Number of payload bytes 0..8 (classic) or 0..64 (CAN FD). This is the frame DLC in bytes; it is what `CanTp_FrameLengths` / the `frameLens` array of `CanTp_Transfer` report |
| 16 | 8 or more | Payload | The data bytes, then zero fill up to the record size |

**Record size** = 24 bytes when PayloadLength ≤ 8, otherwise
`24 + ((PayloadLength − 1) & ~7)`:

| PayloadLength | Record size |
|---|---|
| 0..8 | 24 |
| 9..16 | 32 |
| 17..24 | 40 |
| 25..32 | 48 |
| 33..40 | 56 |
| 41..48 | 64 |
| 49..56 | 72 |
| 57..64 | 80 |

Every classic CAN and J1939 record is therefore 24 bytes: a fixed
stride. Only CAN FD records vary, and `CanTp_FrameLengths` (or
`CanTp_RecordSize` per record) tells the stride.

A CAN FD payload is padded up to the next valid FD length (12, 16, 20, 24,
32, 48, 64) with the message's pad byte before the record is built, so
PayloadLength is always a legal DLC.

## 2. Worked bytes

### 2.1 Classic CAN, 11-bit id 0x123, 3 data bytes

`CanTp_Pack` on a message defined as `{ 0x123, 0, 3, 0, -1, 255, 0, 0 }` with
one 8-bit signal = 5, timestamp 0:

```
offset  00 01 02 03 04 05 06 07 | 08 09 0A 0B | 0C 0D 0E 0F | 10 11 12 13 14 15 16 17
bytes   00 00 00 00 00 00 00 00 | 23 01 00 00 | 00 00 00 03 | 05 00 00 00 00 00 00 00
        timestamp = 0             id = 0x123    type=0 len=3  payload 05 00 00, zero fill
```

PayloadLength 3 is what goes on the bus as DLC; the five trailing bytes are
record fill, not data.

### 2.2 J1939 PG under 8 bytes, 29-bit id 0x18FEEEFE (ET1)

```
00 00 00 00 00 00 00 00 | FE EE FE 38 | 00 00 00 08 | 82 55 xx xx xx xx 46 7D
                          ^ 0x38FEEEFE = 0x18FEEEFE | 0x20000000 (extended flag)
```

The extended flag lives in the record only; the id on the bus is 0x18FEEEFE.

### 2.3 J1939 BAM, 40-byte PG 0xFEE3 from SA 0x00 (EC1)

`CanTp_Pack` writes 7 records of 24 bytes = 168 bytes, TP.CM first:

```
record 0  id 1CECFF00 (+ext)  payload 20 28 00 06 FF E3 FE 00
                                       |  |     |  |  PGN FEE3 (LE24)
                                       |  |     |  reserved FF
                                       |  |     packet count 6
                                       |  total size 40 (LE16)
                                       control 0x20 = BAM
record 1  id 1CEBFF00 (+ext)  payload 01 d0 d1 d2 d3 d4 d5 d6      bytes 0..6
record 2  id 1CEBFF00 (+ext)  payload 02 d7 … d13                   bytes 7..13
…
record 6  id 1CEBFF00 (+ext)  payload 06 d35 d36 d37 d38 d39 FF FF  bytes 35..39, pad FF
```

TP.CM uses PGN 0xEC00, TP.DT PGN 0xEB00, both priority 7, destination 0xFF,
source = the message SA. Every TP.DT payload is 8 bytes (the last one padded
with 0xFF), so all seven `frameLens` entries are 8. Frame *i* carries
timestamp `t0 + i × spacing`; put 50 ms (`spacing100ns = 500000`) between
TP.DT frames on the bus (J1939-21 allows 50..200 ms).

### 2.4 CAN FD, 20-byte payload

A 20-byte FD message packs into one **40-byte** record: PayloadLength = 20,
type `0x10` (or `0x18` with BRS), payload at offset 16..35, zero fill 36..39.

### 2.5 RTS/CTS and ISO-TP

Session calls write the same records: a J1939 RTS (`10 size size n maxPerCts
pgn pgn pgn` on PGN 0xEC00 to the destination), CTS / EndOfMsgAck from the
receiver, TP.DT windows; ISO-TP FirstFrame / ConsecutiveFrames / FlowControl
in 8-byte classic records. See the package guide, section "Sessions".

## 3. Bit numbering inside the payload

Signals are placed with DBC semantics over the **reassembled message**
(after TP reassembly, before splitting into TP.DT packets):

- **Intel (little-endian, DBC `@1`, ECD "Intel")**: the start bit is the
  position of the signal's least significant bit, counted linearly:
  byte *n* bit *b* = *n* × 8 + *b*. A 16-bit signal at start 256 occupies
  bytes 32 (low) and 33 (high). Multi-byte values are little-endian.
- **Motorola (big-endian, DBC `@0`, ECD "Motorola")**: the start bit is the
  position of the signal's most significant bit in sawtooth numbering (bit
  7 of byte 0 is position 7, bit 0 of byte 1 is position 8, and the signal
  continues downward through the bits of a byte and then into the next
  byte). This is exactly the number CANdb++ shows, for payloads of any
  length.
- Signed values are two's complement in the signal's bit width; float
  channels are IEEE 754 single (32 bits) or double (64 bits) in the byte
  order of the signal.
- `physical = raw × factor + offset`; NaN packs as "not available" (all
  bits 1).
- Bits no signal covers hold the message pad byte (0xFF for J1939).

## 4. The `.ncl` logfile

An NI-XNET logfile is the 12-byte header `4E 49 00 03 01 01 02 00 01 00 00
00` (`CanTp_NclHeader`) followed by records exactly as above. Appending the
output of `CanTp_Pack` to such a file gives a log XNET Bus Monitor and
LabVIEW's XNET logfile VIs read directly.

## 5. Walking an array in LabVIEW

- Classic / J1939 only: `Reshape Array` the U8 array into rows of 24, or
  index `i × 24`.
- Mixed or CAN FD: call `CanTp_FrameLengths(frames, n, lens, maxFrames)`
  once; the record at index *i* starts at `24 × i` plus the extra bytes of
  the FD records before it, or simply use `CanTp_RecordSize` in a loop.
- `CanTp_Transfer` in write mode fills the `frameLens` array for you; in
  read mode it accepts the same array and checks it against the records.

## 6. The XNET Frame CAN cluster array (v1.3.0)

`CanTp_TransferXnet`, `CanTp_RecordsToXnet` and `CanTp_XnetToRecords` use a
second format: the bytes LabVIEW's **Flatten To String** produces for a 1-D
array of the NI-XNET **XNET Frame CAN** cluster, big-endian with array
sizes prepended (the defaults). The element order is the cluster's *type*
order, taken from the `XNET Frame CAN.ctl` type descriptor, and it is not
the front-panel order:

| Bytes | Field |
|---|---|
| 4 | I32 frame count |
| 8 | I64 timestamp: seconds since 1904-01-01 00:00:00 UTC |
| 8 | U64 timestamp: fraction of a second in units of 2⁻⁶⁴ |
| 4 | I32 payload length |
| n | payload |
| 4 | U32 identifier, bare 11- or 29-bit |
| 1 | type (0 CAN Data, 1 CAN Remote, 2 CAN Bus Error, 8 CAN 2.0 Data, 16 CAN FD Data, 24 CAN FD+BRS Data, 192 J1939 Data, 224 Delay, 225 Log Trigger, 226 Start Trigger) |
| 1 | extended? |
| 1 | echo? |

So a frame is 27 + n bytes and the array is 4 + Σ(27 + n). The type
numbers are NI-XNET's own (`nxFrameType_*`) and equal the record's type
byte, so the two formats convert without a table.

### 6.1 Two frames flattened by LabVIEW (Scott, 2026-09-14)

`tests/fixtures/xnet_two_frames_scott_2026-09-14.bin`, 74 bytes:

```
00 00 00 02                                        I32 count = 2
-- frame 0 --
00 00 00 00 E6 CD A5 A2                            I64 seconds = 3872236962 -> 2026-09-14 13:22:42 UTC
77 86 B6 68 06 79 96 C4                            U64 fraction = 0.4668993 s
00 00 00 08                                        I32 payload length = 8
01 02 03 04 05 06 07 08                            payload
18 FE F1 00                                        U32 identifier = 0x18FEF100 (PGN FEF1 from SA 0)
00                                                 type = 0 CAN Data
01                                                 extended? = TRUE
00                                                 echo? = FALSE
-- frame 1 --
00 00 00 00 E6 CD A5 A2  77 86 B6 68 06 79 96 C4   same instant
00 00 00 08  10 20 30 40 50 60 70 80               8 bytes
18 FE F1 01  00  01  00                            0x18FEF101, CAN Data, extended, no echo
```

`CanTp_XnetToRecords` turns this into two 24-byte records with timestamp
`0x01DD444C1F523B41` (134338657624668993 × 100 ns since 1601) and identifiers
`0x38FEF100` / `0x38FEF101` (bit 29 set = extended); `CanTp_RecordsToXnet`
gives the 74 bytes back exactly, including the fraction.

### 6.2 What the library writes

For the 20-byte BAM of §2.3-style messages, `xnetMode` 0 produces the same
frames as `CanTp_Pack` — TP.CM under `1CECFF<SA>` then the TP.DT packets
under `1CEBFF<SA>`, all type CAN Data, extended, echo? FALSE — as an array
of `4 + 4 × 35` bytes. With `timestamp100ns` = 0 the timestamp fields are
all zero (LabVIEW's 1904-01-01 "not set"), otherwise they carry the
converted time, and `spacing100ns` advances each frame like `CanTp_Pack`.

`xnetMode` 1 produces one frame of type J1939 Data (192) with the whole
payload (length = the message's byte count, pad applied) under the message
id with SA and DA applied, for an NI-XNET J1939 session:

```
00 00 00 01                                        count 1
00.. (16 bytes)                                    timestamp
00 00 00 14                                        20 bytes
<20 payload bytes>
18 FF 20 80                                        0x18FF2080
C0  01  00                                         J1939 Data, extended, no echo
```

### 6.3 Timestamps

Record: U64, 100 ns since 1601-01-01. LabVIEW: I64 seconds since
1904-01-01 plus a U64 fraction in units of 2⁻⁶⁴ (the 16 flattened bytes).
The offset between the epochs is 9 561 628 800 s. `CanTp_TimeToLabView`
computes `fraction = rem × 2⁶⁴ / 10⁷` exactly (integer arithmetic) and
`CanTp_TimeFromLabView` rounds back to the nearest 100 ns, so a record
time survives a round trip unchanged and a LabVIEW time is reproduced
byte-exact when it came from a 100 ns source (Scott's sample above does).
0 maps to 0/0 in both directions.
