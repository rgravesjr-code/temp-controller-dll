# TempCtl v3.0.0 — Amendment A

**Date:** 2026-09-18
**Applies to:** `TEMPCTL-v3.0.0-HANDOFF.md` (amended in place) and the delivered TempCtl v3.0.0 / TempSim v2.0.0 work tree.
**Raised by:** the implementing project's review, which found that acceptance row C11 contradicts rule R5.4.
**Approved by:** Scott.
**Release status:** apply this amendment, re-run the gates, then release. Nothing about the rest of v3.0.0 changes.

---

## A1. The contradiction, and who was wrong

The implementing project is correct, and the handoff was at fault. R5.4 specified a leaky accumulator with a 1:1 drain; C11 claimed a sensor out of range half the time would fail at about 2 × `ErrorTimeout`. With a 1:1 drain a 50 % duty cycle nets exactly zero, so it never fails. R5.4 was implemented as written; C11 was an arithmetic error in the acceptance test, not a defect in the code.

For a sensor out of range a fraction `d` of the time, with drain factor `k`, the accumulator grows at `d − k(1−d)` per ms and fails at `E = ErrorTimeout`:

| Drain `k` | Fails above duty | 50 % duty | 60 % | 75 % | 100 % |
|---|---|---|---|---|---|
| 1.0 (as delivered) | 50 % | never | 5 × E | 2 × E | 1 × E |
| **0.5 (adopted)** | **33 %** | **4 × E** | **2.5 × E** | **1.6 × E** | **1 × E** |
| 0.25 | 20 % | 2.7 × E | 1.9 × E | 1.3 × E | 1 × E |
| 0 | any | 2 × E | 1.7 × E | 1.3 × E | 1 × E |

C11's original wording implied `k = 0`: pure accumulation with no drain. That was never the intent — with no drain, rare glitches accumulate forever and a healthy sensor eventually fails after a long run.

## A2. Change 1 — drain rate becomes 0.5 (R5.4)

**Decision:** `DRAIN = 0.5`. In range, `accum = max(0, accum − 0.5 × elapsed)`. Out of range is unchanged at `accum += elapsed`.

**Reason.** The accumulator exists (handoff CC-2) so that a sensor flickering in and out of range cannot disturb control indefinitely without ever resolving. With a 1:1 drain, a sensor sitting near 50 % duty does exactly that: control keeps pausing whenever the reading is out of range (R5.7), and the sensor never fails, so the operator never gets a definite answer. With `DRAIN = 0.5` anything worse than one-third duty resolves into a failure, while a genuinely recovered sensor still drains to zero and a single glitch still costs nothing. A smaller drain would fail sensors sooner but would let unrelated, widely spaced glitches add up over a long test.

**Amended R5.4 behaviour table** (now in the handoff):

| Duty out of range | Time to failure |
|---|---|
| 100 % | 1 × E |
| 75 % | 1.6 × E |
| 50 % | 4 × E |
| 33 % or less | never |

## A3. Change 2 — C11 acceptance test corrected

Replace the C11 row with:

> 100 % duty fails at 1 × `ErrorTimeout`; 50 % duty fails at about 4 × `ErrorTimeout`; 33 % duty or less never fails; a single-tick glitch never fails; the accumulator charges on its first out-of-range tick.

## A4. Change 3 — the accumulator's first tick keeps charging (confirmed, no code change)

The implementing project proposed giving the accumulator the same one-tick grace as the relay countdowns, then reversed that decision because it would make an alternating one-tick excursion immune forever. **That reversal is correct and is now the rule.** The one-tick grace in §1.1 of the handoff applies to the deadband, at-setpoint, disagreement and relay-feedback countdowns only.

## A5. Change 4 — `ErrorTimeout` floor (documentation only)

Charging from the first tick exposes an edge the setup check cannot catch: if `ErrorTimeout` is shorter than one loop period, a single out-of-range reading reaches the threshold on the tick it is first seen, failing the sensor and dropping both relays from one sample — against the key rule in §1.1. TempCtl does not know the host loop rate, so it cannot detect this.

**Rule added to R5.4 and to the configuration notes:** set `ErrorTimeout` to at least **two loop periods** (≥ 200 ms at a 100 ms loop). No code change; state it in the spec, the capability document and the LabVIEW guide.

## A6. Change 5 — ship the header in every build package (§9.1)

The `.h` must be **inside each build package** — the Windows x64 `.dll` package and the cRIO x86_64 `.so` package — not only in the source tree, because the header is what LabVIEW imports to generate the wrapper VIs.

It is the **same file** for both targets: only fixed-width integer types and `double`, nothing platform-specific. One set of wrapper VIs therefore serves both, so the DLL can be tested on the desktop and the `.so` deployed to the cRIO with nothing regenerated. A header that had to differ per target would be a defect to report, not to ship.

---

## A7. Work required

1. **Code:** change the drain constant in `src/tempctl.c` to 0.5. Keep the first-tick charge exactly as implemented.
2. **Unit tests:** update the accumulator expectations, which currently pin the 1:1 behaviour. Cover the four duty points in the A2 table, plus a single-tick glitch (no failure) and 100 % duty (fails at 1 × E).
3. **TempSim:** update the flicker scenario's tick-exact expectations. Consider adding a second flicker scenario so both sides of the boundary are exercised — one at 50 % duty (fails) and one at 25 % duty (never fails, runs to the end of the scenario).
4. **Documents:** apply the amended R5.4 wording, the C11 row, the `ErrorTimeout` floor note and the header-packaging rule to spec v3.0.0, the capability copy, TESTING.md, the LabVIEW guide, the package guide and both changelogs.
5. **Packaging:** confirm the `.h` is present in both zips, and that it is byte-identical between them.
6. **Re-run the gates:** unit tests on x64, x86 and aarch64; the oracle; TempSim on Windows and arm64; the screenshot gate. Only the accumulator and flicker expectations should move — anything else changing is a signal worth investigating before release.

## A8. Still open (not part of this amendment)

1. **The seven smaller decisions** listed alongside the two issues have not been reviewed here. Send them for review before tagging.
2. **Confirm the one-tick grace survived** on the disagreement and relay-feedback countdowns. Only the accumulator was meant to lose it.
3. **The Import Shared Library wizard run** needs LabVIEW and stays with Scott.
4. **The cRIO x86_64 `.so` has never executed on a cRIO.** State this in the release notes.
5. Known coverage gaps in §11 of the handoff are unchanged: a physically stuck relay, a heater that never reaches the setpoint, a wrong-but-in-range survivor sensor, the single warning output, and no separate process over-temperature limit.
