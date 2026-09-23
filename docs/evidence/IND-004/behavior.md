# IND-004 evidence — production efficiency and equipment switching

## What was tested

* `tests/test_economy.cpp`: efficiency grows toward the cap and never exceeds it; a
  resource shortage reduces line output monotonically; switching equipment applies
  the retention rule exactly once (through `apply_command` + `phase_industry`, not by
  calling the helper directly); two identical worlds produce identical stockpiles.
* `tests/golden.cpp GOLDEN_001_production_reaches_stockpile`: assigning five factories
  to infantry equipment makes the stockpile grow over 30 simulated days, with
  efficiency above its starting value and a non-zero cumulative output.
* `build/game --days 365 --audit`: no invariant reports an out-of-range efficiency.

## What happened

* `efficiency` starts at `efficiency_start` (0.10) and approaches
  `efficiency_cap` (0.50) at `efficiency_growth_per_day`; the cap itself rises while
  the line keeps producing.
* Removing a line pushes the outgoing model onto `previous`; re-assigning that model
  (or one of the same archetype) makes the industry phase apply the documented
  retention factor once and clear the pending-switch entry.
* Resource shortage multiplies output by the available fraction, floored by
  `resource_shortage_floor`, and is recorded on the line for the UI.

## How we know it works

The double-application bug found during development (commands and industry both
applying retention) is covered by the contract that only `phase_industry` mutates
efficiency on a switch; the test asserts the resulting efficiency directly rather than
asserting that a helper was called.

## Edge cases tested

* zero factories assigned (no output, no cap growth)
* zero resources available (output multiplied by the floor, never divided by zero)
* switching between archetypes (reset to start/base) and within an archetype
  (retention factor)
* line removed while producing

## What remains different

Exact growth/retention constants are approximations (MODERATE confidence);
ahead-of-time research cost and consumer goods interact with output by design (KD-007).