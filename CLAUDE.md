# CLAUDE.md

Temperature controller + J1939 BAM / NI-XNET raw-frame encoder for LabVIEW
CLFN. Plain C99, one source tree -> tempctl.dll (x64 + x86, MSVC) and
libtempctl.so (x86_64 NI Linux RT, zig cross-compile, target cRIO-9045).

## Build / verify / package

```bat
build.bat all              :: dll x64+x86, run 154-check gates on both, .so + linux test binary
python tests\oracle_test.py   :: cantools + pretty_j1939 oracle (pip install cantools pretty_j1939)
package_dist.bat 1.0.0 [pw]   :: dist\TempCtl_v1.0.0{,.zip,_unencrypted.zip}; refuses failing gates
```

zig lives outside the repo at `..\tools\zig-x86_64-windows-*\zig.exe` (or
`ZIG_HOME`). No Linux runtime on the dev box: the .so is inspected with
`tools\elfinfo.py`, executed only on the cRIO via `build\linux-x64\test_tempctl`.

## Layout

```
src/tempctl.h    public API + full semantics (the spec)
src/tempctl.c    controller state machine, 16 zone slots
src/j1939.c      BAM builder, XNET raw frame writer, .ncl header
src/canpack.c    DBC-style packer (Intel/Motorola start-bit semantics)
tests/           test_main.c (compiled with sources), oracle_test.py (ctypes)
examples/        make_sample_ncl.py -> sample .ncl/.csv from a simulated plant
scripts/         packaging helpers (version check, DEPENDENCIES/MANIFEST)
*.md             DISTRIBUTION_README, TEMPCTL_PACKAGE_GUIDE (API ref),
                 LABVIEW_INTEGRATION (CLFN + cRIO), TESTING, CHANGELOG
```

## Invariants

- Exports are C-only, cdecl, no structs/strings/callbacks/allocation; arrays
  are pointer + I32 length. Keep it that way for LabVIEW.
- Bump `TC_VERSION_*` in tempctl.h, CHANGELOG.md and DISTRIBUTION_README.md
  together; `scripts\validate_package_version.ps1` enforces it.
- Countdowns start at the first Step that observes a condition (dt of that
  tick is not charged). Tests encode this.
- Config order `LoLimit < LoDeadband <= Setpoint <= HiDeadband < HiLimit`
  is a warning (1), not an error, on Init and Step.
- Frame output is always the 24-byte NI-XNET raw record; never change the
  layout without updating TEMPCTL_PACKAGE_GUIDE.md and oracle_test.py.
- No <math.h> in the library (keeps the .so on libc only).
