# CLAUDE.md

Two products, one repo, separate packages and version lines:

- **TempCtl** (src/): deadband temperature controller with sensor
  validation for LabVIEW CLFN, signals in / signals out, no CAN inside.
  Plain C99, one source tree -> tempctl.dll (x64 + x86, MSVC) and
  libtempctl.so (linux-x64 for cRIO-904x/905x/906x, linux-arm64 for the
  Raspberry Pi bench; zig cross-build). Version = TC_VERSION_* in
  src/tempctl.h (3.0.0). The diagnostics array (TcGetDiag, 25 doubles) is
  the CAN message (dbc/tempctl.dbc) transported by the standalone **CanTp**
  library (C:\CodeProjects\can-tp-dll, GitHub rgravesjr-code/can-tp-dll).
  TempCtl vendors only the CanTp subset it needs in third_party/cantp/ and
  pins a minimum CanTp version (1.0.0); a CanTp release does not imply a
  TempCtl release.
- **TempSim** (sim/): .NET 10 simulator that closes the loop around the real
  binaries. Own version in sim/Directory.Build.props (2.0.0), own changelog
  (sim/CHANGELOG.md), own packages (one zip per target). It is TempCtl's
  closed-loop test bench, not part of the controller package.

**The v3.0.0 contract** is `docs/notes/TEMPCTL-v3.0.0-HANDOFF.md` (rules
decided with the owner and Scott, standalone) with
`docs/notes/TEMPCTL-v3.0.0-Capability.docx`; the shipped spec
`docs/package/TEMPCTL-SPEC-v3.0.0.md` keeps the handoff's rule numbers and
lists the implementation decisions (section 12) for markup.
`docs/notes/HANDOFF-2026-09-04.md` records the v2 decisions (D1-D12, Q1-Q9)
and the history of the CanTp split.

## Build / verify / package

```bat
build.bat all                               :: dll x64+x86, 1516-check gates on both, .so linux-x64 + linux-arm64
python tools\make_tempctl_dbc.py --tables   :: dbc\tempctl.dbc + dbc\tables (cantools; checks the TC_DIAG_* order vs tempctl.h)
python tests\oracle_test.py                 :: TcCheckTemp/TcGetDiag -> CanTp_Pack vs cantools, every array bit-for-bit -> ALL OK
package_dist.bat 3.0.0 [pw]                 :: dist\TempCtl_v3.0.0{,.zip,_unencrypted.zip}; gates + DBC check + oracle + Pi log; refuses failures
build_sim.bat all                           :: publish build\sim\{win-x64,linux-x64,linux-arm64}; CLI 16 scenarios (110 expectations) + WPF --screenshot
package_sim.bat 2.0.0 [pw] ["rids"]         :: dist\TempSim_v2.0.0_<rid>{,.zip,_unencrypted.zip}; re-runs the CLI + screenshot gates
```

cantools for the DBC generator and the oracle: `set PYTHONPATH=C:\CodeProjects\tools\pylibs`
(or `TEMPCTL_PYLIBS` for package_dist.bat's oracle run). zig lives outside
the repo at `..\tools\zig-x86_64-windows-*\zig.exe`. No Linux runtime on
the dev box: Linux verification = ssh to the Pi bench (pi-engine
192.168.1.148 / pi-trans 192.168.1.178, user pi, key ~/.ssh/id_ed25519):
run build\linux-arm64\test_tempctl (log into docs/testlogs/, which
package_dist.bat appends to TESTLOG.txt), then the linux-arm64 TempSim.Cli
(`--scenario all`, compare CSV/.ncl with the Windows run byte for byte;
`--can can1` / `--rx can0` for the live-bus loop); paste the logs into
sim/testlogs/. The cRIO .so is inspected with tools/elfinfo.py, executed
only on a cRIO.

## Layout

```
src/tempctl.h        public API + index/code #defines (TC_SETUP_*, TC_DIAG_*, TC_ST_*, TC_WN_*); written for the LabVIEW import wizard
src/tempctl.c        controller: leaky accumulators, hourly rings, countdowns, comparison, feedback, control, config check
tests/test_main.c    1516 checks compiled with the source, one function per rule group; tests/oracle_test.py (ctypes + cantools)
tools/make_tempctl_dbc.py   the only definition of the CAN message layout (over TC_DIAG_*); runs CanTp's dbc2tables.py
dbc/                 generated tempctl.dbc + tables (TempCtl.json is what the simulator loads)
third_party/cantp/   CanTp subset: cantp.h, binaries per target, tools/dbc2tables.py, LICENSE.txt, VENDORED.txt (version + sha256 + dependency rule)
docs/package/        ships in the TempCtl package, flat at its root: DISTRIBUTION_README, TEMPCTL_PACKAGE_GUIDE,
                     LABVIEW_INTEGRATION, TESTING, TEMPCTL-SPEC-v3.0.0, TEMPCTL-CAPABILITY-v3.0.0
docs/notes/          repo-only: v3.0.0 handoff + capability docx, HANDOFF-2026-09-04, cover notes, superseded v2.0.1 spec, .docx
docs/testlogs/       bare Pi test_tempctl logs
sim/TempSim.Core     Simulation (plant/profile->sensors->TcCheckTemp->relays, TcGetDiag + pack + unpack each tick, companion zone),
                     Scenario (16 built-ins with expectations), SocketCan, NativeLoader, CsvLogger/NclWriter
sim/TempSim.Cli      console: --scenario all --out DIR [--every S] [--can IFACE] [--rx IFACE]
sim/TempSim.Wpf      WPF: PlotView (strip chart), setup/status panels built in code, --screenshot headless mode
sim/NativeAssets.targets   copies tempctl (build\<rid>) + cantp (third_party) per RuntimeIdentifier
sim/SIMULATOR.md, sim/CHANGELOG.md, sim/Directory.Build.props, sim/testlogs/   the TempSim package's docs + Pi logs
scripts/             validate_package_version.ps1 (TempCtl), validate_sim_version.ps1 (TempSim),
                     report_package_assets.ps1 (generic, same file as in can-tp-dll), xcopy_exclude.txt
claudetodelete/      parked: v1/v2 leftovers, old CanTp packages, old sim outputs (owner rule: never delete)
build/ dist/ out/    ignored
```

A document ships iff it lives in docs/package (TempCtl) or sim/ (TempSim).
Cross-references inside shipped files use bare file names.

## Invariants

- Exports are C-only, cdecl; only int32_t/uint32_t/double in signatures, no
  structs/enums/typedefs/int64/strings/callbacks/allocation; arrays are
  pointer + I32 length. Keep it that way for the LabVIEW import wizard.
- `TC_DIAG_*` order in tempctl.h == `SIGNALS` list in make_tempctl_dbc.py
  == DBC SG_ order == TempCtl.json row order == `TcDiag` enum in
  TempCtlNative.cs. The generator refuses to write a DBC that disagrees
  with the header; the simulator refuses a table with != 25 rows. Changing
  either array is a major version: bump TC_VERSION_*, regenerate the
  DBC/tables, update TEMPCTL_PACKAGE_GUIDE.md, the spec, oracle_test.py,
  TempCtlNative.cs. New setup parameters are only ever appended.
- Bump `TC_VERSION_*` in tempctl.h, CHANGELOG.md and
  docs/package/DISTRIBUTION_README.md together;
  `scripts\validate_package_version.ps1` enforces it. TempSim: bump
  sim/Directory.Build.props and sim/CHANGELOG.md together;
  `scripts\validate_sim_version.ps1` enforces it. A package is re-released
  only when its own code changes.
- TempCtl/TempSim use CanTp_Define / Pack / Unpack / RxFeed + queries only
  (all present since CanTp 1.0.0). Re-vendor third_party/cantp only when a
  newer CanTp function is needed, or when a TempCtl release is cut anyway;
  never edit the vendored files; update VENDORED.txt (version, zip sha256).
- Timing: a countdown starts on the tick that first observes its condition
  (that tick's dt is not charged) and expires on a later tick when elapsed
  >= timeout. The leaky accumulator charges EVERY out-of-range tick incl.
  the first and drains HALF the elapsed time in range (Amendment A, DRAIN
  0.5, kept in half-ms units: 100 % duty fails at 1x ErrorTimeout, 75 % at
  1.6x, 50 % at ~4x, <= 33 % never; the host must keep ErrorTimeout >= 2
  loop periods). The tests and scenarios pin these tick by tick; do not
  "fix" one without the other.
- Faults latch until Reset/Init (ConfigFault: Init only); failed sensors
  are never re-admitted; the warning and diagnostics freeze on the fault
  tick; first fault wins in the order config, sensor, disagreement, heater
  fb, cooler fb.
- A re-Init of a running, valid, enabled zone keeps the relays (R9.3):
  every unit test starts from a `TcReset` zone (harness `base()`), and the
  simulator's Simulation constructor resets before it inits.
- No <math.h> in the library (keeps the .so on libc only); NaN is made from
  bits, isnan is `x != x`, finite is `x == x && x - x == 0`.
- Simulator determinism: plant/sensor math is + - * / only, xorshift noise,
  invariant-culture CSV with 4-decimal rounding. Windows and Pi outputs must
  stay byte-identical (sim/testlogs/pi-sim-*.txt). Scenario expectations
  are the sim's release gate; a scenario edit must keep 110/110.
- NaN diagnostics (Temp2 fields while disabled, ControlTemp during an open
  sensor) are packed by CanTp as J1939 "not available" (all ones) and
  decode as the signal's max; that is expected.
- Git: pushes need a fine-grained PAT on the command line per session
  (`git push https://x-access-token:<PAT>@github.com/rgravesjr-code/temp-controller-dll.git`),
  never stored in the remote URL. Releases via the GitHub REST API; a
  TempCtl release attaches the TempCtl zips, a TempSim release (tag
  `tempsim-vX.Y.Z`) attaches the per-target TempSim zips.
