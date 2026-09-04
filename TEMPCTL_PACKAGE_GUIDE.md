# TempCtl v2 — Package Guide (API reference)

`tempctl` is a **pure C library with flat `extern "C"` exports** for LabVIEW's
Call Library Function Node (CLFN) and any other caller that can load a native
library: C/C++, Python (ctypes), .NET P/Invoke, MATLAB. It ships as
`tempctl.dll` (Windows x64 and x86), `libtempctl.so` (Linux x86_64 for the
cRIO-904x/905x/906x, aarch64 for the Raspberry Pi). One header, one ABI,
every build.

**Calling convention:** C (cdecl) on all exports.
**Numeric types:** `int32_t`/`uint32_t`/`float` exactly as declared; no
structs, no strings, no callbacks, no allocation.
**Arrays:** caller-allocated, passed as a pointer plus an `int32_t` length.
**Return value:** `int32_t`; 0 = ok, negative = error, positive = warning
(the call did its work but something needs attention).
**Threading:** no locks. Different zones may be used from different threads;
one zone from one thread at a time.
**Dependencies:** none. The DLL links the CRT statically (imports only
`KERNEL32.dll`); the .so imports only `memcpy`/`memset` from libc.

The CAN side is **CanTp** (`third_party\cantp\CANTP_PACKAGE_GUIDE.md`); §5
below shows how the two fit together.

## 1. Exports

```c
uint32_t TcVersion(void);       // (major << 16) | (minor << 8) | patch; 0x020000 = 2.0.0
int32_t  TcInputCount(void);    // 17  = TC_INPUT_COUNT
int32_t  TcSignalCount(void);   // 27  = TC_SIGNAL_COUNT
int32_t  TcStep(int32_t zone, int32_t action, uint32_t nowMs,
                const float* in, int32_t inLen, float* out, int32_t outLen);
```

## 2. Return codes

| Code | Value | Meaning |
|---|---|---|
| `TC_OK` | 0 | Success |
| `TC_WARN_CONFIG` | 1 | Configuration inconsistent (see §4.6); the call still ran, and `ErrorStatus` bit 9 is set |
| `TC_ERR_ARG` | −1 | Null pointer, `inLen < 17` or `outLen < 27` |
| `TC_ERR_ZONE` | −2 | Zone outside 0..15 |
| `TC_ERR_ACTION` | −3 | Action not 0/1/2 |
| `TC_ERR_NOT_INIT` | −4 | Step on a zone that has not been initialised |

## 3. `TcStep`

| # | Parameter | CLFN type | Pass | Notes |
|---|---|---|---|---|
| ret | return | Numeric I32 | | 0, 1 (config warning), or negative |
| 1 | zone | Numeric I32 | Value | 0..15; each zone is an independent controller |
| 2 | action | Numeric I32 | Value | 0 Init, 1 Step, 2 Reset |
| 3 | nowMs | Numeric U32 | Value | `Tick Count (ms)`. Only differences matter; wrap at 2³² is handled |
| 4 | in | Array, SGL, 1-D | Array Data Pointer | ≥ 17 elements (order below) |
| 5 | inLen | Numeric I32 | Value | 17 (or 27 when the same array is used for both) |
| 6 | out | Array, SGL, 1-D | Array Data Pointer | pre-sized to 27 elements; may be the same wire as `in` |
| 7 | outLen | Numeric I32 | Value | 27 |

### 3.1 The signal array

Indices 0..16 are read from `in` on every call and echoed to `out` (15 and 16
are replaced by the relay commands). Indices 17..26 are outputs only. The
order is also the order of the CAN message (`dbc\tempctl.dbc`).

| Index | Name | Unit | Input | Output |
|---|---|---|---|---|
| 0 | Setpoint | ° | heating ends at ≥, cooling ends at ≤ | echoed |
| 1 | DeadbandHi | ° offset ≥ 0 | `HiBand = Setpoint + DeadbandHi` | echoed |
| 2 | DeadbandLo | ° offset ≥ 0 | `LoBand = Setpoint − DeadbandLo` | echoed |
| 3 | HiLimit | ° | a sensor above this for ErrorTimeout has failed (hi) | echoed |
| 4 | LoLimit | ° | a sensor below this for ErrorTimeout has failed (lo) | echoed |
| 5 | ErrorTimeout | ms | limit / bad reading / disagreement / feedback countdown | echoed |
| 6 | DeadbandTimeout | ms | time outside the band before a relay engages | echoed |
| 7 | FilterPoints | 1..64 | moving-average length per sensor (0 → 4) | echoed |
| 8 | Temp2Enable | 0/1 | second sensor present | echoed |
| 9 | Temp2Tolerance | ° | `|Temp1f − Temp2f|` above this for ErrorTimeout → disagree | echoed |
| 10 | FeedbackEnable | 0/1 | compare relay feedback with the commands | echoed |
| 11 | Temp1 | ° | sensor 1 (primary), NaN/Inf = bad reading | echoed |
| 12 | Temp2 | ° | sensor 2 (ignored unless Temp2Enable) | echoed |
| 13 | HeaterFeedback | 0/1 | measured heater relay state | echoed |
| 14 | CoolerFeedback | 0/1 | measured cooler relay state | echoed |
| 15 | HeatingCmd | 0/1 | Init only: initial relay state | heating relay command |
| 16 | CoolingCmd | 0/1 | Init only: initial relay state | cooling relay command |
| 17 | ErrorStatus | bit mask | – | see §3.2 |
| 18 | TempStatus | enum | – | see §3.3 |
| 19 | ControlTemp | ° | – | filtered value of the active sensor (drives control) |
| 20 | Temp1Filtered | ° | – | moving average of Temp1; NaN until a valid sample exists |
| 21 | Temp2Filtered | ° | – | moving average of Temp2; NaN when disabled / no sample |
| 22 | HiBand | ° | – | Setpoint + DeadbandHi |
| 23 | LoBand | ° | – | Setpoint − DeadbandLo |
| 24 | ErrorRemainMs | ms | – | smallest running error countdown, 0 when none |
| 25 | DbRemainMs | ms | – | remaining deadband countdown, 0 when idle or running |
| 26 | ActiveSensor | 1/2 | – | the sensor whose filtered value is ControlTemp |

### 3.2 ErrorStatus bits

| Bit | Value | Name | Set when | Cleared by |
|---|---|---|---|---|
| 0 | 1 | T1_HI | Temp1Filtered > HiLimit for ErrorTimeout → sensor 1 failed | Reset / Init |
| 1 | 2 | T1_LO | Temp1Filtered < LoLimit for ErrorTimeout → sensor 1 failed | Reset / Init |
| 2 | 4 | T1_BAD | Temp1 NaN/Inf (or no filtered value yet) for ErrorTimeout → sensor 1 failed | Reset / Init |
| 3 | 8 | T2_HI | as bit 0 for sensor 2 (only with Temp2Enable) | Reset / Init |
| 4 | 16 | T2_LO | as bit 1 for sensor 2 | Reset / Init |
| 5 | 32 | T2_BAD | as bit 2 for sensor 2 | Reset / Init |
| 6 | 64 | DISAGREE | `|Temp1f − Temp2f| > Temp2Tolerance` for ErrorTimeout | Reset / Init |
| 7 | 128 | HEATER_FB | HeaterFeedback ≠ previous HeatingCmd for ErrorTimeout | Reset / Init |
| 8 | 256 | COOLER_FB | CoolerFeedback ≠ previous CoolingCmd for ErrorTimeout | Reset / Init |
| 9 | 512 | CONFIG | configuration inconsistent on this call (§4.6) | fixing the config |

Bits 0..2 keep the v1 meaning (1 hi limit, 2 lo limit, bad reading) for a
single-sensor system; only "bad" moved from value 3 to bit 2 (value 4).

### 3.3 TempStatus values

| Value | Name | Meaning |
|---|---|---|
| 0 | InBand | idle, ControlTemp inside [LoBand, HiBand] |
| 1 | HeatPending | below LoBand, deadband countdown running |
| 2 | Heating | heating relay on, until ControlTemp ≥ Setpoint |
| 3 | CoolPending | above HiBand, deadband countdown running |
| 4 | Cooling | cooling relay on, until ControlTemp ≤ Setpoint |
| 5 | ErrorPending | an error countdown is running (`ErrorRemainMs > 0`) |
| 6 | Stopped | fault: relays off, latched until Reset |
| 7 | Degraded | Temp2Enable and exactly one sensor failed; running on the other |
| 8 | FilterWarmup | fewer than FilterPoints samples in the active sensor's filter |

When several apply the first of Stopped, ErrorPending, FilterWarmup, Degraded
wins, then the control state. The relay commands (15, 16) are always the
truth about the relays, whatever the status says.

### 3.4 Actions

- **Init (0)** — clears everything for the zone: faults, latched bits,
  timers, filters. Takes the initial relay state from `in[15]`/`in[16]`
  (heating wins if both are set); sensor 1 becomes active; the current
  readings enter the filters. Use once at start-up and whenever the process
  is restarted.
- **Step (1)** — one control tick. Requires a previous Init.
- **Reset (2)** — the operator's "acknowledge": clears the stopped state, all
  latched bits, the timers; relays off; sensor 1 active again. Keeps the
  filters and the configuration. If a fault condition is still present it
  is timed again from the first Step that sees it.

## 4. Behaviour of Step

Evaluated in this order on every Step; time comes from `nowMs` differences.

### 4.1 Filtering

Each sensor's valid samples (not NaN/Inf) enter a moving average of
FilterPoints (1..64). The filtered value is NaN until the first valid sample.
A change of FilterPoints applies immediately (the average uses the last N
stored samples). `FilterWarmup` is reported until N samples exist. Filters
keep running while Stopped.

### 4.2 Sensor rationality and failure

Per sensor (Temp2 only when Temp2Enable): the condition is *bad* when the raw
reading is NaN/Inf or no filtered value exists, else *hi* when the filtered
value is above HiLimit, else *lo* when below LoLimit. A condition held for
ErrorTimeout ms **fails the sensor**: its bit is set and stays set until
Reset. Countdowns start at the first Step that observes the condition (that
tick's dt is not charged) and restart from full whenever the condition
changes. A sensor that returns inside its limits does *not* un-fail on its
own (Q3: latched; avoids flapping).

### 4.3 Failover and stop

- Temp2Enable = 0: sensor 1 failed → **Stopped** (relays off, latched).
- Temp2Enable = 1: the active sensor failed and the other has not →
  ActiveSensor switches, status **Degraded**, control continues on the other
  sensor's filtered value. The backup failing while the primary is fine also
  reports Degraded (backup lost) without switching. Both failed → Stopped.
- Disabling Temp2 while running on sensor 2 returns control to sensor 1
  (which, being failed, stops the controller).

### 4.4 Disagreement

With Temp2Enable, both filtered values valid and neither sensor failed:
`|Temp1f − Temp2f| > Temp2Tolerance` for ErrorTimeout sets DISAGREE
(latched). The controller keeps running on the active sensor — two readings
cannot tell which one is wrong. If one sensor later fails its limits, §4.3
applies.

### 4.5 Control

A raw NaN/Inf on the **active** sensor drops both relays at once (and clears
the deadband countdown) while its error countdown runs — as v1. Otherwise on
ControlTemp: heating stays on until ≥ Setpoint, cooling until ≤ Setpoint.
When idle, above HiBand for DeadbandTimeout → cooling on; below LoBand for
DeadbandTimeout → heating on. Heating and cooling are mutually exclusive.
Zero timeouts act on first observation.

### 4.6 Feedback

With FeedbackEnable, HeaterFeedback / CoolerFeedback (> 0.5 = on) are
compared with the commands issued by the **previous** call (the Init state
counts as the first command). A mismatch held for ErrorTimeout sets
HEATER_FB / COOLER_FB; **operation continues** (D4); the bit is latched (Q2).

### 4.7 Configuration check

`TC_WARN_CONFIG` (and bit 9) when any of: DeadbandHi/Lo < 0, `LoLimit <
LoBand` false, `HiBand < HiLimit` false, ErrorTimeout or DeadbandTimeout < 0,
Temp2Tolerance < 0, FilterPoints negative or > 64 (clamped to 64), or a NaN
in any of these. The controller runs anyway; a setpoint band outside the
limits simply cannot be reached without a fault.

## 5. Sending the state on CAN (CanTp)

```c
/* once: the message definition from dbc\tables (or your own DBC through dbc2tables.py) */
CanTp_Define(0, TempCtl_msgdef, 8, &TempCtl_sigdefs[0][0], TempCtl_NSIG);   /* cantp_tables.h */
int32_t need = CanTp_OutputSize(0);                                           /* 9 frames x 24 = 216 bytes */

/* every tick */
TcStep(0, TC_ACTION_STEP, tick, in, 17, out, 27);
CanTp_PackSgl(0, out, 27, timestamp100ns, spacing100ns, frames, need, &written);
/* -> XNET Write (Frame Output Stream, raw), or append to a .ncl after CanTp_NclHeader() */
```

The msgdef's source address column (index 4) replaces the DBC's 0xFE
placeholder with the node's real SA (e.g. 0x80). On the receiving side
`CanTp_Unpack` (buffer) or `CanTp_RxFeed` (one record per call) return the
27 values in the same order; NaN outputs (Temp2Filtered while disabled) are
packed as the J1939 "not available" pattern and decode as the signal's
maximum raw value.

In LabVIEW: `Read Delimited Spreadsheet` on `dbc\tables\TempCtl.msg.csv` (1×8)
and `TempCtl.sig.csv` (27×8) → `Reshape Array` to 1-D DBL → `CanTp_Define`.
`TempCtl.names.txt` lists the signal order.

## 6. Timing

The library differences successive `nowMs` values (`Tick Count (ms)`), so a
loop period of 10 ms or 1 s gives the same countdown behaviour. Countdown
resolution is one loop period. Calling Step less often than ErrorTimeout
makes faults latch on the second observation.
