# CLAUDE.md

Two products, one repo, separate packages and version lines:

- **TempCtl** (src/): deadband temperature controller with sensor
  validation and an explicit Start / Stop / run-permissive lifecycle for
  LabVIEW CLFN, signals in / signals out, no CAN inside. Plain C99, one
  source tree -> tempctl.dll (x64 + x86, MSVC) and libtempctl.so (linux-x64
  for the Intel cRIO-904x/905x/906x, linux-armhf for the myRIO-1900,
  linux-arm64 for the Raspberry Pi bench; zig cross-build). Version =
  TC_VERSION_* in src/tempctl.h (4.0.0). The diagnostics array (TcGetDiag,
  28 doubles) is the CAN message (dbc/tempctl.dbc, dbc/tempctl.ecd, 44-byte
  J1939 BAM) transported by the standalone **CanTp** library
  (C:\CodeProjects\can-tp-dll, GitHub rgravesjr-code/can-tp-dll). TempCtl
  vendors only the CanTp subset it needs in third_party/cantp/ and pins a
  minimum CanTp version (1.2.0, for CanTp_DefineFlat); a CanTp release does
  not imply a TempCtl release.
- **TempSim** (sim/): .NET 10 simulator that closes the loop around the real
  binaries. Own version in sim/Directory.Build.props (3.0.0), own changelog
  (sim/CHANGELOG.md), own packages (one zip per target). It is TempCtl's
  closed-loop test bench, not part of the controller package.

**The v4.0.0 contract** is `docs/notes/TEMPCTL-v4.0.0-REQUIREMENTS-CHANGE-HANDOFF.md`
(approved 2026-09-22) with the decisions of
`docs/notes/TEMPCTL-v4.0.0-IMPLEMENTATION-PLAN.md` (V4-D1..D8, owner);
the shipped spec `docs/package/TEMPCTL-SPEC-v4.0.0.md` keeps the handoff's
rule numbers (R1..R9 from v3, R10 lifecycle) and lists the implementation
decisions (section 12, V4-D1..D12) for markup. The v3 contract
(`docs/notes/TEMPCTL-v3.0.0-HANDOFF.md`, Amendment A) and its spec /
capability (now in docs/notes) remain the base of R1..R9.
`docs/notes/HANDOFF-2026-09-04.md` records the v2 decisions (D1-D12, Q1-Q9)
and the history of the CanTp split.

## Build / verify / package

```bat
build.bat all                               :: dll x64+x86, 2186-check gates on both, .so linux-x64 + linux-armhf + linux-arm64
python tools\make_tempctl_dbc.py --tables --ecd  :: dbc\tempctl.dbc + dbc\tables + dbc\tempctl.ecd (cantools; checks TC_DIAG_*/TC_ST_*/TC_WN_* vs tempctl.h, asserts 44 bytes, reads the ECD back)
python tests\oracle_test.py                 :: TcInit/Start/CheckTemp/Stop/GetDiag -> CanTp_Pack (table slot + ECD slot) vs cantools, every array bit-for-bit -> ALL OK
package_dist.bat 4.0.0 [pw]                 :: dist\TempCtl_v4.0.0{,.zip,_unencrypted.zip}; gates + regeneration check + oracle + target logs; refuses failures
build_sim.bat all                           :: publish build\sim\{win-x64,linux-x64,linux-arm64}; CLI 29 scenarios (208 expectations) + ECD cross-check + WPF --screenshot
package_sim.bat 3.0.0 [pw] ["rids"]         :: dist\TempSim_v3.0.0_<rid>{,.zip,_unencrypted.zip}; re-runs the CLI + screenshot gates
```

cantools for the DBC generator and the oracle: `set PYTHONPATH=C:\CodeProjects\tools\pylibs`
(or `TEMPCTL_PYLIBS` for package_dist.bat). zig lives outside the repo at
`..\tools\zig-x86_64-windows-*\zig.exe`. No Linux runtime on the dev box:
Linux verification = ssh to the Pi bench (pi-engine 192.168.1.148 /
pi-trans 192.168.1.178, user pi, key ~/.ssh/id_ed25519): run
build\linux-arm64\test_tempctl (log into docs/testlogs/pi-test_tempctl-<date>.txt,
which package_dist.bat appends to TESTLOG.txt), then the linux-arm64
TempSim.Cli (`--scenario all`, compare CSV/.ncl with the Windows run byte for
byte; `--can can1` / `--rx can0` for the live-bus loop); paste the logs into
sim/testlogs/. The cRIO (linux-x64) and myRIO (linux-armhf) binaries are
inspected with tools/elfinfo.py and executed only on those targets by the
owner (logs docs/testlogs/crio-*.txt, myrio-*.txt; package_dist.bat records
"executed" or "built and inspected" per target, V4-D5). The armhf binary
cannot run on the aarch64 Pi (no 32-bit loader).

## Layout

```
src/tempctl.h        public API + index/code #defines (TC_SETUP_*, TC_DIAG_*, TC_ST_*, TC_WN_*); written for the LabVIEW import wizard
src/tempctl.c        controller: lifecycle (started / idle / blocked / pending / tripped), leaky accumulators, hourly rings, countdowns, comparison, feedback, control, config check
tests/test_main.c    2186 checks compiled with the source, one function per rule group (R10 groups labelled 13.x.y); tests/oracle_test.py (ctypes + cantools + ecdflat)
tools/make_tempctl_dbc.py   the only definition of the CAN message layout (over TC_DIAG_*); runs CanTp's dbc2tables.py; --ecd writes tempctl.ecd via CanTp's ecdflat.py
dbc/                 generated tempctl.dbc, tempctl.ecd and tables (TempCtl.json is what the simulator loads; tempctl.ecd ships beside it and is cross-checked)
third_party/cantp/   CanTp subset: cantp.h, binaries per target (incl. linux-armhf), tools/dbc2tables.py + ecdflat.py, LICENSE.txt, VENDORED.txt (version + sha256 + dependency rule)
docs/package/        ships in the TempCtl package, flat at its root: DISTRIBUTION_README, TEMPCTL_PACKAGE_GUIDE, LABVIEW_INTEGRATION,
                     TESTING, TEMPCTL-SPEC-v4.0.0, TEMPCTL-CAPABILITY-v4.0.0, TEMPCTL-v4.0.0-API-AND-LABVIEW-GUIDE
docs/notes/          repo-only: v4.0.0 handoff + implementation plan, v3.0.0 handoff + capability docx + superseded v3 spec/capability, HANDOFF-2026-09-04, cover notes, v2.0.1 spec
docs/testlogs/       bare test_tempctl logs from targets (pi-*, crio-*, myrio-*)
sim/TempSim.Core     Simulation (plant/profile->sensors->TcCheckTemp(runPermissive)->relays; Start/Stop/ReInit/Reset with the Stop->zero DOs->Init/Reset->Start host sequence;
                     TcGetDiag + pack + unpack each tick, companion zone), Scenario (29 built-ins with expectations), MessageTable (JSON + .ecd reader, DefineFlat), SocketCan, NativeLoader, CsvLogger/NclWriter
sim/TempSim.Cli      console: --scenario all --out DIR [--every S] [--can IFACE] [--rx IFACE] [--table x.json|x.ecd]
sim/TempSim.Wpf      WPF: PlotView, fixture view, Start/Stop/Run permissive toolbar, status panel with the lifecycle line and 8 meters, --screenshot headless mode
sim/NativeAssets.targets   copies tempctl (build\<rid>) + cantp (third_party) per RuntimeIdentifier; the csproj files copy TempCtl.json + tempctl.ecd
sim/SIMULATOR.md, sim/CHANGELOG.md, sim/START_HERE.txt, sim/Directory.Build.props, sim/testlogs/   the TempSim package's docs + Pi logs
scripts/             validate_package_version.ps1 (TempCtl), validate_sim_version.ps1 (TempSim),
                     report_package_assets.ps1 (generic, same file as in can-tp-dll), xcopy_exclude.txt
claudetodelete/      parked: v1/v2 leftovers, old CanTp packages, old sim outputs (owner rule: never delete)
build/ dist/ out/ output/   ignored
```

A document ships iff it lives in docs/package (TempCtl) or sim/ (TempSim).
Cross-references inside shipped files use bare file names.

## Invariants

- Exports are C-only, cdecl; only int32_t/uint32_t/double in signatures, no
  structs/enums/typedefs/int64/strings/callbacks/allocation; arrays are
  pointer + I32 length; booleans are int32_t 0/nonzero. Keep it that way for
  the LabVIEW import wizard. setupLen must equal TC_SETUP_COUNT exactly;
  diagLen >= TC_DIAG_COUNT, nothing written beyond it (V4-D4).
- `TC_DIAG_*` order in tempctl.h == `SIGNALS` list in make_tempctl_dbc.py
  == DBC SG_ order == ECD channel order == TempCtl.json row order == `TcDiag`
  enum in TempCtlNative.cs. The generator refuses a DBC that disagrees with
  the header (indexes, status and warning code sets), asserts the 44-byte
  payload and compares the ECD it wrote with the DBC and the tables; the
  simulator refuses a table with != 28 rows and a stale/reordered ECD.
  Changing either array is a major version: bump TC_VERSION_*, regenerate
  DBC/tables/ECD, update TEMPCTL_PACKAGE_GUIDE.md, the spec, the API guide,
  oracle_test.py, TempCtlNative.cs. New setup parameters are only ever
  appended. The eight millisecond diagnostics are U16 on the wire (0..64255,
  0xFFFF not available, saturation on the wire only).
- Bump `TC_VERSION_*` in tempctl.h, CHANGELOG.md and
  docs/package/DISTRIBUTION_README.md together;
  `scripts\validate_package_version.ps1` enforces it. TempSim: bump
  sim/Directory.Build.props and sim/CHANGELOG.md together;
  `scripts\validate_sim_version.ps1` enforces it. A package is re-released
  only when its own code changes.
- TempCtl/TempSim use CanTp_Define / DefineFlat / GetDef / Pack / Unpack /
  RxFeed + queries only (DefineFlat and GetDef since CanTp 1.2.0). Re-vendor
  third_party/cantp only when a newer CanTp function is needed, or when a
  TempCtl release is cut anyway; never edit the vendored files (copy them
  out of the release zip); update VENDORED.txt (version, zip sha256).
- Lifecycle (R10): Init and Reset leave a zone IdleStopped; only TcStart
  runs control; a false runPermissive on an active tick de-energizes both
  relays on that sample (the one deliberate exception to REQ-7) and starts
  OperatingConditionTimeout (pending -> tripped on recovery, -> fault 17 on
  expiry); an existing fault maturing on the same tick wins; nothing
  auto-restarts. Stopped ticks evaluate nothing (raw temps mirrored, rings
  age in wall time, accumulators frozen, V4-D1/D10); Stop and a trip clear
  transient warnings 1-5 (V4-D9). Init/Reset return no DO values: the host
  sequence for an active zone is Stop -> apply zeros -> Init/Reset -> Start;
  the simulator's ReInit/Reset implement exactly that.
- Timing: a countdown starts on the tick that first observes its condition
  (that tick's dt is not charged) and expires on a later tick when elapsed
  >= timeout; the operating-condition countdown follows the same rule. The
  leaky accumulator charges EVERY out-of-range active tick incl. the first
  and drains HALF the elapsed time in range (Amendment A, DRAIN 0.5, kept in
  half-ms units: 100 % duty fails at 1x ErrorTimeout, 75 % at 1.6x, 50 % at
  ~4x, <= 33 % never; the host must keep ErrorTimeout >= 2 loop periods).
  Start re-bases lastMs so stopped time never feeds a countdown. The tests
  and scenarios pin these tick by tick; do not "fix" one without the other.
- Faults latch until Reset/Init (ConfigFault: Init only); failed sensors
  are never re-admitted (Stop/Start do not heal them); the warning and
  diagnostics freeze on the fault tick; first fault wins in the order
  config, sensor, disagreement, heater fb, cooler fb, operating condition.
- Unit tests: the harness init0()/rst0() = Init/Reset + Start(1) so the v3
  checks keep their meaning; oinit0()/orst0() are the raw calls; base()
  resets zone 0, so multi-zone tests set up zone 0 last and tests needing an
  untouched zone run before the 16-zone loop of test_two_zones.
- No <math.h> in the library (keeps the .so on libc only); NaN is made from
  bits, isnan is `x != x`, finite is `x == x && x - x == 0`.
- Simulator determinism: plant/sensor math is + - * / only, xorshift noise,
  invariant-culture CSV with 4-decimal rounding. Windows and Pi outputs must
  stay byte-identical (sim/testlogs/pi-sim-*.txt). Scenario expectations
  are the sim's release gate; a scenario edit must keep 208/208. Events fire
  BEFORE the tick whose time they name and expectations are checked AFTER
  it, so a "still idle" check must sit one tick before a Start event.
  StartOnInit (default true) makes a run Init + Start at t = 0.
- NaN diagnostics (Temp2 fields while disabled, ControlTemp during an open
  sensor, RunPermissive before its first evaluation) are packed by CanTp as
  J1939 "not available" (all ones) and decode as the signal's max; that is
  expected.
- Git: pushes need a fine-grained PAT on the command line per session
  (`git push https://x-access-token:<PAT>@github.com/rgravesjr-code/temp-controller-dll.git`),
  never stored in the remote URL. Releases via the GitHub REST API; a
  TempCtl release attaches the TempCtl zips, a TempSim release (tag
  `tempsim-vX.Y.Z`) attaches the per-target TempSim zips.
