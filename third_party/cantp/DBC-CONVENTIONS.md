# DBC conventions for CanTp

How a `.dbc` message maps onto a CanTp message definition, and what a DBC
must contain so that `tools/dbc2tables.py` converts it without hand edits.
The conventions are the ones Vector CANdb++ / CANalyzer use for J1939 and
CAN FD databases; `J1939_NGHD_V130.dbc` (EC1, RC, TCFG, …) is the reference.

## 1. One `BO_` = one CanTp slot

```
BO_ 2566841342 EC1: 40 Vector__XXX
 SG_ EngReferenceTorque : 152|16@1+ (1,0) [0|64255] "Nm" Vector__XXX
 ...
BA_ "VFrameFormat" BO_ 2566841342 3;
BA_ "GenMsgCycleTime" BO_ 2566841342 5000;
```

| DBC element | CanTp msgDef column | Rule |
|---|---|---|
| `BO_ <id>` | 0 `id` | The number as written. Bit 31 set means "extended" in DBC files; CanTp strips it and reads the low 29 bits |
| id ≥ 2³¹ or `VFrameFormat` ∈ {ExtendedCAN, J1939PG, ExtendedCAN_FD} | 1 `extended` = 1 | 11-bit otherwise |
| `BO_ … : <length>` | 2 `length` | Payload bytes. ≤ 8 classic; 9..1785 J1939 transport; 9..64 CAN FD |
| `VFrameFormat` | 3 `transport` | see §2 |
| low byte of a 29-bit id | 4 `sa` | `0xFE` is Vector's "any source" placeholder. Leave `sa = -1` to keep it (CanTp then accepts any sender on receive and sends with SA 0xFE), or set the node's real SA and CanTp substitutes it in every frame |
| PS byte for PDU1 PGNs | 5 `da` | Destination address; 255 = broadcast (required for BAM). PDU2 PGNs ignore it |
| — | 6 `pad` | Fill byte for bits no signal covers: 255 for J1939 (spec), 0 or 0xCC/0xAA otherwise |
| `GenMsgCycleTime` | 7 `cycleMs` | Informational |

## 2. Transport selection

| DBC | `transport` | Frames produced |
|---|---|---|
| length ≤ 8, any `VFrameFormat` except `*_FD` | 0 classic | one frame, DLC = length |
| length 9..1785 and `VFrameFormat = J1939PG` (3), or a 29-bit id with no FD attribute | 1 J1939 BAM | length ≤ 8 → one frame under the PGN; else TP.CM(BAM) + ⌈length/7⌉ TP.DT, priority 7, DA 0xFF, pad 0xFF, sequence 1..N |
| `VFrameFormat = StandardCAN_FD` (14) / `ExtendedCAN_FD` (15), or cantools `is_fd` | 2 CAN FD | one frame, length padded up to the next valid FD DLC (12, 16, 20, 24, 32, 48, 64) with `pad`; record type 0x10 |
| same + `BA_ "CANFD_BRS" BO_ <id> 1` | 3 CAN FD BRS | as above, record type 0x18 |
| length 9..1785, `VFrameFormat = J1939PG` and `dbc2tables.py --da <addr>` with addr ≠ 255 | 4 J1939 RTS/CTS | TP.CM(RTS) to DA, then TP.DT windows on the receiver's CTS, EndOfMsgAck from the receiver; a session (see the package guide). length ≤ 8 → one PDU frame to DA |
| custom attribute `TpProtocol = "ISOTP"` on the `BO_` (`BA_DEF_ BO_ "TpProtocol" STRING ;`), or `dbc2tables.py --isotp <Message>` | 5 ISO-TP | ISO 15765-2 on classic CAN, normal addressing: SingleFrame (≤ 7 bytes), else FirstFrame + ConsecutiveFrames paced by the receiver's FlowControl on the peer id; a session. Pad 0xCC |

A J1939 PG is identified by the PGN, not by the full id: on receive CanTp
compares `(id >> 8) & 0x3FFFF` (with the PS byte masked for PDU1) and the SA
rule above, so the priority bits in the incoming frame may differ.

## 3. Signals (`SG_`)

```
 SG_ Name : <start>|<length>@<order><sign> (<factor>,<offset>) [<min>|<max>] "unit" Receiver
```

| DBC | CanTp sigDef column | Rule |
|---|---|---|
| `<start>` | 0 `startBit` | Verbatim. Intel (`@1`): position of the LSB, numbered linearly over the whole payload (byte n bit b = n·8 + b), so a 40-byte PG uses 0..319. Motorola (`@0`): position of the MSB in sawtooth numbering, continuing across bytes exactly as in an 8-byte frame |
| `<length>` | 1 `bitLength` | 1..64 |
| `@1` / `@0` | 2 `byteOrder` | 0 Intel, 1 Motorola |
| `+` / `-` | 3 `valueType` | 0 unsigned, 1 signed two's complement |
| `SIG_VALTYPE_ <id> Name : 1;` / `: 2;` | 3 `valueType` | 2 IEEE float32 (length must be 32), 3 IEEE float64 (64) |
| `(<factor>,<offset>)` | 4, 5 | `physical = raw × factor + offset`; factor must be non-zero |
| `[<min>\|<max>]` | 6, 7 | Physical clamp on pack; ignored when max ≤ min (the DBC default `[0|0]`) |

### Multiplexed signals

```
 SG_ AxleLocation M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ AxleWeight_1F m31 : 8|16@1+ (0.5,0) [0|32127.5] "kg" Vector__XXX
```

The multiplexor (`M`) is an ordinary row; every `m<n>` signal is a row whose
mux entry is `[row of the multiplexor, n]`; plain signals get `[-1, 0]`.
`dbc2tables.py` writes this second table (`<Message>.mux.csv`, `"muxdefs"`,
`<Message>_muxdefs`) and it is loaded with `CanTp_DefineMux` after
`CanTp_Define`. A signal defined for several values (`SG_MUL_VAL_` ranges) is
emitted once per value with the name suffixed `@m<n>`. Extended
multiplexing (a multiplexor that is itself multiplexed) works one level per
row. On pack an unselected signal leaves its bits as pad; on unpack it reads
as NaN. Verified against cantools on the Vector VW message (17 rows).

## 4. Values on the wire

- Pack: physical → clamp to [min, max] → `(phys − offset) / factor` → round
  half away from zero → saturate to the bit width → place bits. A **NaN**
  input packs the J1939 "not available" pattern (all bits 1; float types:
  NaN).
- Unpack: bits → raw (sign-extended for signed) → `raw × factor + offset`.
  No NA mapping on unpack; a receiver that cares checks for the all-ones raw
  value itself.
- Overlapping signals are not detected; the later row overwrites.
- Bits no signal covers carry `pad`.

## 5. The `SPN`, `GenSigStartValue` and other attributes

Ignored by CanTp. `dbc2tables.py` keeps signal names and units in the JSON
and `.names.txt` outputs for documentation and for the LabVIEW front panel,
but the library only ever sees numbers.

## 6. Writing a new multipacket message (temp-controller example)

```
BO_ 2566848254 TEMPCTL: 60 Vector__XXX          ; 0x98FF00FE: prio 6, PGN 65280, SA any
 SG_ Setpoint     : 0|16@1+ (0.03125,-273) [-273|1735] "degC" Vector__XXX
 SG_ Temp1        : 16|16@1+ (0.03125,-273) [-273|1735] "degC" Vector__XXX
 SG_ ErrorTimeout : 32|16@1+ (1,0) [0|65535] "ms" Vector__XXX
 SG_ HeatingCmd   : 48|2@1+ (1,0) [0|3] "" Vector__XXX
 ...
BA_ "VFrameFormat" BO_ 2566848254 3;
BA_ "GenMsgCycleTime" BO_ 2566848254 100;
```

`dbc2tables.py file.dbc --message TEMPCTL --sa 0x80` produces the two tables
the RT code loads once with `CanTp_Define`; every tick then calls
`CanTp_PackSgl` with the controller's output array in signal order.

## 7. Checking a DBC against the library

`tests/oracle_test.py` packs every message you name with both CanTp and
cantools and compares the bytes, then unpacks with both and compares the
values. Run it on a new DBC before deploying the tables:

```
python tests\oracle_test.py build\win-x64\cantp.dll --dbc my.dbc
```

(section (c) picks EEC1/EC1/RC/ET1/TCFG when present; edit the list for
your message names; section (h) does the same for every cluster of an
`.ecd` file through `CanTp_DefineFlat`).

## 8. The ECD / `J1939Msg(V4)` cluster (v1.2.0)

`CanTp_DefineFlat` accepts the flattened LabVIEW cluster instead of the
tables. The mapping is one-to-one with §1 and §3:

| Cluster | Table | Note |
|---|---|---|
| message ID, extended? | msg 0, 1 | id > 0x7FF or bit 31 set ⇒ extended, whatever the flag says (three messages in `J1939_NGHD_V130.ecd` have the flag clear on a 29-bit id) |
| NumDataBytes | msg 2 | |
| UpdateRate | msg 7 | when > 0 |
| start bit, number of bits | sig 0, 1 | |
| byte order 0 Intel / 1 Motorola | sig 2 | |
| data type 0 Signed / 1 Unsigned / 2 IEEE Float | sig 3 = 1 / 0 / 2 (32 bit) or 3 (64 bit) | note the swapped numbering |
| scaling factor, offset, min, max | sig 4..7 | |
| default | `CanTp_Defaults` | |

Transport, SA and DA are not in the cluster; §2 applies with the id's own
bytes (derivation rules in the package guide), and the `transport` / `sa`
arguments override.

**Motorola start bits.** The library uses the DBC rule in §3 (start = MSB
position, sawtooth). `J1939_NGHD_V130.ecd` has four Motorola channels in
754 messages; two of them (`ISO15765_*.FirstFrameDataLength`, ECD `8|12`)
would be `3|12@0` in the DBC and two (`ETC2.TransCurrentRange` ECD `48|16`,
`TransRqedRange` `32|16`) are `55|16@0` / `39|16@0` — the ECD numbers follow
neither the DBC rule nor one single other rule. Until the convention used by
the ECD editor is confirmed, Motorola channels from a cluster should be
checked against the DBC (the oracle does this by name), and a start bit that
does not fit is rejected with −5 rather than silently shifted. Intel
channels (all 6429 others) map verbatim.

