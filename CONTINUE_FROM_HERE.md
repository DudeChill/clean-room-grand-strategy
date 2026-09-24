# CONTINUE FROM HERE

State of the project at the end of the session that shipped v0.7.0. Read this first when
resuming; then `DEVLOG.md` (top entry), `docs/PARITY_MATRIX.md` and
`docs/DISCREPANCIES.md`.

## Current build state

* Branch `master`, clean tree, pushed to `origin`
  (`https://github.com/DudeChill/clean-room-grand-strategy`).
* Releases published: v0.1.0 (foundation), v0.2.0 (air), v0.3.0/v0.3.1 (naval),
  v0.4.0 (focus trees and decisions), v0.5.0 (national spirits and advisers),
  v0.6.0 (trade, convoys, blockade), **v0.7.0 (equipment designers, scarce resources)**.
* Build directories in use: `build/` (main), plus per-slice dirs (`build-air/`,
  `build-navy/`, `build-pol/`, `build-spirit/`, `build-trade/`, `build-des/`, ...).
  Never build two agents into the same directory.

Last verified commands (all green on a clean Release build):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/hoi_tests                            # 239 passed, 0 failed
scripts/verify.sh                          # release gate
build/game --days 365 --audit --summary    # world audit: OK, 0 content warnings
build/game --days 30 --inspect-trade       # 9/10 countries in deficit, 12 routes
build/game --days 365 --inspect-country VEL  # designs owned and on production lines
```

* Performance (365-day run, 2275 provinces, 10 countries): 6.3 ms per simulated hour
  average, p50 0.8 ms, p95 19 ms, p99 62 ms. The `--quiet` report now prints an AI
  breakdown (`ai detail: industry ... military ... design ...`) so a regression can be
  attributed without guessing.

## What is implemented and verified

Deterministic tick, command queue with validation, hashing/save/replay; map, states,
regions, province graph; industry, construction, production lines, equipment, resources
and **scarcity-driven trade with convoys and blockades**; research and technology; manpower,
fuel, training; movement, land combat, fronts, battle plans, supply network, weather;
politics, laws, stability, **focus trees, decisions, national spirits, advisers**;
diplomacy, factions, wars, capitulation, peace, occupation; AI layers (industry, trade,
design, research, production, military, politics, diplomacy) that act only through the
command queue and record numeric `AiReason` factors; air warfare; naval warfare and
invasions; **equipment designers** (components, computed variants, ownership, AI, UI); and a
browser client that plays the whole game over `/api/state` + `/api/command`.

## Open work, in priority order

1. **MIL-021 (MAJOR, blocker for the equipment chain)**: division slots hold one exact
   `EquipmentId`, so a new model reaches new units only - existing divisions never
   retrofit, and the AI cannot re-point templates at its own designs. Fix: family/variant
   sets on `BattalionSlot` (or match on `EquipmentDef::archetype` with a preference order),
   family-aggregated demand in `compute_equipment_demand`, and an AI template upgrade step.
   Evidence and the design sketch are in `docs/DISCREPANCIES.md`.
2. **INT-001 (MAJOR)**: intelligence - agencies, networks, operations, decryption.
3. **MP-001 (MAJOR)**: multiplayer transport over the existing command/save layer.
4. **MOD-002/003 (MINOR)**: mod load order, content validator CLI, mod test pack.
5. Residual from v0.7.0: components exist for five categories only (support, anti-tank,
   anti-air, motorized, mechanized, convoy have none), and designers have no ship-hull
   module variety beyond the four slots.

## Conventions that keep this project honest

* Every claim needs a measurement: run the game, not just the tests. The three defects
  found in the designer slice (dropped `defense`/`breakthrough`, cross-country design
  theft, unparsed HTTP `components`) were all found by measuring, none by reading.
* Statuses are the fixed vocabulary from the brief (`NOT_IMPLEMENTED` ... `VALIDATED`).
  `VALIDATED` requires an evidence packet; nothing in this repo claims it without one.
* Balance numbers live in `data/common/*.json` or in named `constexpr` constants with a
  comment; never as literals in the middle of a formula.
* Determinism: ascending-id iteration, seeded streams per subsystem, no wall-clock or
  pointer-order dependence, and the world hash is the oracle.
