# TempCtl — Changelog

---

## v4.0.0 - 2026-09-22

Major revision from the requirements-change handoff of 2026-09-22
(`TEMPCTL-SPEC-v4.0.0.md`, `TEMPCTL-CAPABILITY-v4.0.0.md`,
`TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`): an explicit Start / Stop
lifecycle and a live run permissive on top of v3.0.0 with Amendment A.
`TcVersion()` reports 0x040000, `TcSetupCount()` 18, `TcDiagCount()` 28.
**Not backwards compatible with v3** (see Migration below).

- Pre-release review corrections: repeated `TcStop` calls now age hourly
  sensor-event history before rebasing time; eight new regression checks
  cover Stop, stopped samples, clock wrap and no double-aging on Start.
  Packaging rejects every nonzero oracle exit and missing/invalid inspected
  assets. Target execution claims require the current version, successful
  summary/exit and SHA-256 hashes of both target binaries. Corrected the
  cRIO architecture guidance: the 906x family is ARM, not x86-64.

- **New exports (V4-1, V4-2).** `TcStart(zone, nowMs, runPermissive, ...)`
  starts control when the zone is enabled, not faulted and the permissive
  is true, otherwise refuses it as `IdleStartBlocked` (7) with warning
  `OperatingConditionNotMet` (8), `TC_OK` either way; Start while started is
  a no-op. `TcStop(zone, nowMs, &doHeater, &doCooler, ...)` returns 0, 0 at
  once without a fault, keeps the setup, sensor history and any fault, and
  keeps a permissive-trip cause (`IdleOperatingConditionTripped`).
- **Live permissive (V4-3 to V4-7).** `TcCheckTemp` gains `runPermissive`
  after the two feedback inputs. While running, the first false sample
  de-energizes both relays (`OperatingConditionPending`, 8) and starts the
  new setup value `OperatingConditionTimeout` (index 17); recovery before
  the timeout gives `IdleOperatingConditionTripped` (9), no fault, no
  restart; a sustained loss gives `OperatingConditionFault` (17). A false
  permissive on a stopped, blocked or tripped zone never counts down.
- **Init and Reset leave the zone stopped (V4-9).** A passing enabled Init
  returns `IdleStopped` (6) with both relays 0; the v3 relay keeping across
  a re-Init (R9.3) is removed. Reset clears faults and history and leaves
  `IdleStopped`; control resumes only after `TcStart`. Because neither
  returns DO values, the host sequence for an active zone is `TcStop` ->
  apply the zeros -> `TcInit` / `TcReset` -> `TcStart`.
- **While stopped** nothing is evaluated (no accumulation, averaging,
  comparison, control or feedback; warnings 1-5 cleared); the raw readings
  are mirrored into the diagnostics and the hourly rings age in wall time.
  Start clears the averages and `Initial_HC_Flag`, keeps sensor failures,
  accumulators and hourly counts, and re-bases the time reference so
  stopped time never feeds a countdown.
- **Fault precedence (V4-8):** config -> sensor -> disagreement -> heater
  feedback -> cooler feedback -> operating condition; an existing fault
  maturing on the tick the permissive is lost wins; a latched fault is
  never overwritten; Start and Stop on a faulted zone change nothing.
- **Diagnostics (V4-10):** `RunPermissive` (25; NaN until the first
  evaluation), `OperatingConditionRemainMs` (26), `ControllerStarted` (27);
  the status / warning / DO mirrors follow every stateful call.
- **CAN (V4-15):** the message is **44 bytes** (was 55): the eight
  millisecond diagnostics are U16 (0..64255, 0xFFFF not available,
  saturation on the wire only); PGN 65280 and id `0x18FF00FE` unchanged.
  `dbc	empctl.ecd` is generated from the same signal model as the DBC and
  the tables (channels in `TC_DIAG_*` order, `CanTp_DefineFlat` gives the
  same slot; no reorder table) and read back for comparison.
- **Targets (V4-11, V4-12):** new `linux-armhf` build for the myRIO-1900
  (32-bit ARM, EABI v5 hard float, Cortex-A9; the library imports nothing
  from libc); debug sections stripped from every Linux build; the same
  `tempctl.h` in every binary folder, byte-identical and hashed.
- **Tests:** `test_tempctl` 2194 checks (was 1516): the v3 groups under the
  lifecycle plus R10 groups labelled with the handoff's section 13 items;
  green on x64, x86 and the Raspberry Pi. Oracle rewritten for v4: 1346
  arrays bit-identical with cantools, table slot and ECD slot identical, U16
  boundary and saturation vectors, stale-table refusal.
- **Simulator:** TempSim 3.0.0 (separate package) drives the v4 API with
  Start / Stop / Run permissive and runs 29 scenarios (208 expectations),
  the 14 lifecycle scenarios of the handoff included.
- **Documents:** all package documents rewritten; new standalone
  `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md`; the v3 spec and capability
  moved to the repository's `docs
otes`. CanTp remains v1.3.1
  (`tools\ecdflat.py` added to the vendored subset; minimum CanTp 1.2.0).
- **Release gates relaxed by owner decision (V4-D5, V4-D6):** the cRIO and
  myRIO native runs and the LabVIEW / LabVIEW RT wrapper tests are
  owner-executed; the package records for each Linux target whether it was
  executed or only built and inspected. Windows x64 / x86 and the Pi remain
  hard gates.

### Migration from v3.0.0

- Regenerate or update every wrapper VI against the v4 `tempctl.h`: the
  `TcCheckTemp` signature changed (`runPermissive` inserted before the
  outputs), `TcStart` and `TcStop` are new. v3 wrapper VIs must not be used
  with a v4 binary.
- Array constants: setup 17 -> **18** (append `OperatingConditionTimeout`,
  >= 1 ms, at least two loop periods and longer than the longest expected
  transient); diagnostics 25 -> **28**. `TcInit` rejects `setupLen` 17.
- Add `TcStart` after `TcInit` (and after `TcReset`); a zone no longer
  controls by itself. Add `TcStop` -> apply zeros before any `TcInit` or
  non-fault `TcReset` of a zone that may be active.
- Wire the host's combined operating condition to `runPermissive` on
  `TcStart` and every `TcCheckTemp` (1 when no such condition exists).
- Handle the new statuses 6..9 and 17 and warning 8 in the HMI and the
  shutdown logic (>= 10 is still a fault).
- CAN: replace the v3 `tempctl.dbc`, tables and any ECD with the v4 files;
  the v3 55-byte layout cannot decode v4 frames (the U16 timers move every
  later signal); `CanTp_OutputSize` is 192 (8 frames), was 216.
- TempSim 2.x cannot drive a v4 binary; use TempSim 3.0.0.

## v3.0.0 - 2026-09-18

Major revision from the v3.0.0 implementation handoff (rules decided with
the system owner; `TEMPCTL-SPEC-v3.0.0.md`, `TEMPCTL-CAPABILITY-v3.0.0.md`),
including Amendment A (2026-09-18): the out-of-range accumulator drains at
half rate (`DRAIN = 0.5`, so a sensor out of range more than a third of the
time fails: 50 % duty at about 4 x `ErrorTimeout`), the accumulator charges
on its first out-of-range tick, and `ErrorTimeout` must be at least two loop
periods. `TcVersion()` reports 0x030000. Not backwards compatible with v2.

- **API split (C1, C2, C20).** `TcStep` and its 17-in / 27-out SGL array are
  gone. `TcInit(zone, nowMs, setupArray[17], ...)` loads the setup (once, and
  again for any parameter change); `TcCheckTemp(zone, nowMs, temp1, temp2,
  diHeaterFB, diCoolerFB, &doHeater, &doCooler, &status, &warning)` is the
  control tick with live signals only; `TcReset`; `TcGetDiag(zone,
  diagArray[25], 25)` read-only diagnostics; `TcSetupCount`, `TcDiagCount`.
  Everything is `double` / `int32_t` / `uint32_t`, no enums or structs in the
  header (LabVIEW Import Shared Library wizard). No input echo, no CAN.
- **New setup parameters (C3-C5, C7, C8, C15):** `TempCtrlEnable`,
  `TempUnits` (label), `AtSetPtTimeout`, `Temp2Offset`,
  `TempCompareTimeout`, `RelayFeedbackTimeout`.
- **Behaviour:** raw control and limits, averages only for the comparison
  (C9); NaN/Inf = out of range high, no separate bad bits (C10); leaky
  out-of-range accumulator (charge 1, drain 0.5) instead of a restarting
  countdown (C11); a single failed sensor with a healthy partner is a warning
  (`RunningOnTemp2`), not a fault (C12); control pauses while the active
  sensor is out of range instead of dropping the relays (C13);
  `Initial_HC_Flag` replaces the warm-up status and gates the comparison
  (C14); disagreement warns at `TempCompareTimeout/10` and faults at the
  full time (C6); a running relay releases only after `AtSetPtTimeout` at
  the setpoint (C8); per-relay feedback warning and fault (C15); config
  check at Init with `ConfigFault` / `ConfigInvalid` (C16); Reset clears the
  averages and all history (C17); relays kept across a re-Init of a running
  zone (C18); single status code (states 0-5, faults 10-16) and single
  warning code (0-7) instead of the bit mask (C19); backwards time steps
  count as 0 ms (C21); 60-minute out-of-range event counters (C22).
- **CAN:** `dbc\tempctl.dbc` now defines the 25-value diagnostics array
  (PGN 65280, 55 bytes, 9-frame BAM) with status and warning value tables;
  the host packs `TcGetDiag`'s output with `CanTp_Pack`.
- **Tests:** `test_tempctl` rewritten, 1516 checks organised by rule number
  and by the handoff's change list (C1-C22) and scenarios (S1-S15), with
  the four duty points of Amendment A's table; passes x64, x86 and on the
  Raspberry Pi (aarch64). Oracle rewritten for the new API: 1287
  diagnostics arrays bit-identical with cantools. The bare Pi run is now
  captured in `TESTLOG.txt`.
- **Simulator:** TempSim 2.0.0 (separate package) drives the v3 API and
  runs the 15 required scenarios plus `flicker-25` with built-in
  expectations.
- **Documents:** all package documents rewritten; the spec lists the
  implementation decisions for markup (section 12; the C11-vs-R5.4
  inconsistency it raised is resolved by Amendment A). The v2.0.1 spec
  moved to the repository's `docs\notes`.
- CanTp remains v1.3.1 (only `Define` / `Pack` / `Unpack` / `RxFeed` and the
  queries are used; minimum CanTp 1.0.0).

## v2.0.3 - 2026-09-15

Packaging release: no controller change (`TcVersion()` reports 2.0.3; the
181 checks, the DBC and the tables are identical to 2.0.2).

- **The simulator is no longer in this package.** TempSim ships as its own
  packages (`TempSim_v1.0.0_win-x64`, `_linux-x64`, `_linux-arm64`, see
  `sim\CHANGELOG.md`) with its own version. The controller package is a few
  MB again instead of 121 MB.
- **CanTp is vendored as the subset TempCtl needs**: `cantp.h`, the
  binaries per target, `tools\dbc2tables.py`, the license and
  `VENDORED.txt`. CanTp's documentation, source, tests and examples are in
  the CanTp package. `VENDORED.txt` states the dependency rule: minimum
  CanTp 1.0.0 (only `Define` / `PackSgl` / `Unpack` / `RxFeed` and queries
  are used), so a CanTp release does not require a TempCtl release. This
  release carries CanTp 1.3.1.
- Repository: shipped documents live in `docs\package\` (copied flat into
  the package root); the handoff, cover notes and Word files in
  `docs\notes\`; the simulator's guide, changelog and Pi logs under `sim\`.
  `MANIFEST.txt` no longer records the zip password.
- `TEMPCTL-SPEC-v2.0.1.md` (the requirements spec sent for markup on
  2026-09-14) is included in the package.

## v2.0.2 - 2026-09-14

Maintenance release: no controller change. Re-vendors **CanTp v1.3.0**
(`third_party\cantp\`), which adds `CanTp_TransferXnet`: the frames as the
flattened LabVIEW `XNET Frame CAN` cluster array (TP.CM + TP.DT as CAN Data
frames, or one J1939 Data frame for an XNET J1939 session), converters to
and from the raw records, and exact LabVIEW timestamp conversion, after
Scott's follow-up on the CAN parsing thread. The simulator still uses
`CanTp_PackSgl` / `CanTp_Unpack` and is rebuilt against the new binaries.
`TcVersion()` reports 2.0.2.

## v2.0.1 - 2026-09-12

Maintenance release: no controller change. Re-vendors **CanTp v1.2.0**
(`third_party\cantp\`), which adds the flattened LabVIEW `J1939Msg(V4)`
cluster as a message-definition input (`CanTp_DefineFlat`), a per-frame
length array and the one-call `CanTp_Transfer`, plus a 32-bit ARM build, after
Scott's "CanParsing" notes. The TempCtl message can now be loaded into CanTp
either from `dbc\tables\` or from an ECD cluster of the same layout; the
simulator still uses `CanTp_PackSgl` / `CanTp_Unpack` and is rebuilt against
the new binaries. `TcVersion()` reports 2.0.1.


## v2.0.0 - 2026-09-04

Breaking release: the controller is now signals-in / signals-out only, and all
CAN work moved to the separate **CanTp** library (`rgravesjr-code/can-tp-dll`,
vendored here as `third_party\cantp\` v1.0.0). Decided with Scott after the
v1.0.0 review (see `docs\DESIGN-DECISIONS-2026-09-04.md`).

Controller (`TcStep`, `tempctl.h`):

- **New signal layout**: 17 inputs, 27 outputs (`TC_INPUT_COUNT` /
  `TC_SIGNAL_COUNT`, also exported as `TcInputCount()` / `TcSignalCount()`).
  The output order is the CAN message order, so the array goes into
  `CanTp_PackSgl` unchanged.
- **Deadband as offsets** from the setpoint: `HiBand = Setpoint + DeadbandHi`,
  `LoBand = Setpoint − DeadbandLo` (both reported as outputs).
- **Moving-average filter** per sensor (`FilterPoints` 1..64, default 4);
  control, limits and rationality act on the filtered values; bad samples
  (NaN/Inf) are not averaged; `FilterWarmup` status until the window is full.
- **Second sensor** (`Temp2Enable`): per-sensor limit / bad-reading
  countdowns, **failover** to the surviving sensor (status `Degraded`,
  `ActiveSensor` output), stop only when both have failed; **disagreement**
  check `|Temp1f − Temp2f| > Temp2Tolerance` for ErrorTimeout (bit set,
  operation continues).
- **Relay feedback** (`FeedbackEnable`): heater/cooler feedback compared with
  the previous command; mismatch for ErrorTimeout sets a bit, operation
  continues.
- **ErrorStatus is a bit mask** (b0 T1 hi, b1 T1 lo, b2 T1 bad, b3..b5 the same
  for T2, b6 disagree, b7 heater feedback, b8 cooler feedback, b9 config
  warning). Bits 0..8 latch until Reset/Init.
- **TempStatus** enum: InBand, HeatPending, Heating, CoolPending, Cooling,
  ErrorPending, Stopped, Degraded, FilterWarmup.
- Outputs `ControlTemp`, `Temp1Filtered`, `Temp2Filtered`, `HiBand`, `LoBand`,
  `ErrorRemainMs` (smallest running countdown), `DbRemainMs`, `ActiveSensor`.
- Unchanged: 16 zones, live configuration every call, countdowns start at the
  first observation, wrap-safe `nowMs`, Init/Step/Reset actions, config
  warning (now also bit 9), no allocation, no libc beyond memcpy/memset.
- Removed: `TcEncodeFrames`, `TcJ1939Bam`, `TcJ1939BamFrameCount`,
  `TcCanPack`, `TcNclHeader` (all replaced by CanTp).

CAN message:

- `dbc\tempctl.dbc` generated by `tools\make_tempctl_dbc.py`: PGN 65280
  (Proprietary B), priority 6, SA placeholder 0xFE, `VFrameFormat J1939PG`,
  50-byte payload = 1 TP.CM + 8 TP.DT frames as a J1939 BAM; J1939-style
  scaling (temperatures 16-bit 0.03125 °C −273, times U32 ms, 2-bit flags,
  4-bit status, 16-bit error mask). `dbc\tables\` holds the CanTp tables
  (JSON, CSV for LabVIEW, C header) produced by CanTp's `dbc2tables.py`.

Simulator (`simulator\`, .NET 10, self-contained):

- `TempSim.exe` (Windows, WPF): strip chart with sensors / ControlTemp /
  plant / setpoint / bands / limits / relay lanes, countdown bars, relay and
  feedback lamps, live controller settings, plant rates, sensor override
  sliders and fault injection, relay stuck switches, raw frames panel beside
  the CanTp-unpacked values, optional CSV/.ncl logging, settings persisted,
  `--screenshot` headless mode.
- `TempSim.Cli` (Windows x64, Linux x64, Linux arm64): seven scripted
  scenarios, CSV + .ncl per scenario, optional SocketCAN transmit (`--can`)
  and receive/decode (`--rx`) on Linux.

Verification:

- 181 unit checks on x64, x86 and Raspberry Pi 5 (aarch64).
- Python oracle: every tick of a scripted run packed by CanTp and compared
  bit-for-bit with cantools encoding `tempctl.dbc`; cantools decode equals
  `CanTp_Unpack`.
- Simulator CSV and .ncl outputs byte-identical on Windows x64 and the Pi.
- Live bus: TempSim on pi-engine (can1) → TempSim `--rx` on pi-trans (can0),
  every BAM reassembled by `CanTp_RxFeed`.

## v1.0.0 - 2026-09-04

First release (reference implementation; superseded by v2 + CanTp).

- **Temperature controller** `TcStep`: per-zone (16 slots) single-step state
  machine on an 11-element SGL array. Live configuration every tick, latched
  limit faults with ErrorTimeout countdown, deadband-timed heat/cool
  engagement that runs to setpoint, NaN/Inf reading handling (ErrorStatus 3),
  wrap-safe millisecond tick input, optional 13-element output with the two
  remaining-countdown values, config-order warning on Init and Step.
- **J1939 BAM transport** `TcJ1939Bam` / `TcEncodeFrames` /
  `TcJ1939BamFrameCount`, **generic DBC-style packer** `TcCanPack`, NI-XNET
  raw frame output, `.ncl` header.
- Builds: `tempctl.dll` x64 and x86, `libtempctl.so` x86_64; 154 unit checks;
  cantools / pretty_j1939 oracle.
