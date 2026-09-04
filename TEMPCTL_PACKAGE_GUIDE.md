# tempctl — Package Guide (API reference)

`tempctl` is a **pure C library with flat `extern "C"` exports** for LabVIEW's
Call Library Function Node (CLFN) and any other caller that can load a native
library: C/C++, Python (ctypes), .NET P/Invoke, MATLAB. It ships as
`tempctl.dll` (Windows x64 and x86) and `libtempctl.so` (NI Linux RT x86_64,
cRIO-904x/905x/906x). One header, one ABI, every build.

**Calling convention:** C (cdecl) on all exports.
**Numeric types:** `int32_t`/`uint32_t`/`uint64_t`/`uint8_t`/`float`/`double`
exactly as declared; no structs, no strings, no callbacks, no allocation.
**Arrays:** caller-allocated, passed as a pointer plus an `int32_t` length.
**Return value:** `int32_t`; 0 = ok, negative = error, positive = warning
(the call did its work but something needs attention).
**Threading:** no locks. Different zones may be used from different threads.
One zone must be used from one thread at a time. The frame encoders keep no
state and are fully re-entrant.
**Dependencies:** none. The DLL links the CRT statically (imports only
`KERNEL32.dll`); the .so imports only `memcpy`/`memset` from libc.

## Return codes

| Code | Value | Meaning |
|---|---|---|
| `TC_OK` | 0 | Success |
| `TC_WARN_CONFIG` | 1 | Init/Step: config not in the order `LoLimit < LoDeadband ≤ Setpoint ≤ HiDeadband < HiLimit`, or a negative timeout. The call still ran. |
| `TC_ERR_ARG` | −1 | Null pointer or array shorter than required |
| `TC_ERR_ZONE` | −2 | Zone outside 0..15 |
| `TC_ERR_ACTION` | −3 | Action not 0/1/2 |
| `TC_ERR_NOT_INIT` | −4 | Step on a zone that has not been initialised |
| `TC_ERR_BUFFER` | −5 | Output U8 array too small; `bytesWritten` holds the size needed |
| `TC_ERR_PAYLOAD` | −6 | Payload longer than 1785 bytes (J1939 TP limit) |
| `TC_ERR_SIGDEF` | −7 | Generic packer: bad signal row (length, start bit, factor 0, does not fit in DLC, float type with wrong length) |
| `TC_ERR_FRAMEDEF` | −8 | Generic packer: bad frame row (DLC > 8, ID out of range, > 256 frames) |

## `TcVersion`

```c
uint32_t TcVersion(void);        // (major << 16) | (minor << 8) | patch, 0x010000 = 1.0.0
```

CLFN: return type Numeric U32, no parameters.

## `TcStep` — temperature controller

```c
int32_t TcStep(int32_t zone, int32_t action, uint32_t nowMs,
               const float* in, int32_t inLen, float* out, int32_t outLen);
```

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0, 1 (config warning), or negative |
| 1 | zone | Numeric I32 | Value | 0..15; each zone is an independent controller |
| 2 | action | Numeric I32 | Value | 0 Init, 1 Step, 2 Reset |
| 3 | nowMs | Numeric U32 | Value | `Tick Count (ms)`. Only differences matter; wrap at 2³² is handled |
| 4 | in | Array, SGL, 1-D | Array Data Pointer | 11 elements (order below) |
| 5 | inLen | Numeric I32 | Value | 11 |
| 6 | out | Array, SGL, 1-D | Array Data Pointer | pre-sized to 11 or 13 elements; may be the same wire as `in` |
| 7 | outLen | Numeric I32 | Value | 11 or 13 |

### Signal array

| Index | Name | Unit | On input | On output |
|---|---|---|---|---|
| 0 | HiLimit | ° | above this for ErrorTimeout → fault 1 | echoed |
| 1 | LoLimit | ° | below this for ErrorTimeout → fault 2 | echoed |
| 2 | HiDeadband | ° (absolute) | above this for DeadbandTimeout → cooling on | echoed |
| 3 | LoDeadband | ° (absolute) | below this for DeadbandTimeout → heating on | echoed |
| 4 | Setpoint | ° | heating ends at ≥, cooling ends at ≤ | echoed |
| 5 | ActualTemp | ° | measured (conditioned thermocouple) | echoed |
| 6 | ErrorTimeout | ms | limit → fault delay | echoed |
| 7 | DeadbandTimeout | ms | deadband → relay delay | echoed |
| 8 | CoolingActive | 0/1 | Init only: initial relay state | cooling relay command |
| 9 | HeatingActive | 0/1 | Init only: initial relay state | heating relay command |
| 10 | ErrorStatus | enum | ignored | 0 none, 1 HiLimit, 2 LoLimit, 3 bad reading |
| 11 | ErrorRemainMs | ms | – | optional (outLen ≥ 13): remaining error countdown, 0 when not counting |
| 12 | DbRemainMs | ms | – | optional: remaining deadband countdown, 0 when idle or running |

### Actions

**Init (0)** clears the fault and both countdowns, records `nowMs`, takes the
initial relay state from elements 8/9 (heating wins if both are 1), and
validates the config (warning 1 if out of order). Output is written.

**Step (1)** runs one tick, see below. Requires a prior Init on that zone.

**Reset (2)** clears the fault and countdowns, forces both relays off, records
`nowMs`. Config is untouched (it is read from `in` on every call anyway).
Also acts as an Init with relays off on a never-initialised zone.

### Step semantics

`dt` = `nowMs − previous nowMs` (unsigned, wrap-safe). Then, in order:

1. **Latched fault?** If ErrorStatus ≠ 0: both relays stay off, nothing else
   happens until Reset or Init.
2. **Error condition** = 3 if ActualTemp is NaN or ±Inf, else 1 if above
   HiLimit, else 2 if below LoLimit, else 0. When the condition **changes**,
   the countdown is reloaded with ErrorTimeout and this tick's `dt` is not
   charged (the countdown measures time since first observation). While the
   condition is non-zero the countdown decrements by `dt`; at ≤ 0 the fault
   latches with ErrorStatus = condition, both relays off, done.
   A running relay keeps running during the countdown (a failing heater with
   the temperature falling through LoLimit is the realistic case).
3. **Bad reading** (condition 3) and not yet faulted: both relays off now,
   deadband countdown cleared, done. A single glitch therefore does not latch
   a fault, but it does interrupt a heat/cool cycle.
4. **Cycle end:** heating turns off when ActualTemp ≥ Setpoint; cooling turns
   off when ActualTemp ≤ Setpoint.
5. **Running:** if a relay is on, the deadband countdown is idle, done.
6. **Idle:** deadband condition = 1 if above HiDeadband, 2 if below
   LoDeadband, else 0. Same reload-on-change rule as the error countdown.
   At ≤ 0: condition 1 → cooling on, 2 → heating on.

A zero timeout acts on the first observation. With a 100 ms loop and a 500 ms
timeout the relay changes on the sixth observation of the condition.

**Config rule:** `LoLimit < LoDeadband ≤ Setpoint ≤ HiDeadband < HiLimit`.
A setpoint outside the deadband makes the relay chatter at the deadband
timeout rate (it turns on, immediately satisfies the setpoint, turns off).
Treat return code 1 as an error in the RT code and move the deadbands
together with the setpoint.

## NI-XNET raw frame format (all encoders)

Every frame produced is one 24-byte record, little-endian, identical to the
event record of an NI-XNET logfile (`.ncl`) and to what
**XNET Write (Frame Output Stream).vi** / **XNET Read (Frame Raw)** use:

| Offset | Size | Field | Value written |
|---|---|---|---|
| 0 | 8 | Timestamp | U64, 100 ns units since 1601-01-01 00:00 UTC; 0 when none |
| 8 | 4 | Identifier | CAN ID; bit 29 (`0x20000000`) set for 29-bit extended IDs |
| 12 | 1 | Type | `0x00` CAN data frame |
| 13 | 1 | Flags | 0 |
| 14 | 1 | Info | 0 |
| 15 | 1 | PayloadLength | 0..8 |
| 16 | 8 | Payload | data, zero padded to 8 |

Records are concatenated into the U8 output array. To write a `.ncl` file:
12-byte header from `TcNclHeader`, then the records.

```c
int32_t TcNclHeader(uint8_t* out, int32_t outLen);   // out: U8 array of 12, outLen 12
```

Header bytes: `4E 49 00 03 01 01 02 00 01 00 00 00` ("NI", header size 3×4,
header v1.1, event v2.0, little-endian events).

## `TcJ1939Bam`, `TcEncodeFrames`, `TcJ1939BamFrameCount` — J1939 transport

```c
int32_t TcJ1939BamFrameCount(int32_t payloadLen);   // frames needed, 0 if len invalid
int32_t TcJ1939Bam(uint32_t pgn, uint8_t sa, uint8_t priority,
                   const uint8_t* payload, int32_t payloadLen,
                   uint64_t timestamp100ns, uint64_t spacing100ns,
                   uint8_t* out, int32_t outLen, int32_t* bytesWritten);
int32_t TcEncodeFrames(const float* signals, int32_t n,
                       uint32_t pgn, uint8_t sa, uint8_t priority,
                       uint64_t timestamp100ns, uint64_t spacing100ns,
                       uint8_t* out, int32_t outLen, int32_t* bytesWritten);
```

| Parameter | CLFN type | Pass | Notes |
|---|---|---|---|
| signals / payload | Array SGL / U8, 1-D | Array Data Pointer | SGL values go on the wire as raw IEEE-754 single, little-endian, 4 bytes each |
| n / payloadLen | Numeric I32 | Value | element count / byte count (≤ 1785 bytes) |
| pgn | Numeric U32 | Value | 18-bit PGN incl. DP/EDP bits. Default for the controller: 65280 (`0xFF00`, Proprietary B) |
| sa | Numeric U8 | Value | source address; default `0x80` |
| priority | Numeric U8 | Value | 0..7 for the single-frame case; TP frames always use 7 (J1939-21). Default 6 |
| timestamp100ns | Numeric U64 | Value | timestamp of the first frame; 0 = none |
| spacing100ns | Numeric U64 | Value | added per following frame; 500 000 = 50 ms |
| out | Array U8, 1-D | Array Data Pointer | pre-sized to `TcJ1939BamFrameCount(len) × 24` |
| outLen | Numeric I32 | Value | |
| bytesWritten | Numeric I32 | Pointer to Value | bytes actually written, or bytes needed on `TC_ERR_BUFFER` |

Framing:

- `payloadLen ≤ 8`: one frame under the PGN itself, priority as given.
  PDU1 PGNs (PF < 240) get destination address 0xFF (global).
- `9 ≤ payloadLen ≤ 1785`: **TP.CM BAM** (PGN 60416, ID `1CECFFss`) then
  `ceil(len/7)` **TP.DT** packets (PGN 60160, ID `1CEBFFss`), sequence 1..N,
  7 data bytes each, unused bytes of the last packet 0xFF.

The 11 controller signals (44 bytes) become 8 frames, 192 bytes:

```
1CECFF80  20 2C 00 07 FF 00 FF 00   BAM: 44 bytes, 7 packets, PGN 0x00FF00
1CEBFF80  01 b0 b1 b2 b3 b4 b5 b6   seq 1, bytes 0..6   (HiLimit, LoLimit[0..2])
1CEBFF80  02 b7 ... b13             seq 2
   ...
1CEBFF80  07 b42 b43 FF FF FF FF FF seq 7, ErrorStatus[2..3] + padding
```

Byte `4*i .. 4*i+3` of the payload is signal *i* as float32 LE. A receiver
with a DBC can describe the message as 11 float signals of 32 bits, Intel
byte order, start bits 0, 32, 64, … inside a 44-byte multiplexed PGN.

Timing: J1939-21 asks for 50–200 ms between BAM packets. The library only
stamps the frames (`timestamp + i × spacing`); the caller paces the actual
transmissions. XNET Frame Output Stream ignores timestamps unless replay
mode is on, so write one frame per 50 ms slot, or enable replay timing.

## `TcCanPack` — generic DBC-style packer

```c
int32_t TcCanPack(const double* sigDefs, int32_t nSig,
                  const double* frameDefs, int32_t nFrames,
                  const float* values, int32_t nValues,
                  uint64_t timestamp100ns,
                  uint8_t* out, int32_t outLen, int32_t* bytesWritten);
```

| Parameter | CLFN type | Pass | Notes |
|---|---|---|---|
| sigDefs | Array DBL, 1-D | Array Data Pointer | `nSig × 9` numbers, row-major (reshape a 2-D `nSig × 9` array) |
| nSig | Numeric I32 | Value | |
| frameDefs | Array DBL, 1-D | Array Data Pointer | `nFrames × 4` numbers |
| nFrames | Numeric I32 | Value | ≤ 256 |
| values | Array SGL, 1-D | Array Data Pointer | one physical value per signal row |
| nValues | Numeric I32 | Value | ≥ nSig |
| timestamp100ns | Numeric U64 | Value | stamped on every frame |
| out | Array U8, 1-D | Array Data Pointer | pre-sized to `nFrames × 24` |
| outLen | Numeric I32 | Value | |
| bytesWritten | Numeric I32 | Pointer to Value | |

Signal row (the "database"), all columns DBL:

| Col | Field | Meaning |
|---|---|---|
| 0 | frameIndex | row in frameDefs |
| 1 | startBit | **DBC convention.** Intel: bit position of the LSB. Motorola: bit position of the MSB in sawtooth numbering. Both exactly the number that appears after `SG_ name :` in a `.dbc` |
| 2 | bitLength | 1..64 |
| 3 | byteOrder | 0 = Intel / little-endian (`@1` in DBC), 1 = Motorola / big-endian (`@0`) |
| 4 | valueType | 0 unsigned, 1 signed two's complement, 2 IEEE float32 (length 32), 3 IEEE float64 (length 64) |
| 5 | factor | `physical = raw × factor + offset`; must be non-zero |
| 6 | offset | |
| 7 | min | clamp applied to the physical value; ignored when max ≤ min |
| 8 | max | |

Frame row:

| Col | Field | Meaning |
|---|---|---|
| 0 | arbitrationId | 11-bit or 29-bit CAN ID (no XNET flag bit) |
| 1 | extended | 0 standard, 1 extended |
| 2 | dlc | 0..8 |
| 3 | cycleMs | reserved, pass 0 |

Raw value: `(phys − offset) / factor`, rounded half away from zero, then
saturated to the bit width (unsigned 0..2ⁿ−1, signed −2ⁿ⁻¹..2ⁿ⁻¹−1).
Float types write the IEEE bits of `(phys − offset) / factor`. Every frame is
emitted in row order, unused bits 0. Overlapping signals are not detected
(later rows OR into earlier ones). A signal that does not fit inside its
frame's DLC returns `TC_ERR_SIGDEF`.

The packing was verified bit-for-bit against `cantools` (the reference Python
DBC library) over 3000 random layouts: Intel and Motorola, 1–32 bit, signed,
unsigned and float, with scaling.

## Calling from other languages

Python:

```python
import ctypes as C
lib = C.CDLL(r"tempctl.dll")          # or "./libtempctl.so"
lib.TcStep.argtypes = [C.c_int32, C.c_int32, C.c_uint32, C.POINTER(C.c_float), C.c_int32, C.POINTER(C.c_float), C.c_int32]
sig = (C.c_float * 13)(100, 0, 60, 40, 50, 20, 1000, 500, 0, 0, 0, 0, 0)
out = (C.c_float * 13)()
lib.TcStep(0, 0, 0, sig, 11, out, 13)  # init
```

`tests/oracle_test.py` and `examples/make_sample_ncl.py` are complete ctypes
examples for every export. C/C++: include `tempctl.h`, link `tempctl.lib`
(x64 or x86) or `-ltempctl`. .NET: `[DllImport("tempctl", CallingConvention = CallingConvention.Cdecl)]`.
