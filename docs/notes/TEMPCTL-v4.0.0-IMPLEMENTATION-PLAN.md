# TempCtl v4.0.0 / TempSim v3.0.0 — Implementation Plan

Companion to `TEMPCTL-v4.0.0-REQUIREMENTS-CHANGE-HANDOFF.md` (decision date
2026-09-22). This file records the implementation decisions taken with the
owner on 2026-09-22 and the order of work. Repo-only; it does not ship.

## 1. Decisions taken on top of the handoff (owner, 2026-09-22)

| # | Decision | Where it lands |
|---|---|---|
| V4-D1 | While Started = 0 nothing is evaluated (R10.6 as written), but the raw inputs are still mirrored into `Temp1Raw` / `Temp2Raw` on every stopped tick so displays stay live. `ControlTemp`, averages, accumulators, warnings 1/2/5 freeze. | tempctl.c, spec §12, tests |
| V4-D2 | A refused `TcStart` (blocked, or pending/tripped with permissive false) records the evaluated permissive in `RunPermissive`; only the idempotent already-started Start and disabled/faulted calls do not. `TcStop` never touches it. | tempctl.c, spec |
| V4-D3 | `tempctl.ecd` is generated here by `make_tempctl_dbc.py` from the same `SIGNALS` list, in `TC_DIAG_*` order, using the J1939Msg(V4) cluster layout and cipher documented in CanTp's `ecdflat.py`. No reorder table exists because there is no reorder. The generator decrypts its own output with `ecdflat.py` and compares every channel with the DBC. | tools/, dbc/, oracle |
| V4-D4 | Array lengths keep the v3 rule: `setupLen` must equal 18 (`TC_ERR_ARG` otherwise), `diagLen` must be >= 28 and elements beyond 28 are never written. Handoff test 13.1.5 is read as a diag-buffer test. | tempctl.c, tests, guide |
| V4-D5 | myRIO-1900 is a stretch target. The 32-bit ARM `.so` and `test_tempctl` are built (zig `arm-linux-gnueabihf`), inspected with `elfinfo.py` and shipped. If the owner runs them on a myRIO the dated log goes into `docs/testlogs/` and the packager includes it; if not, `TESTLOG.txt` states "built and inspected, not executed" and the package still cuts on the Windows x64/x86 gates, the oracle and the Pi log. Same rule for the Intel cRIO. This relaxes handoff §17 items 4, 7 and 13 by owner decision. | build.bat, package_dist.bat, TESTING.md |
| V4-D6 | LabVIEW wrapper generation and the LabVIEW RT tests (§12, §15) are owner-executed. The package ships the header, the instructions and a checklist; the results are recorded by the owner in `TESTLOG.txt` when available. | LABVIEW guide |
| V4-D7 | glibc floor for the ARM 32-bit build is 2.24 (same as the other zig targets) unless `ldd --version` on the actual myRIO says otherwise. | build.bat |
| V4-D8 | TempSim goes to 3.0.0 (handoff §14 recommends a major). The uncommitted TempSim 2.1.0 to 2.2.1 fixture work is committed first, as its own history, before any v4 edit touches `sim/`. | git |

## 2. Wire layout (for the record)

Current v3 message: 55 bytes. v4: seven `MS32` timers become `U16`
(saves 14 bytes), then `RunPermissive` (2-bit flag, NaN = 3 not available),
`OperatingConditionRemainMs` (U16), `ControllerStarted` (2-bit flag) are
appended: 41 + 3 = 44 bytes, 7 data frames + TP.CM. The generator asserts
44. CanTp's own clamp-to-max gives the 64,255 saturation; NaN packs as all
ones already. No CanTp change and no re-vendor.

## 3. Work order

Each phase ends with its gate green before the next starts.

### Phase 0 — baseline commit (owner go-ahead needed)
- Commit the fixture work (`sim/` 2.2.1, `tests/TempSim.FixtureChecks/`,
  `tools/make_tempsim_manual.py`, `sim/START_HERE.txt`, manual PDF,
  `AGENTS.md`, `.gitignore`) as "TempSim v2.2.1: animated fixture" with the
  Pi-validation-pending note that is already in its changelog.
- Commit the handoff and this plan.

### Phase 1 — controller (src/, tests/)
1. `tempctl.h`: version 4.0.0, `TC_SETUP_COUNT 18`, `TC_DIAG_COUNT 28`,
   `TC_SETUP_OPERATING_CONDITION_TIMEOUT`, `TC_DIAG_RUN_PERMISSIVE` /
   `_OPERATING_CONDITION_REMAIN_MS` / `_CONTROLLER_STARTED`, statuses 6..9
   and 17, warning 8, prototypes for `TcStart` / `TcStop`, new
   `runPermissive` parameter on `TcCheckTemp`, rewritten function comments
   (lifecycle, Stop -> apply zeros -> Init/Reset, serialization incl. GetDiag).
2. `tempctl.def`: add the two exports. `tempctl.rc`: file version 4.0.0.
3. `tempctl.c`:
   - zone state: `started`, `lifecycle` (idle / blocked / pending / tripped),
     `runPerm` (-1 = NaN), `ocCond`/`ocEl` countdown, `cfg.ocTimeout`;
   - `TcInit`: validate index 17 (>= 1 ms when enabled), drop R9.3 relay
     keeping, clear started, zero both command sets, return `IdleStopped`;
   - `TcStart`: the eight-step order of R10.3; accepted Start resets
     `lastMs`, all countdowns, average buffers, `initialHc`; keeps sensor
     failure, accumulators, hourly rings;
   - `TcStop`: R10.4 including the per-state table and warning rules;
   - `TcReset`: v3 clearing plus started = 0, lifecycle idle;
   - `TcCheckTemp`: disabled -> stopped/blocked/tripped path (V4-D1 mirror
     of raw temps, warning 8 live) -> pending path (countdown only, true
     permissive -> tripped, expiry -> fault 17 with frozen warning) ->
     active path (v3 evaluation first, then permissive trip R10.7,
     feedback suppression R10.9);
   - `TcGetDiag`: three new fields; mirrors follow every stateful call.
   - Hourly rings are keyed on `nowMs` differences so a long Stop does not
     corrupt them when `lastMs` is re-based at Start.
4. `tests/test_main.c`: harness `base()` becomes Reset -> Init -> Start
   (permissive 1) so the 1516 v3 checks keep their meaning; `tick()` gains
   the permissive argument. New groups: `test_R10_start`, `test_R10_stop`,
   `test_R10_init_reset`, `test_R10_blocked`, `test_R10_permissive_loss`,
   `test_R10_time`, `test_R10_warning_status`, `test_R10_two_zones`,
   `test_R10_diag_readonly`, covering handoff §13.1 to §13.7 item by item
   (each check labelled with its 13.x.y number). Expected new count is
   recorded in TESTING.md after the run.
5. Gate: `build.bat all` green on x64 and x86.

### Phase 2 — DBC / ECD / oracle (tools/, dbc/, tests/)
1. `make_tempctl_dbc.py`: `MS16` layout, three appended signals, status and
   warning value tables extended, "v4" strings, payload-length assertion
   (44), `--ecd` writer using a vendored copy of CanTp's `ecdflat.py`
   (added to `third_party/cantp/tools/` from the v1.3.1 zip; VENDORED.txt
   updated, no version change), decode-and-compare step against the DBC.
2. Regenerate `dbc/tempctl.dbc`, `dbc/tables/`, new `dbc/tempctl.ecd`.
3. `oracle_test.py`: 28-value arrays, `TcStart` in the setup, permissive
   argument, U16 boundary vectors 0 / 1 / 64,255 / 64,256 / large / NaN,
   ECD-flattened definition through `CanTp_DefineFlat` compared bit for bit
   with the table definition and with cantools; v3 table rejected with a
   clear message.
4. Gate: generator + `--tables --ecd` + oracle ALL OK.

### Phase 3 — Linux targets (build.bat, tools/elfinfo.py)
1. Add `linux-armhf` (zig `arm-linux-gnueabihf.2.24`, V4-D7) next to x64
   and arm64; `elfinfo.py` reports it; `DEPENDENCIES.txt` gains the row.
2. Pi: `test_tempctl` arm64 run, log to `docs/testlogs/pi-test_tempctl-<date>.txt`.
3. cRIO / myRIO: build + inspect here; execution by the owner (V4-D5).

### Phase 4 — TempSim 3.0.0 (sim/)
1. `TempCtlNative.cs` / `TcConst` / `TcDiag` / `TcSetup` / `TcStatus` /
   `TcWarning`: v4 constants, `TcStart`, `TcStop`, new `TcCheckTemp`;
   `Controller` refuses a non-4 major and wrong counts.
2. `Simulation`: `Start(perm)`, `Stop()`, live `RunPermissive` property,
   Init/Reset leave the zone idle, a Stop writes both modelled relays off;
   `ReInit` becomes Stop -> apply zeros -> Init (-> Start where the
   scenario says so). CSV / `.ncl` / trace columns for the three new
   diagnostics; ECD loading path via `CanTp_DefineFlat` alongside the JSON
   table; stale table / v3 binary fail with a clear message.
3. `Scenario`: every existing scenario gets `Start` at t = 0;
   `setpoint-reinit` is rewritten as the Stop -> Init -> Start sequence
   (handoff scenario 11); the 14 new scenarios of §14 are added with
   expectations; the CLI prints the new total (was 110).
4. WPF: Start / Stop buttons, Run Permissive toggle, status/warning names,
   Started / permissive / condition-remaining readouts; `--screenshot`
   gate retained. `SIMULATOR.md`, `sim/CHANGELOG.md` (3.0.0 with the v3 to
   v4 migration note), `Directory.Build.props`.
5. Gate: `build_sim.bat all` green (CLI all scenarios, screenshot); Pi
   CLI run byte-identical to Windows (`sim/testlogs/pi-sim-<date>.txt`).

### Phase 5 — documents (docs/package/, root)
- `TEMPCTL-SPEC-v4.0.0.md` (handoff rule numbers R10.x kept, §12
  implementation decisions V4-D1..D8), `TEMPCTL-CAPABILITY-v4.0.0.md`,
  new `TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE.md` (handoff §16 list),
  `DISTRIBUTION_README.md`, `TEMPCTL_PACKAGE_GUIDE.md`, `LABVIEW_INTEGRATION.md`
  (import settings of §12, target-aware library path, the two basenames),
  `TESTING.md` (gates, check counts, owner-executed gates), `CHANGELOG.md`
  (v3 -> v4 migration section, wire incompatibility warning).
- v3 spec and capability move to `docs/notes/` as superseded.
- `CLAUDE.md` / `AGENTS.md` build lines, counts, versions.

### Phase 6 — packaging
- `package_dist.bat 4.0.0`: header copied byte-identical into every target
  folder (root, `x86\`, `linux-x64\`, `linux-armhf\`, `linux-arm64\`),
  `dbc\tempctl.ecd` in the manifest, armhf row in `DEPENDENCIES.txt`,
  cRIO/myRIO log handling per V4-D5, sha256 manifest check on every file.
- `package_sim.bat 3.0.0` for win-x64, linux-x64, linux-arm64.
- `validate_package_version.ps1` / `validate_sim_version.ps1` unchanged in
  spirit; extended to check `TcSetupCount`/`TcDiagCount` strings in the
  README.
- Commits: "TempCtl v4.0.0 + TempSim v3.0.0: ...", tags, GitHub releases
  (owner supplies the PAT on the command line, per CLAUDE.md).

## 4. Owner-executed items (checklist)

- [ ] `ldd --version` (or `/lib/libc.so.6`) on the myRIO-1900 image -> glibc floor (V4-D7).
- [ ] Run `linux-armhf/test_tempctl` on the myRIO, paste the log into `docs/testlogs/myrio-test_tempctl-<date>.txt`.
- [ ] Run `linux-x64/test_tempctl` on the Intel cRIO, log into `docs/testlogs/crio-test_tempctl-<date>.txt`.
- [ ] Import `tempctl.h` + x86 DLL in 32-bit LabVIEW 2026 with the §12 settings; smoke test Version -> SetupCount -> DiagCount -> Init -> Start -> CheckTemp -> Stop -> Reset -> GetDiag; record in `TESTLOG.txt`.
- [ ] LabVIEW RT wrapper run on cRIO and (if possible) myRIO.

## 5. Status (2026-09-22, end of the implementation session)

| Phase | Result |
|---|---|
| 0 | fixture work committed (TempSim 2.2.1), handoff + plan committed |
| 1 | controller v4.0.0: 2186 checks green on win-x64, win-x86 and the Pi (arm64); linux-armhf built and ELF-inspected (EABI v5 hard float, no libc import) |
| 2 | DBC 44 bytes, tables, tempctl.ecd generated and cross-checked; oracle 1346 arrays ALL OK, table slot == ECD slot |
| 3 | Pi log docs/testlogs/pi-test_tempctl-2026-09-22.txt; cRIO / myRIO execution pending (owner, V4-D5) |
| 4 | TempSim 3.0.0: 29 scenarios / 208 expectations on Windows and the Pi, 60 CSV/.ncl byte-identical; screenshot gate green |
| 5 | all package documents rewritten for v4; v3 spec / capability moved to docs/notes |
| 6 | dist\TempCtl_v4.0.0 (+ zips) and dist\TempSim_v3.0.0_{win-x64,linux-x64,linux-arm64} (+ zips) built by the gates; not yet tagged / pushed / released |

Open (owner): the checklist of section 4 (myRIO glibc, myRIO and cRIO
native runs, LabVIEW import + smoke test, LabVIEW RT runs), then re-run
`package_dist.bat 4.0.0` so `TESTLOG.txt` records the target logs, tag,
push and release.
