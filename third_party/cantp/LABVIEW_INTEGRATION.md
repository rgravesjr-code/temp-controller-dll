# CanTp — LabVIEW and cRIO integration guide

The "what goes where and what do I type in the CLFN" companion to
`CANTP_PACKAGE_GUIDE.md`.

## 1. Files

| Target | File | Where it goes |
|---|---|---|
| cRIO-904x/905x/906x (x86_64 NI Linux RT) | `linux-x64\libcantp.so` | `/usr/local/lib/libcantp.so` |
| Raspberry Pi 4/5 64-bit (test bench) | `linux-arm64\libcantp.so` | `/usr/local/lib/libcantp.so` |
| 32-bit ARM Linux (LabVIEW LINX / Hobbyist toolkit chroot on a Raspberry Pi) | `linux-armhf\libcantp.so` | inside the chroot, e.g. `/usr/local/lib/libcantp.so` |
| self-test binaries | `linux-x64\test_cantp`, `linux-arm64\test_cantp`, `linux-armhf\test_cantp` | anywhere, run once |
| Windows 64-bit LabVIEW | `cantp.dll` | next to the VI/EXE or on PATH |
| Windows 32-bit LabVIEW | `x86\cantp.dll` | same |
| C/C++ | `cantp.h`, `cantp.lib`, `x86\cantp.lib` | your project |
| host tools | `tools\dbc2tables.py`, `tools\ecdflat.py`, `tools\pi_bus_loop.py` | PC with Python (+ cantools for the DBC) / Pi with python-can |

## 2. Deploy

```bat
scp linux-x64\libcantp.so admin@<crio>:/usr/local/lib/
scp linux-x64\test_cantp  admin@<crio>:/home/admin/
ssh admin@<crio> "chmod 755 /usr/local/lib/libcantp.so /home/admin/test_cantp && /home/admin/test_cantp"
```

Expected: `440 passed, 0 failed`. Same for the Pi with the `linux-arm64`
files (user `pi`). One `.so` does not run on both: the cRIO is x86_64, the
Pi is aarch64. Both are built from the same source and the test binary
proves each.

Desktop LabVIEW for Linux (Community Edition included) is x86_64 only; a
Raspberry Pi is reached through the LINX / Hobbyist toolkit, whose runtime
lives in a **32-bit** ARM chroot on the Pi. That is what `linux-armhf\` is
for: deploy that `.so` inside the chroot and point the CLFN at it.

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


### 3b. Or skip the tables: wire the ECD message cluster (v1.2.0)

If the message already exists as a `J1939Msg(V4).ctl` cluster (your CAN
database editor, or one element of the array an `.ecd` file unflattens
to), wire it through **Flatten To String** (defaults: big-endian, prepend
array/string sizes) → **String To Byte Array** → `CanTp_DefineFlat`. Signal
order is the channel order of the cluster; `CanTp_SignalCount` tells `nSig`,
`CanTp_Defaults` gives the channel defaults as a DBL array. Transport and
source address are derived from the id and byte count (see the package
guide); pass an explicit transport or SA when you need to override them.

| # | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | slot | Numeric | Signed 32-bit | Value |
| 2 | flat | Array | Unsigned 8-bit, 1-D | Array Data Pointer |
| 3 | flatLen | Numeric | Signed 32-bit | Value |
| 4 | transport | Numeric | Signed 32-bit (−1 = derive) | Value |
| 5 | sa | Numeric | Signed 32-bit (−1 = from the id) | Value |

Return −14 means the bytes are not that cluster (wrong control, or Flatten
To String set to little-endian / no size prefixes); −5 usually means a
Motorola channel whose start bit does not follow the DBC numbering (see
DBC-CONVENTIONS.md §8).

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


### 4.3b CanTp_Transfer (one CLFN for both directions, with frame lengths)

| # | Name | Type | Data type | Pass |
|---|---|---|---|---|
| 1 | slot | Numeric | Signed 32-bit | Value |
| 2 | mode | Numeric | Signed 32-bit: 0 write (pack), 1 read (unpack) | Value |
| 3 | values | Array | 8-byte Double, 1-D, nSig elements (`CanTp_TransferSgl`: 4-byte Single) | Array Data Pointer |
| 4 | nValues | Numeric | Signed 32-bit | Value |
| 5 | frames | Array | Unsigned 8-bit, 1-D, pre-sized to `CanTp_OutputSize` | Array Data Pointer |
| 6 | framesLen | Numeric | Signed 32-bit | Value |
| 7 | frameLens | Array | Unsigned 8-bit, 1-D, pre-sized to `CanTp_FrameCount` | Array Data Pointer |
| 8 | frameLensLen | Numeric | Signed 32-bit (0 = no length array) | Value |
| 9 | timestamp100ns | Numeric | Unsigned 64-bit | Value |
| 10 | spacing100ns | Numeric | Unsigned 64-bit | Value |
| 11 | bytesUsed | Numeric | Signed 32-bit | Pointer to Value |
| 12 | nFrames | Numeric | Signed 32-bit | Pointer to Value |

Write: `Array Subset(frames, 0, bytesUsed)` is the sequence (TP.CM first),
`Array Subset(frameLens, 0, nFrames)` the DLC of each record. Read: wire
the records from XNET Read (Frame Raw) as `frames`, either an empty
`frameLens` (0) or the lengths you know; 1 back means `values` holds the
message, `bytesUsed` / `nFrames` say how far the walk went. The same VI with
the mode wired to a boolean serves both ends.

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
    CanTp_Define(0, TEMPCTL.msg, 8, TEMPCTL.sig, nSig)      -> 0     (tables)
    CanTp_DefineFlat(3, flatBytes, len, -1, -1)              -> 0     (or: the ECD cluster, flattened)
    size = CanTp_OutputSize(0); nFrames = CanTp_FrameCount(0)
    CanTp_Define(1, EC1.msg, 8, EC1.sig, nSigEC1)           -> 0   (a second message)

[Each cycle]
    CanTp_PackSgl(0, controllerOut, nSig, 0, 0, buf, size, &n)
      or CanTp_Transfer(0, 0, values, nSig, buf, size, lens, nFrames, 0, 0, &n, &nf)
    enqueue buf[0..n) as 24-byte records (lens[i] bytes of data each) for the 50 ms sender loop

[50 ms sender loop]
    dequeue one record -> XNET Write (Frame Output Stream, Raw)

[Receive loop]
    raw = XNET Read (Frame Raw)
    for each record in raw:
        if CanTp_RxFeed(1, record, 24, ec1Values, nSigEC1) == 1 -> use ec1Values
```

### 5.1 Session transports (J1939 RTS/CTS, ISO-TP)

The same receive loop drives a session; add `Tick Count (ms)` as `nowMs`
and send whatever the calls write into `out`:

```
[Init]
    CanTp_Define(2, DIAG.msg, 8, DIAG.sig, nSig)              (transport 4 or 5 in the msg row)
    CanTp_SessionConfig(2, {-1, -1, 4, 0, 0, 0}, 6)           (window 4 as receiver; defaults otherwise)
    size = CanTp_OutputSize(2)

[Send a message]
    rc = CanTp_TxStart(2, values, nSig, Tick Count, 0, 0, buf, size, &n)
    XNET Write buf[0..n)                                       (the RTS / FirstFrame; rc 2 = single frame, done)

[Receive loop, every iteration]
    raw = XNET Read (Frame Raw)
    for each record in raw (and once more with frame = NULL for the timers):
        rc = CanTp_TxFeed(2, record, 24, Tick Count, 0, spacing, buf, size, &n)
        XNET Write buf[0..n)                                   (DT / ConsecutiveFrames when a CTS / FC came in)
        rc == 2 -> message delivered; rc < 0 -> aborted / timed out (buf may hold the abort frame)
        rc = CanTp_RxStep(3, record, 24, Tick Count, 0, values, nSig, buf, size, &n)
        XNET Write buf[0..n)                                   (CTS / EndOfMsgAck / flow control)
        rc == 1 -> values holds a received message
```

`CanTp_TxFeed` and `CanTp_RxStep` CLFN parameters: slot I32; frame U8 1-D
Array Data Pointer (wire an empty array for the timer call and 0 as
frameLen); frameLen I32; nowMs U32; timestamp100ns U64; (TxFeed:
spacing100ns U64); (RxStep: values DBL 1-D Array Data Pointer, nValues I32);
out U8 1-D Array Data Pointer pre-sized to `CanTp_OutputSize`; outLen I32;
bytesWritten I32 Pointer to Value. `CanTp_TxStart` is `CanTp_Pack` plus the
`nowMs` U32 after `nValues`. `CanTp_SessionConfig`: slot I32, cfg DBL 1-D
Array Data Pointer (6 elements), cfgLen I32. `CanTp_DefineMux`: slot I32,
muxDefs DBL 1-D Array Data Pointer (nSig × 2), nSig I32.

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
| −14 from DefineFlat | Not a flattened `J1939Msg(V4)` cluster: check Flatten To String is big-endian with size prefixes, and that the whole string reached the DLL |
| −5 from DefineFlat | Usually a Motorola channel: the library reads the start bit as the DBC sawtooth MSB position; an ECD entry using another convention does not fit (DBC-CONVENTIONS.md §8) |
| −6 from Pack | `out` too small: size it from `CanTp_OutputSize` |
| RxFeed never returns 1 | SA mismatch (an override was given but the sender uses another SA), a missing TP.DT packet (the sequence is abandoned until the next TP.CM), or frames fed as something other than one whole record |
| Values decode as the maximum | Sender packed NaN ("not available", all ones) |
| Receiver on the bus shows no PG | BAM packets sent back-to-back or > 200 ms apart; pace them 50..200 ms |
