# DEVLOG

Newest first. Format per spec section 151: IMPLEMENTED / FIXED / VALIDATED /
NEW DISCREPANCIES / PERFORMANCE / TEST RESULTS / NEXT PRIORITY.

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
