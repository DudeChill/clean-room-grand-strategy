# SIM-001 evidence — simulation clock and tick order

## What was tested

* `tests/test_core.cpp`: calendar arithmetic, tick/date round trips over 1900-2000.
* `tests/golden.cpp GOLDEN_012`: 90 days of simulation on a hand-built two-country
  world with AI on both sides; asserts invariants and finiteness of division state.
* `build/game --days 365 --audit --summary`: full scenario, all countries AI-played.

## What happened

* 365 simulated days = 8,760 ticks execute in one pass with no invariant violations
  and no non-finite values; the auditor reports `world audit: OK`.
* `Game::tick_once` performs the twelve phases in the documented order and advances
  `World::tick`/`World::date` exactly once per call; `run_ticks(n)` executes exactly
  `n` ticks regardless of wall-clock time.

## How we know it works

* The date printed after a run equals the start date plus the simulated duration.
* Two runs with the same seed and no commands produce identical `world_hash`, which
  can only happen if the phase order and tick accounting are deterministic.

## Edge cases tested

* month/year rollover, leap years (1900 not leap, 2000 leap)
* non-zero start hour in tick/date conversion
* zero-length runs (`--days 0`) and single-tick runs

## What remains different

Sub-hour phases do not exist (KD-003); game-speed control is a client feature, not a
CLI one (SIM-006).