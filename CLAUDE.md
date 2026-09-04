# CLAUDE.md

TempCtl v2: dual-sensor temperature controller for LabVIEW CLFN, signals in /
signals out, no CAN inside. Plain C99, one source tree -> tempctl.dll (x64 +
x86, MSVC) and libtempctl.so (linux-x64 for cRIO-904x/905x/906x, linux-arm64
for the Raspberry Pi bench; zig cross-build). The CAN message is a DBC
(dbc/tempctl.dbc) transported by the sibling **CanTp** library
(C:\CodeProjects\can-tp-dll, GitHub rgravesjr-code/can-tp-dll), vendored
unmodified in third_party/cantp/. A .NET 10 simulator (sim/) closes the loop
around the real binaries. `docs/HANDOFF-2026-09-04.md` records every decision
taken with the owner and Scott (D1-D12, Q1-Q9) and the history of the split.

## Build / verify / package

```bat
build.bat all                               :: dll x64+x86, 181-check gates on both, .so linux-x64 + linux-arm64
python tools\make_tempctl_dbc.py --tables   :: dbc\tempctl.dbc + dbc\tables (cantools; checks order vs tempctl.h)
python tests\oracle_test.py                 :: TcStep -> CanTp_PackSgl vs cantools, every tick bit-for-bit -> ALL OK
build_sim.bat all                           :: publish build\sim\{win-x64,linux-x64,linux-arm64}; CLI all scenarios + WPF --screenshot
package_dist.bat 2.0.0 [pw]                 :: dist\TempCtl_v2.0.0{,.zip,_unencrypted.zip}; refuses failing gates / stale DBC
```

zig lives outside the repo at `..\tools\zig-x86_64-windows-*\zig.exe`. No
Linux runtime on the dev box: Linux verification = ssh to the Pi bench
(pi-engine 192.168.1.148 / pi-trans 192.168.1.178, user pi, key
~/.ssh/id_ed25519): run build\linux-arm64\test_tempctl, then the
linux-arm64 TempSim.Cli (`--scenario all`, compare CSV/.ncl with the Windows
run byte for byte; `--can can1` / `--rx can0` for the live-bus loop). The
cRIO .so is inspected with tools/elfinfo.py, executed only on a cRIO.

## Layout

```
src/tempctl.h        public API + full semantics (the spec); enum TcSignal is the array AND the DBC order
src/tempctl.c        controller: filters, per-sensor countdowns, failover, disagreement, feedback, control
tests/test_main.c    181 checks compiled with the source; tests/oracle_test.py (ctypes + cantools)
tools/make_tempctl_dbc.py   the only definition of the CAN message layout; runs CanTp's dbc2tables.py
dbc/                 generated tempctl.dbc + tables (TempCtl.json is what the simulator loads)
third_party/cantp/   CanTp release package, do not edit (VENDORED.txt has version + sha256)
sim/TempSim.Core     Simulation (plant->sensors->TcStep->relays, pack+unpack each tick), Scenario, SocketCan,
                     NativeLoader (tempctl/cantp resolver), CsvLogger/NclWriter
sim/TempSim.Cli      console: --scenario all --out DIR [--can IFACE] [--rx IFACE]
sim/TempSim.Wpf      WPF: PlotView (strip chart), settings panels built in code, --screenshot headless mode
sim/NativeAssets.targets   copies tempctl (build\<rid>) + cantp (third_party) per RuntimeIdentifier
scripts/             packaging helpers; claudetodelete/ = parked v1 CAN sources + examples (owner rule: never delete)
```

## Invariants

- Exports are C-only, cdecl, no structs/strings/callbacks/allocation; arrays
  are pointer + I32 length. Keep it that way for LabVIEW.
- `enum TcSignal` order == `SIGNALS` list in make_tempctl_dbc.py == DBC SG_
  order == TempCtl.json row order. The generator refuses to write a DBC that
  disagrees with the header; the simulator refuses a table with != 27 rows.
  Changing the array is a major version: bump TC_VERSION_*, regenerate the
  DBC/tables, update TEMPCTL_PACKAGE_GUIDE.md, oracle_test.py, TempCtlNative.cs.
- Bump `TC_VERSION_*` in tempctl.h, CHANGELOG.md and DISTRIBUTION_README.md
  together; `scripts\validate_package_version.ps1` enforces it.
- Countdowns start at the first Step that observes a condition (dt of that
  tick is not charged) and expire when remaining <= 0. Tests encode this.
- Latched things (failed sensors, disagree, feedback bits, stopped) clear
  only on Reset/Init (owner Q2/Q3). Config bit 9 is not latched.
- Filters keep running while Stopped; NaN samples never enter a filter.
- No <math.h> in the library (keeps the .so on libc only); NaN is made from
  bits, isnan is `x != x`.
- Simulator determinism: plant/sensor math is + - * / only, xorshift noise,
  invariant-culture CSV with 4-decimal rounding. Windows and Pi outputs must
  stay byte-identical (docs/testlogs/pi-vs-windows-*.txt).
- NaN outputs (Temp2Filtered while disabled) are packed by CanTp as J1939
  "not available" (all ones) and decode as the signal's max; that is expected.
- Git: pushes need a fine-grained PAT on the command line per session
  (`git push https://x-access-token:<PAT>@github.com/rgravesjr-code/temp-controller-dll.git`),
  never stored in the remote URL. Releases via the GitHub REST API.
