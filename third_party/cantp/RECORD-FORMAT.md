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
