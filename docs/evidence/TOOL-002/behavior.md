# TOOL-002 evidence — determinism oracle

## What was tested

* `tests/golden.cpp GOLDEN_009_determinism_same_seed_same_hash`: two identical
  hand-built worlds, both countries AI-controlled, same seed (99), 30 simulated days,
  then `world_hash` compared.
* `tests/golden.cpp GOLDEN_010_save_load_continues_identically`: save, load into a
  fresh `Game`, compare `world_hash`, then run 10 more days in both and compare again.
* `build/game --days 20 --save-every-days 10`: periodic save/load round trips inside
  a real scenario run, each verified by hash equality.
* `build/game --days N --hashes`: per-subsystem hashes for cross-run comparison.

## What happened

* AI-driven runs with identical seeds produce identical hashes; a divergence would
  fail loudly with the world hashes printed side by side.
* Save/load preserves every subsystem hash, and continued simulation after a load
  stays hash-identical to the uninterrupted run.

## How we know it works

`world_hash` folds the canonical binary serialization of every subsystem in a fixed
order, so hash equality is state equality, not a sample. The persistence test suite
additionally mutates one field at a time (242 cases) and asserts the hash changes -
proving the hash actually covers the state rather than ignoring it.

## Edge cases tested

* corruption detection: flipping a byte in a section payload fails the load and names
  the affected subsystem
* version rejection: an unknown save version is refused with both versions in the
  message

## What remains different

Cross-platform bit-identical runs are not guaranteed for arbitrary libm versions
(KD-004); the hash detects such drift instead of hiding it.