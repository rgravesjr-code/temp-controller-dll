# Pre-release review corrections

These changes retain the unreleased TempCtl 4.0.0 and TempSim 3.0.0
versions. They do not change public setup/diagnostic arrays or the wire
layout. No tags, pushes or GitHub releases were made.

## Corrected behavior

| Finding | Correction | Regression evidence |
|---|---|---|
| Repeated Stop loses hourly-history age | Age both rings before rebasing the controller clock | Eight native checks, including wrap and no double-aging |
| Scenario events use the preceding sample time | Advance plant and all zone clocks, apply events, then sample/control | Start/Reset elapsed-time and companion checks; reset scenario expiry now 22.0 s |
| WPF overwrites initial permissive and leaves checkbox stale | Initialize from config; refresh checkbox under event suppression | Full blocked-start, permissive-trip and reset WPF gates |
| Noise has positive bias and exceeds amplitude | Correct 53-bit scaling to [-1,1) | Bounds/mean checks and noisy fixture cross-platform comparison |
| BAM timestamps overlap at 100 ms control period | Separate message interval (default 1000 ms) from per-tick verification; paced live sender | Chronological NCL checks, pacing/overlap/error tests |
| Stale wire tables can pass startup | Both applications validate mandatory JSON/ECD pair against embedded generated layout | Reorder, scale, size, malformed row, missing/corrupt ECD and explicit-override checks |
| Truncated scenarios say ALL OK | Track unreached expectations and return exit 2 / INCOMPLETE | Zero-duration CLI gate; snapshot reports explicitly list pending checks |
| Oracle crash does not stop packaging | Any nonzero exit blocks packaging | Actual batch guard exercised with nonzero exit and no FAIL marker |
| Target filename alone certifies execution | Match version, summary, exit status and both binary hashes | Stale, failed, incomplete and wrong-version target-log regressions |
| Missing/invalid assets escape inspection | Require specified assets, successful inspection, matching architectures and TempCtl exports | Package gates plus missing-asset regression |
| cRIO-906x incorrectly called Intel | Architecture guidance corrected; no 906x compatibility claim without target validation | NI source linked in LabVIEW integration guide |

## Verification

- Native controller: 2194/2194 on Windows x64, Windows x86 and Pi ARM64.
- CAN oracle: 1346 diagnostic arrays bit-exact against cantools 43.0.0;
  table and ECD slots identical. Generated DBC/ECD/JSON unchanged bytewise.
- All 29 scenarios: 208/208 on Windows and Pi; no unpack mismatches.
- Simulator regressions: 26/26 on Windows and Pi, including 16 concurrent
  native zones and all diagnostics over 64,000 samples versus a serial run.
- Fixture checks: 30/30 on both platforms; 600-second fixture run with
  sensor noise. All 63 scenario/fixture CSV/NCL files match byte for byte.
- Release-gate regressions: 11/11, including required current Pi evidence.
  WPF lifecycle gates complete all their
  expectations; failover screenshot reports seven reached and two pending.
- Native Linux x64, ARMHF and ARM64 artifacts rebuilt; only ARM64 executed
  here. Current target evidence is in the `*-2026-09-22-review.txt` logs.
- Both product packages and encrypted/plain ZIPs were regenerated through
  their gates. All four package manifests were independently rehashed.

## Remaining target acceptance

The owner still runs native tests on the Intel cRIO and myRIO, and the
32-bit LabVIEW import/smoke sequence and RT wrapper checks. New target logs
must use the metadata format in `docs/package/TESTING.md`; older logs do
not certify rebuilt binaries. The cRIO-906x family needs separate ARM ABI
validation. Live SocketCAN was not re-run; the paced sender was exercised
with a capture callback on both Windows and Pi.

The prior fixture model's pending Pi validation is now completed for the
tested settings. This is deterministic software-model verification, not
calibration against a physical thermal fixture.
