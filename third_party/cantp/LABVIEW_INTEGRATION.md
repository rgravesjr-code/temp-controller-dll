# CanTp — LabVIEW and cRIO integration guide

The "what goes where and what do I type in the CLFN" companion to
`CANTP_PACKAGE_GUIDE.md`.

## 1. Files

| Target | File | Where it goes |
|---|---|---|
| cRIO-904x/905x/906x (x86_64 NI Linux RT) | `linux-x64\libcantp.so` | `/usr/local/lib/libcantp.so` |
| Raspberry Pi 4/5 64-bit (test bench) | `linux-arm64\libcantp.so` | `/usr/local/lib/libcantp.so` |
| self-test binaries | `linux-x64\test_cantp`, `linux-arm64\test_cantp` | anywhere, run once |
| Windows 64-bit LabVIEW | `cantp.dll` | next to the VI/EXE or on PATH |
| Windows 32-bit LabVIEW | `x86\cantp.dll` | same |
| C/C++ | `cantp.h`, `cantp.lib`, `x86\cantp.lib` | your project |
| host tools | `tools\dbc2tables.py`, `tools\pi_bus_loop.py` | PC with Python + cantools / Pi with python-can |

## 2. Deploy

```bat
scp linux-x64\libcantp.so admin@<crio>:/usr/local/lib/
scp linux-x64\test_cantp  admin@<crio>:/home/admin/
ssh admin@<crio> "chmod 755 /usr/local/lib/libcantp.so /home/admin/test_cantp && /home/admin/test_cantp"
```

Expected: `177 passed, 0 failed`. Same for the Pi with the `linux-arm64`
files (user `pi`). One `.so` does not run on both: the cRIO is x86_64, the
Pi is aarch64. Both are built from the same source and the test binary
proves each.

## 3. Generate the tables once, on the PC

```bat
pip install cantools
python tools\dbc2tables.py MyBus.dbc --message TEMPCTL --message EC1 --sa 0x80 --out tables
```

Per message you get `NAME.msg.csv` (1 × 8), `NAME.sig.csv` (nSig × 8),
`NAME.names.txt` (signal order and units), `NAME.json`, and one
`cantp_tables.h`. In LabVIEW read the two CSVs with **Read Delimited
Spreadsheet (DBL)**, `Reshape Array` the signal table to 1-D
(`nSig × 8` elements), and keep them in a cluster or a functional global.
`nSig` = number of rows. Or type the 8 numbers per row straight into a
2-D DBL constant on the diagram; they are exactly the `SG_` numbers from
the DBC.

## 4. CLFN settings

Common: calling convention **C**, return type **Signed 32-bit Integer**,
Run in any thread (one slot per loop; different slots may be used from
different loops).

### 4.1 CanTp_Define (once, at init)

| # | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | slot | Numeric | Signed 32-bit | Value |
| 2 | msgDef | Array | 8-byte Double, 1-D | Array Data Pointer |
| 3 | msgDefLen | Numeric | Signed 32-bit | Value (8) |
| 4 | sigDefs | Array | 8-byte Double, 1-D | Array Data Pointer |
| 5 | nSig | Numeric | Signed 32-bit | Value |

Check the return: negative means the table is wrong; −7 means the DBC asks
for a transport this release does not do (RTS/CTS, ISO-TP).

### 4.2 CanTp_OutputSize (once, after Define)

One I32 in (slot), I32 return = bytes per packed message. Use it to
`Initialize Array` the U8 output once.

### 4.3 CanTp_PackSgl / CanTp_Pack (every cycle)

| # | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | slot | Numeric | Signed 32-bit | Value |
| 2 | values | Array | 4-byte Single (or 8-byte Double for `CanTp_Pack`), 1-D | Array Data Pointer |
| 3 | nValues | Numeric | Signed 32-bit | Value |
| 4 | timestamp100ns | Numeric | Unsigned 64-bit | Value |
| 5 | spacing100ns | Numeric | Unsigned 64-bit | Value |
| 6 | out | Array | Unsigned 8-bit, 1-D | Array Data Pointer |
| 7 | outLen | Numeric | Signed 32-bit | Value |
| 8 | bytesWritten | Numeric | Signed 32-bit | Pointer to Value |

`Array Subset(out, 0, bytesWritten)` → **XNET Write (Frame Output Stream,
Raw)**. For a BAM the array holds every frame of the sequence; write one
24-byte record every 50 ms (a queue and a 50 ms loop), or enable the XNET
session's replay timing and give the records real timestamps.

### 4.4 CanTp_RxFeed (every received frame)

| # | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | slot | Numeric | Signed 32-bit | Value |
| 2 | frame | Array | Unsigned 8-bit, 1-D | Array Data Pointer |
| 3 | frameLen | Numeric | Signed 32-bit | Value |
| 4 | values | Array | 8-byte Double, 1-D (pre-sized nSig) | Array Data Pointer |
| 5 | nValues | Numeric | Signed 32-bit | Value |

**XNET Read (Frame Raw)** returns a U8 array of concatenated records. Walk
it: `CanTp_RecordSize(subarray, remaining)` gives the size of the record at
the current offset; feed that record to every slot you are listening for;
a return of 1 means `values` now holds a complete message for that slot.
For classic CAN and J1939 every record is 24 bytes, so a simple
`Decimate`/`Reshape` into 24-byte rows also works.

### 4.5 CanTp_Unpack (buffers, logs, tests)

Same as RxFeed but over a whole U8 array, plus an I32 `bytesConsumed`
pointer. Loop while it returns 1, advancing by `bytesConsumed`.

### 4.6 CanTp_NclHeader, CanTp_MakeRecord, CanTp_Version

`CanTp_NclHeader`: U8 array pre-sized to 12 + I32 12; write it once at the
start of a `.ncl` log, then append the records from Pack. `CanTp_MakeRecord`
builds a record from id/extended/type/payload for frames that arrive from
a non-XNET driver. `CanTp_Version` has no parameters, returns U32.

## 5. RT loop sketch

```
[Init]
    CanTp_Define(0, TEMPCTL.msg, 8, TEMPCTL.sig, nSig)      -> 0
    size = CanTp_OutputSize(0)
    CanTp_Define(1, EC1.msg, 8, EC1.sig, nSigEC1)           -> 0   (a second message)

[Each cycle]
    CanTp_PackSgl(0, controllerOut, nSig, 0, 0, buf, size, &n)
    enqueue buf[0..n) as 24-byte records for the 50 ms sender loop

[50 ms sender loop]
    dequeue one record -> XNET Write (Frame Output Stream, Raw)

[Receive loop]
    raw = XNET Read (Frame Raw)
    for each record in raw:
        if CanTp_RxFeed(1, record, 24, ec1Values, nSigEC1) == 1 -> use ec1Values
```

## 6. Timestamps

Raw-record timestamps count 100 ns since 1601-01-01 UTC. From a LabVIEW
absolute timestamp (seconds since 1904-01-01):
`timestamp100ns = (ts + 9561628800) × 1e7`. Pass 0 unless you log to `.ncl`
or use replay timing.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| LabVIEW error 7 on RT | .so not at the CLFN path or not executable: `chmod 755` |
| Error 13 / not a valid Win32 application | 32-bit LabVIEW with the x64 DLL; use `x86\cantp.dll` |
| −4 from Define | Length does not fit the transport (classic > 8, FD > 64), or an SA override on an 11-bit id |
| −5 from Define | A signal does not fit in the payload, or factor 0, or float type with a length other than 32/64. Check the start bit against the `.names.txt` order |
| −7 from Define | DBC message needs RTS/CTS (DA ≠ 255) or ISO-TP — release 2 |
| −6 from Pack | `out` too small: size it from `CanTp_OutputSize` |
| RxFeed never returns 1 | SA mismatch (an override was given but the sender uses another SA), a missing TP.DT packet (the sequence is abandoned until the next TP.CM), or frames fed as something other than one whole record |
| Values decode as the maximum | Sender packed NaN ("not available", all ones) |
| Receiver on the bus shows no PG | BAM packets sent back-to-back or > 200 ms apart; pace them 50..200 ms |
