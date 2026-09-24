# CONTINUE FROM HERE

State of the project at the end of the session that shipped v0.3.0. Read this first
when resuming; then read `DEVLOG.md` (top entry) and `docs/PARITY_MATRIX.md`.

## Current build state

* Branch `master`, clean tree, pushed to `origin`
  (`https://github.com/DudeChill/clean-room-grand-strategy`).
* Releases published: **v0.1.0** (foundation), **v0.2.0** (air), **v0.3.0** (naval),
  **v0.3.1** (verified AI invasion staging; also the corrected naval artifact — use this
  for the naval milestone, v0.3.0's tarball was packaged after an in-flight edit).
* Last verified commands (all green on a clean Release build):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/hoi_tests                 # 170 passed, 0 failed
scripts/verify.sh               # 8 checks passed, 0 failed
build/game --days 365 --audit --summary    # world audit: OK, 29 wars, 336 divisions
```

* Performance: 8.0 ms per simulated hour in a full war year; supply ~2.4 ms,
  AI ~1.9 ms, naval 0.006 ms, air 0.005 ms per tick.

## What is implemented and verified

Simulation core (deterministic tick, commands, hashing, saves, replay), map/states/
regions, industry, construction, production lines, resources, research, equipment,
manpower, fuel, training, movement, land combat, fronts and battle plans, supply
network, weather, politics/laws/stability, diplomacy/factions/wars/capitulation/
peace/occupation, AI (industry, research, production, military, air, naval,
diplomacy), air warfare (wings, missions, air control, CAS, bombing, anti-air),
naval warfare (ships, task forces, detection, engagements, missions, invasions) and
overseas supply with convoys and blockade. Browser client covers all of it.

## Active task at the moment of stopping

**Implement focus trees (POL-001 — the last BLOCKER).** Nothing is half-written: the
tree is clean and every milestone is published, so this starts from a green baseline.

## Next concrete implementation steps

1. **Script engine first** (`docs/mechanics/` has no file for it yet; write
   `docs/mechanics/scripting.md` as the spec before coding):
   * a small trigger/effect evaluator over `Game`/`World` with scopes
     (`country`, `state`, `province`, `war`), conditions (owns, at_war, has_tech,
     pp/stability/war_support comparisons, date/year), and effects
     (add_political_power, add_stability, set_law, grant_tech, declare_war,
     add_opinion, create_state_claim, unlock_equipment, add_modifier, complete_focus,
     trigger_event);
   * everything data-driven: no per-focus C++.
2. **Focus trees**: `FocusDef` (key, name, icon slot, x/y, prerequisites,
   mutual exclusions, days, available/finished/bypass triggers, effects), loaded from
   `data/common/focuses/*.json`; `Country::focus_progress`, `completed_focuses`,
   `selected_focus`; commands `SelectFocus` / `CancelFocus`; completion in a politics
   phase step; AI weighting per focus with scored reasons.
3. **Events and decisions (POL-002/003)** on the same script engine: event definitions
   with triggers, options, effects, `trigger_event` chains, decisions with visibility,
   cost, timers, targeting and AI evaluation.
4. **Persistence**: focuses, events fired, decisions taken, script variables — extend
   the save slice and the constants/field drift guard (see the note below).
5. **Client**: focus tree view (nodes, prerequisites, progress, select), event popups
   with options, decision list.
6. **Tests**: golden tests for a focus chain completing and applying effects, an event
   firing from a decision, a bypass path, and determinism.

## Traps and conventions that cost time this session

* **One writer per build directory.** Concurrent `cmake --build build` from several
  agents produced reproducible "optimization-sensitive" segfaults that were pure
  build artifacts. Agents must use their own build dirs; `scripts/verify.sh` owns
  `build/`.
* **Every denormalised id must be set where the entity is created.** `Store::create`
  now auto-assigns a payload's `id` via `if constexpr`; do the same for anything not
  in a Store (content definitions set their own ids in the loader).
* **Single owner for every rule.** Efficiency retention, the factory pool rule and
  the construction cost model each live in exactly one place; duplicating a rule
  produced real double-application bugs.
* **Tunable numbers live in data.** `SimConstants` (111 fields) + `constants.json`;
  the save serializer has a drift guard (`static_assert` and a named table) that must
  be updated with any new field.
* **Allowed status vocabulary** in the parity matrix only (spec section 10); no
  invented statuses, no percentages.
* **Anticipate mid-edit trees**: when a peer's file does not compile, check
  `g++ -std=c++20 -Isrc -fsyntax-only <file>` and wait rather than assuming UB.
* **Never package a release while an agent is mid-edit.** v0.3.0's tarball was cut
  after an edit landed and before it was tested, which produced an artifact that did not
  match its tag and shipped untested code. Release steps now: stop all agents, freeze the
  tree, run `scripts/verify.sh`, then package, and verify the artifact against the tag
  (`git show <tag>:<file> | sha256sum` versus the file inside the tarball).

## Known gaps (all tracked in docs/DISCREPANCIES.md)

BLOCKER: POL-001 focus trees. MAJOR: events/decisions, national spirits, trade and
convoys as an economy, equipment designers, intelligence, multiplayer transport.
MODERATE/MINOR: battle plan execution without micromanagement, combat tactics,
strategic redeployment, motorisation effect, rail damage, air detection model, air
fuel/pilots, reconnaissance, raider zone coverage, convoy depletion granularity,
mod load order and validator, tooling and UI polish.
