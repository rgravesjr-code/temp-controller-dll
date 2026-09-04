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
| J1939 with DA ≠ 0xFF (RTS/CTS), ISO 15765-2 | 4, 5 | reserved for release 2; `CanTp_Define` returns `CANTP_ERR_TRANSPORT` |

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

Multiplexed signals (`m0`, `M`) are not supported in release 1; the
converter skips them with a warning.

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
your message names).
