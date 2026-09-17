# Cover note: TempCtl v3.0.0 implementation decisions for review (Amendment A, A8.1)

To: Scott
Date: 2026-09-18
Status: DRAFT, not sent

Amendment A is applied (drain 0.5, first-tick charge confirmed, `ErrorTimeout`
floor documented, header in the package) and every gate was re-run: 1516
unit checks on x64, x86 and the Raspberry Pi, the cantools oracle, 16
simulator scenarios with 110 tick-exact expectations on Windows and arm64
with byte-identical outputs, the screenshot gate. Only the accumulator and
flicker expectations moved.

A8.1 asks for the remaining implementation decisions to be reviewed before
tagging. They are section 12 of `TEMPCTL-SPEC-v3.0.0.md` in the package;
here they are in one place. Each is pinned by a unit test, so a change is a
one-line rule change plus its test.

| # | Decision taken | Where the handoff left room |
|---|---|---|
| I3 | While control is paused (active sensor out of range, R5.7) the **status code holds** with the relays: `HeaterON` stays `HeaterON`, `HeatPending` stays `HeatPending`. | R5.7 says relays and countdowns hold; it does not name the status. |
| I4 | The two-sensor comparison is evaluated **before** the control step of a tick, so it first qualifies on the tick **after** `Initial_HC_Flag` is set by a relay release. One tick of delay, once. | R6.2 lists the gate conditions, not the evaluation order within a tick. |
| I5 | Before the first `TcCheckTemp`, `TcInit` and `TcReset` report **`TempAtSetPt`** for a valid enabled zone (or `HeaterON` / `CoolerON` when relays were kept across a re-Init, `TempCtrlDisabled`, `ConfigFault`). The first `TcCheckTemp` corrects it. | No state code exists for "no reading yet"; 6-9 are reserved. |
| I6 | A timeout that truncates to 0 ms (for example 0.5) **fails the config check**; the smallest accepted value is 1 ms, which still needs a later tick to expire. | Section 3 says "> 0" and "fractions truncated" without saying which is checked first. |
| I7 | A failed sensor's accumulator is **frozen** at the failure value, so `TempxOorAccumMs` stays at `ErrorTimeout` while the sensor is failed instead of draining back toward zero. | R5.3 says a failed sensor is not re-evaluated; the accumulator's display value is not specified. |
| I8 | A sensor that is out of range on its **first tick after Init or Reset counts one** `OorEventsPerHour` transition. | R5.6 counts in-range to out-of-range transitions; there is no "previous" state right after Init. |
| I9 | The warning frozen on a fault tick is the one **computed on that tick** (for example `Temp2OutOfRange` when sensor 2's excursion caused `BothSensorsFailed`), not the previous tick's. | R9.6 says the warning "freezes at its last value". |

Two more points from Amendment A, both confirmed in the tests:

- **A8.2** The one-tick grace survived on the deadband, at-setpoint,
  disagreement and relay-feedback countdowns: each test shows the observing
  tick with the full countdown remaining and expiry on a later tick. Only
  the accumulator charges from its first tick.
- **A8.4** The cRIO x86_64 `.so` has still never executed on a cRIO; the
  release notes say so.

Reply with "agreed" or the numbers you want changed.
