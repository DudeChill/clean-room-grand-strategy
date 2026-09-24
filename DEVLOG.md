# DEVLOG

Newest first. Format per spec section 151: IMPLEMENTED / FIXED / VALIDATED /
NEW DISCREPANCIES / PERFORMANCE / TEST RESULTS / NEXT PRIORITY.

## 2026-09-24 — v0.5.0 national spirits and political advisors (POL-006 closed)

IMPLEMENTED
* National spirits: permanent named modifier blocks with availability triggers, slot
  costs and optional effect blocks; they feed the same modifier stack as technology
  and laws and are granted by content through the script effect `add_national_spirit`.
* Political advisors: appointments costing political power, occupying advisor slots,
  granting modifiers until dismissed; the script effect `add_advisor` grants one.
* Raw grant entry points (`spirit_grant` / `advisor_grant`) so content effects grant
  without a trigger or cost while slot capacity stays an invariant that no path can
  break.
* AI: the politics layer appoints advisors and adopts spirits scored against posture
  (military modifiers at war, industry and research in peace), acting only through
  commands, with reasons recorded on `AiLayer::Politics`.
* Client: the Politics tab lists held spirits with free slots and the advisors in
  office, plus the available choices; `--inspect-country` reports both pools.
* Content: 10 spirits (5 trigger-gated, one costing two slots) and 8 advisors, with
  per-country starting spirits in the scenario for the two largest powers.
* Save format version 5 (spirits, advisors and their content tables serialized).

FIXED
* The script engine granted appointments through the command path, which charged
  political power and enforced content triggers; effects now use the grant variants,
  so a focus can hand out a spirit whose trigger does not pass while capacity stays
  enforced (found by cross-checking two agents' semantics against each other).

TEST RESULTS
* `hoi_tests`: 215 passed, 0 failed (16 new spirit/advisor tests).
* `scripts/verify.sh`: 8 checks passed, 0 failed.
* 400-day observer run: an AI country holds five spirits (including two trigger-gated
  ones) and three advisors, with both slot pools fully used, and the audit is clean.
* Client verified live: adopting a spirit through the panel changed state and slot
  accounting; advisor choices were correctly gated by political power.

NEW DISCREPANCIES
* Ideology drift, elections and coups remain unmodelled (a new POL-007 entry).

NEXT PRIORITY
* Trade and convoys as an economic system (ECON-001, MAJOR), then equipment designers.

## 2026-09-24 — v0.4.0 politics: script engine, focus trees, events, decisions (POL-001 closed)

IMPLEMENTED
* Script engine (src/sim/script.cpp, docs/mechanics/scripting.md): 30 trigger keys and
  19 effect keys over country/state scopes, comparators, boolean combinators,
  randomness on the events RNG stream, script variables, timed and permanent
  modifiers, chained and delayed events, claims, wars, laws and technologies — one
  vocabulary shared by every content type, so no engine code knows about a specific
  focus, event or decision.
* Focus trees (src/sim/focus.cpp): prerequisites, mutual exclusions, availability and
  bypass triggers, daily progress with a data-driven speed, completion applying the
  focus's effects through the script engine, AI scoring per available focus with
  recorded reasons on a new `AiLayer::Politics`.
* Events (src/sim/events.cpp): automatic firing per country per day, chained firing,
  delayed firing, `fire_only_once` bookkeeping, immediate and per-option effects, AI
  option choice by weight.
* Decisions: visibility and availability triggers, political-power cost, active timers
  with removal effects, cooldowns, state-targeted decisions, AI evaluation.
* Scripted modifiers: timed or permanent modifier blocks granted by content feed the
  same country modifier stack as technology and laws, tagged with their source so the
  inspector and client can attribute them.
* Content: three shared focus trees (industry, army, politics) plus country trees for
  the two largest powers (36 focuses), 12 events, 14 decisions, with cross-reference
  validation that names the file and key for every broken reference.
* Client: a Politics tab — focus trees with prerequisites and completion state, the
  active focus with progress and cancel, pending events with their options, decisions
  with cost and timers; plus politics detail in `--inspect-country`.
* Save format version 4 (section layout change) with the new state and content
  serialized and the constants drift guard at 112.

FIXED
* The client rejected a legitimate snapshot when `tick` was 0 (a paused, freshly
  started game) because it tested truthiness instead of the field's type; the whole
  politics panel was unreachable until the game was unpaused. Found by verifying the
  new UI against the live server.
* `add_claim` and state script flags were variable-key workarounds; they now use
  `State::core_owners` and a real `State::flags` field.
* The loader treated missing event/decision content files as hard errors, which broke
  minimal data sets (and three content tests); politics content is optional again.

TEST RESULTS
* `hoi_tests`: 202 passed, 0 failed (was 170): 16 script, 8 focus, 9 event/decision,
  10 persistence plus the existing suites.
* `scripts/verify.sh`: 8 checks passed, 0 failed.
* 365-day observer run: `world audit: OK`; the largest power completed five focuses,
  is working a sixth, holds six active decisions and carries modifiers granted by
  events and decisions.
* UI verified against the live server: 36 focuses rendered, a focus started from the
  panel and progressing, decisions listed with costs.

NEW DISCREPANCIES
* POL-006 (national spirits and advisors) remains open; ideology drift, elections and
  coups are not modelled.

NEXT PRIORITY
* National spirits and advisors (POL-006), then trade and convoys as an economic
  system, then equipment designers.

## 2026-09-23 — v0.3.1: AI invasion staging (release-artifact correction)

FIXED
* **Release artifact corrected.** The v0.3.0 tarball was packaged after an agent had
  landed additional AI staging code that was never covered by a test, so the artifact
  and the tag disagreed. v0.3.1 is cut from a single tree that is gate-verified as a
  whole, and the v0.3.0 release notes now point at it.
* AI invasion staging had three real defects, all found by writing the missing test
  (the reproduction is `ai_stages_an_inland_army_at_a_port_before_invading`):
  1. staging stopped as soon as one division reached the port, so the rest of the army
     was never marched;
  2. the army layer kept re-tasking staged divisions, pulling them back to the front
     (a `MoveDivision` clears the division's own order, so the guard now uses the
     army-level invasion order, which is durable);
  3. an army whose divisions were locked in a battle could be chosen for a landing and
     would then wait forever at the gather step — army selection now requires a free
     army, and a division already marching somewhere else is redirected to the port.
* Added `staged_division_was_pulled_back` to the test as the regression guard for (2):
  the failure it catches is invisible in the final state, since the army simply never
  crosses.

NEW DISCREPANCIES
* NAV-011: an engaged division assigned to a staging army before commitment can still
  stall the gather step; the AI now avoids committing such armies, and the race is
  recorded with its reproduction.

TEST RESULTS
* `hoi_tests`: 170 passed, 0 failed (one more than v0.3.0).
* Release gate re-run on the corrected tree: see `docs/reviews/v0.3.1.md`.

NEXT PRIORITY
* Focus trees (POL-001, last BLOCKER).

## 2026-09-23 — v0.3.0 naval warfare (NAV-001 and LOG-006 closed)

IMPLEMENTED
* Ships as entities (hull strength, crew organisation, experience, fuel, home port,
  sea zone), task forces with missions and sea zones, fleets as administration.
* Detection and engagement between hostile task forces in one sea zone, with
  positioning (destroyer screening of capitals, capital weight, carrier aircraft),
  guns against armour, torpedoes against large hulls, submarines penalised without
  sub detection, and simultaneous damage.
* Sinking through the real entity lifecycle, retreat to port when damaged or out of
  fuel, repair and refit in port at a fuel and spare-parts cost, sortie when fit.
* Missions: patrol, strike force, convoy escort, convoy raiding, invasion support,
  training, and stand-down.
* Naval control per sea zone derived from the ships present; convoy raiding cuts the
  victim's control and sinks convoy stock.
* Naval invasions: divisions gather at a port over the land graph, load, cross with
  real interception losses, land, and take control when the coast is undefended;
  every abort path resets the army and its divisions coherently.
* Ports as supply sources (LOG-006 closed): capacity scales with naval base level and
  sea route distance, is cut by enemy control of the zone, is zeroed by raider
  strength above a threshold, and draws convoys from the owner's stockpile — so
  raiding starves an overseas theatre through the ordinary supply graph.
* Naval content: destroyer, light and heavy cruiser, battleship, carrier, submarine,
  transport and convoy models with a 1936 naval technology tree, derived ports, and
  starting fleets for the two largest powers.
* AI naval layer: naval production demand per pool, fleet and task force formation
  sized to port capacity, mission assignment by posture and damage, invasion staging
  and launch gated on transports and naval control, with scored reasons.
* Client: Navy tab (fleets, task forces, missions, ship condition, naval control,
  invasion launch) and naval control colouring for sea zones.

FIXED
* IND-012 closed: ship and convoy lines now draw on dockyards, everything else on
  military factories, through one shared rule (`equipment_factory_pool`) used by
  command validation, the industry phase and the auditor.
* Scenario production lines now clamp per pool, so a naval power's ship lines can
  never exceed its dockyards.

PERFORMANCE
* Naval phase: 0.006 ms/tick in the shipped scenario, 0.023 ms/tick for a synthetic
  400-ship, 30-zone world. One year of AI war (336 divisions, 29 wars) costs 8.0 ms
  per simulated hour, of which the AI layers are the largest single cost (1.9 ms).

TEST RESULTS
* `hoi_tests`: 169 passed, 0 failed (19 naval, 7 invasion).
* `scripts/verify.sh`: 8 checks passed, 0 failed.
* 365-day observer run with fleets, air wings and invasions: `world audit: OK`.

NEW DISCREPANCIES
* NAV-008 (raiders counted only in the destination port's zone), NAV-009 (binary
  convoy gate, draw keyed to capacity rather than delivered throughput).

NEXT PRIORITY
* Focus trees (POL-001, the last BLOCKER), then events and decisions (POL-002), then
  trade and convoys as an economic system (ECON-001).

## 2026-09-23 — v0.2.0 air warfare (AIR-001 closed)

IMPLEMENTED
* Air wings as real entities: aircraft model, planes versus establishment, base,
  mission region, mission, efficiency, experience, cumulative losses.
* Missions: air superiority, interception, CAS, strategic bombing, logistics strike,
  reconnaissance.
* Air combat between hostile wings in a region with documented loss and experience
  models; losses become replacement demand drawn from the equipment stockpile.
* Air control per strategic region derived from the wings flying there, feeding land
  combat as an additive term (`BattleDebugLine::air_mod`, shown in the client battle
  detail).
* CAS damages enemy divisions in the region's battles; strategic bombing reduces the
  target state's factories; logistics strike cuts railway levels (and therefore supply
  capacity); anti-air levels reduce both.
* Wing displacement to the nearest friendly air base with capacity when a base is
  lost, destruction when none exists.
* Aircraft content: fighter/CAS/tactical-bomber archetypes with 1936 and 1940 models,
  an air technology tree, guaranteed capital air bases, starting wings for the two
  largest powers.
* Client: Air tab (form, mission, disband, air-control per contested region), an
  air-control map overlay, and the air term in the battle damage breakdown.
* AI air layer (on the military cadence): aircraft production demand, wing formation
  at the base nearest the operating region, superiority/CAS/interception assignment
  by posture, rebasing and disbanding lost or depleted wings, with scored reasons.
* 23 air tuning constants live in `data/common/constants.json` rather than in code.

FIXED
* Anti-air is a real building again (AIR-002 closed): `Province::anti_air` reduces
  bombing and logistics-strike damage and raises attacker losses over defended ground.
* `SimConstants` serialization fell behind the struct (60 of 78 fields): saves and
  `world_hash()` were reading past what was written, which segfaulted the hash oracle.
  All 78 fields are serialized in declaration order, with a test-time drift guard
  (a named table of every constant plus a count assertion) so it cannot recur silently.
* The AI production layer could not open an aircraft line at all, because aircraft
  need was derived only from divisions and templates never field aircraft; it now adds
  wing replacements plus a reserve, and its factory split uses largest-remainder
  allocation so it can never assign more factories than the country controls.
* `Store::create` auto-assigns a payload's `id`, `military_destroy_division` detaches
  from battles, and `military_prune_battles` closes the loop (all from the v0.1.0 gate
  work, re-verified here).

PERFORMANCE
* Air phase: 0.005 ms/tick in the shipped scenario, 0.033 ms/tick for a synthetic
  500-wing world. One year of AI war (484 divisions, 23 wars) costs 5.9 ms per
  simulated hour on average.

TEST RESULTS
* `hoi_tests`: 136 passed, 0 failed (24 of them air-specific).
* `scripts/verify.sh`: 8 checks passed, 0 failed.
* 365-day observer run with air: `world audit: OK`.

NEW DISCREPANCIES
* AIR-003 (no air detection model or radar effect), AIR-004 (no sortie fuel or pilot
  manpower), AIR-005 (reconnaissance has nothing to reveal without fog of war).

NEXT PRIORITY
* Naval warfare per `docs/mechanics/naval_warfare.md` (NAV-001, BLOCKER), then focus
  trees/events/decisions, then trade and convoys.

## 2026-09-23 — v0.1.0 release gate green

VALIDATED (with evidence packets under docs/evidence/)
* SIM-001 simulation clock and tick order, TOOL-002 determinism oracle,
  LND-005 land combat, IND-004 production efficiency.

FIXED (release-candidate round)
* `ai_production_layer` indexed a vector by `EquipmentDef::id`, which hand-built
  content had left invalid: an out-of-bounds write that only crashed at -O3. The
  upgrade target is now tracked by index; nothing in the AI indexes by a definition
  id any more.
* `military_destroy_division` destroyed a division without detaching it from its
  battle, leaving dangling ids that the auditor caught; `start_battle` could also add
  a division to both sides of the same battle. Both fixed, plus
  `military_prune_battles` called at the start of combat and the end of the territory
  phase so no battle can outlive its divisions.
* Wars could keep an empty side after a capitulation. Rosters are now kept as the
  historical record and a war ends when a side has no living participant;
  `phase_diplomacy` closes such wars defensively. Capitulation also clears the
  loser's production lines and construction queue (a defeated country owns no
  industry).
* `refresh_at_war` read `live.empty()` after `std::move(live)`, so every surviving
  participant of a still-active war silently lost `at_war` after any capitulation -
  which broke supply sharing, co-belligerence and AI war perception for the rest of
  that war.
* Production lines kept assignments for factories the country no longer controlled;
  the industry phase now releases the excess deterministically (last line first,
  retiring it exactly like a removal) and logs it once per retired line.
* Verification-gate defects: the inspector checks in `scripts/verify.sh` were
  malformed shell (they never ran), and the determinism comparison included timing
  lines that legitimately differ between runs.

PERFORMANCE
* Supply was 99% of tick time (43.4 ms/tick). A signature-keyed network cache plus a
  bounded, grouped multi-source search brought it to 1.2 ms/tick with **bit-identical
  results** (proved by digesting every province supply level, source, bottleneck and
  every division's supply/fuel before and after the change). Under full war load
  (449 divisions, 31 wars) a simulated hour costs 7.2 ms on average, p50 0.63 ms, and
  a year of game time runs in 54 s wall.

TEST RESULTS
* `hoi_tests`: 112 passed, 0 failed.
* `scripts/verify.sh`: 8 checks passed, 0 failed - build, tests, world audit,
  determinism, save/load round trip, 365-day observer run, three inspectors.
* 365-day observer run: `world audit: OK`, 31 wars, 449 divisions, 23,989 commands
  applied, civilian industry growing for every surviving country.

NEW DISCREPANCIES
* IND-012 (ship lines charge the military factory pool), AIR-002 (anti-air gated),
  plus the tuning note AI-009 (AI aggression in all-AI games).

NEXT PRIORITY
* Air warfare per `docs/mechanics/air_warfare.md` (AIR-001, BLOCKER), then naval
  warfare, then focus trees/events/decisions.

FIXED (verification round)
* `Division::id` was never populated at creation, so `detach_from_battle` could not
  find a retreating division and battles kept fighting phantom defenders forever -
  attackers never advanced and provinces never changed hands. Fixed at the storage
  layer: `Store::create` now assigns a payload's own `id` field via `if constexpr`,
  so no creation site can forget it.
* Saves were not self-contained: the loaded game had no content (equipment, techs,
  laws, buildings, constants), so a save played differently from the session that
  produced it. The Economy section now carries the content snapshot and rebuilds the
  derived key maps; the header also carries `start_date`, `ticks_run`,
  `ai_controlled` and `player_country` and cross-checks them against the sections.
* Scenario coverage: 313 of 332 states were unowned (an inert world with no industry
  and no supply sources). Every land state is now assigned to a country by a
  capacity-aware multi-source BFS with a smoothing pass; all ten countries hold 21+
  states and no land province is unowned.
* Construction: cost was exponential per level and capacity sat entirely on the head
  project, so nothing ever completed. Cost is now linear per level, and capacity
  spreads across queued projects (up to 15 factories each, the parity cap). First
  completion for the largest power lands around day 110, which is reference-like
  pacing for a 38-civilian-factory economy.
* Factory costs are differentiated in data (civilian 10800, military 7200, dockyard
  6400) instead of one cost for all three.
* `SimConstants::from_json` silently ignored three fields that had been added to the
  struct, so two synthetic-refinery rates and the per-project factory cap could not
  be tuned from data at all.
* Scenario capitals: the snapshot exposed the capital *state* id where the client
  expected a province, so "recenter" flew to a sea zone.
* Audit additions: battle membership consistency (battle side <-> division.battle),
  sea provinces exempt from state/owner checks, land adjacency must be land-only.

PERFORMANCE
* Measured on the shipped scenario: 52.7 ms/tick, of which supply is ~52 ms (99%).
  Every other phase sums to about 0.4 ms/tick. A supply-network cache plus a bounded
  search is being implemented; the target is <= 5 ms/tick.

NEW DISCREPANCIES
* AIR-002 (anti-air gated until air warfare exists), IND-011 closed for synthetic
  refineries (now produce oil and rubber through real state fields).

TEST RESULTS
* 100/102 tests pass on a clean build; the two remaining failures are the
  construction golden test (fixed by the linear cost model plus per-project factory
  allocation) and the save round-trip hash (missing `ai_controlled`,
  `player_country`, `start_date`, `ticks_run` - being added to the save/Ai section).

NEXT PRIORITY
* Land the three integration fixes above, re-run the full verification, publish
  v0.1.0, then implement air warfare against `docs/mechanics/air_warfare.md`.

## 2026-09-23 — foundation, map, economy, land warfare, logistics, AI, persistence

IMPLEMENTED
* Deterministic core: strong entity ids, entity store with stable slots, xoshiro256**
  RNG streams (combat/AI/events/weather/intel/map), FNV-1a hashing, dependency-free
  JSON parser/serializer, binary IO with bit-exact doubles, calendar/time helpers.
* Command system: 22 command types, pure validation + separate application, result
  recording, full serialization; AI issues commands through the same path.
* World model: provinces/states/regions with adjacency, terrain, resources, rails,
  hubs, victory points; countries with industry, research, laws, stockpiles, armies.
* Map pipeline: `tools/genmap` generates a deterministic grid world (land + sea,
  states, regions, terrain, resources, hubs) into `data/maps/world.json`; scenario
  loading builds countries, ownership, starting industry, technologies and armies.
* Economy: resource yields and consumption, production lines with efficiency and
  efficiency cap growth, equipment switching with retention, construction queue
  consuming civilian industry, consumer goods, fuel, equipment stockpiles.
* Research: prerequisites, year-based cost, slot progress, unlock propagation into
  equipment availability and country modifiers.
* Military: division templates with composition-derived statistics, division state
  (organisation, strength, equipment, experience, entrenchment, planning, supply,
  fuel), movement over the province graph, incremental combat with per-division
  damage breakdown, reinforcement into combat width, retreat legality, destruction
  when surrounded, organisation recovery, weather attrition.
* Territory: control transfer by occupation, state controller aggregation,
  construction cancellation on capture, capitulation triggers.
* Logistics: supply sources (capital + hubs), capacity from rail/infrastructure,
  graph propagation with distance falloff, per-province supply level and bottleneck,
  emergent encirclement, per-division supply and fuel distribution.
* Training: off-map divisions consuming manpower and equipment, readiness gating,
  deployment via command.
* Politics/weather: political power, manpower growth, laws, stability and war
  support drift, consumer goods ratio, per-region weather with movement/combat
  effects.
* Diplomacy: war creation with faction and guarantee propagation, co-belligerence,
  capitulation, territorial transfer, peace offers, occupation resistance and
  compliance.
* AI: five layers (industry, research, production, military, diplomacy) that plan on
  intervals and issue real commands, with scored reasons recorded for the debugger.
* Persistence: versioned sectioned save files, per-subsystem hashes, world hash,
  command-log replay, hash verification on load with first-mismatch reporting.
* Server + client: dependency-free HTTP server exposing static map data, world
  snapshots and a command endpoint; browser client with canvas map, overlays
  (political/control/supply/terrain/fronts), production/construction/research/
  military/diplomacy/log panels, time controls and unit orders.

FIXED
* Duplicate `OrderKind` definition in `world.h` (reported by three slices).
* `Store<T>` id mismatch: payload types now map to their strongly-typed ids through
  an `EntityTag` trait, plus a free-list/alive bookkeeping bug.
* Production-line efficiency retention was applied twice (commands + industry);
  single owner is now the industry phase, commands only record the switch.

VALIDATED
* See `docs/PARITY_MATRIX.md`; promotions require an evidence packet under
  `docs/evidence/`.

NEW DISCREPANCIES
* Tracked in `docs/DISCREPANCIES.md` (air, naval, focus trees, trade convoys,
  intelligence and multiplayer are absent by design at this milestone).

TEST RESULTS
* Unit, golden, determinism and save round-trip suites: see `docs/reviews/` for the
  latest recorded run.

NEXT PRIORITY
* Air warfare and naval warfare simulations (spec sections 51-55), then focus trees
  and events/decisions, then modding load order and validation.
