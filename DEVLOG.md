# DEVLOG

Newest first. Format per spec section 151: IMPLEMENTED / FIXED / VALIDATED /
NEW DISCREPANCIES / PERFORMANCE / TEST RESULTS / NEXT PRIORITY.

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
