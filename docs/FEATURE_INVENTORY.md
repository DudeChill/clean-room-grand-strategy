# FEATURE INVENTORY

Every identified gameplay capability, deeply hierarchical. This is the discovery
list; `docs/PARITY_MATRIX.md` records the status of each entry and the evidence for
it. Newly discovered reference behaviour is added here first (spec section 8), never
silently dropped.

Legend: `[x]` implemented and covered by a parity record, `[~]` partial, `[ ]` not
implemented.

## 1. Simulation

* [x] Fixed simulation clock (1 tick = 1 game hour, 24/day)
* [x] Deterministic RNG streams (combat, AI, events, weather, intel, map)
* [x] Command pipeline (validate -> apply -> log -> replay)
* [x] Entity lifecycle (create, reference, destroy) for countries/divisions/armies/
      battles/wars
* [x] Subsystem hashing and world hash
* [x] Headless execution (`--headless`, default)
* [ ] Variable game speed as a *player* setting influencing tick pacing from the CLI
      (implemented in the server client, not from the command line)
* [ ] Replay playback mode (log is recorded and loadable; a driven playback command
      is not exposed)

## 2. Map and territory

* [x] Provinces with terrain, infrastructure, population, victory points
* [x] Adjacency graph (authoritative for movement, supply, fronts, encirclement)
* [x] States (ownership, cores, factories, building slots, manpower pool)
* [x] Strategic regions (weather grouping)
* [x] Sea zones, coastlines, naval bases (data present; naval gameplay not yet)
* [~] Rivers (crossing penalty modelled via marsh proxy; river entities not modelled)
* [x] Railways and supply hubs as infrastructure entities
* [x] Air bases (data + construction; air gameplay not yet)
* [x] Owner vs. controller distinction with occupation
* [x] Resource yields per province aggregated per state
* [x] Victory points (data + garrison order targets; capitulation uses factories)
* [ ] Radar, anti-air, synthetic refineries (buildable entries exist; effects not
      simulated)
* [ ] Straits, canals, impassable terrain beyond a flag

## 3. Country

* [x] Tags, names, ideology, capital, overlord/puppet relations
* [x] Political power, stability, war support
* [x] Laws (conscription, economy, trade, training families) with costs and modifiers
* [x] Manpower pool with population-derived growth and recruitment consumption
* [x] Fuel production, storage, capacity and consumption
* [x] Equipment stockpiles per model
* [x] Modifier scopes (base, technology, law, national) summed per country
* [x] Consumer goods ratio
* [ ] National spirits, advisors, government composition
* [ ] Stability/war-support inputs beyond war state and modifiers

## 4. Industry

* [x] Civilian, military and dockyard factory counts per state
* [x] Construction queue with industrial allocation, level scaling and completion
* [x] Capacity spread across projects (up to a per-project factory cap, like the
      reference game) instead of all capacity on the head project
* [x] Building slots limiting state-wide construction
* [x] Production lines with assignment validated against controlled factories
* [x] Production efficiency with growth, cap growth and equipment-switch retention
* [x] Resource demand per line with deterministic allocation order and shortage
      factors
* [x] Consumer goods share reducing available industry
* [x] Synthetic refineries producing oil and rubber from no imports
* [x] Repair of damaged buildings (queue flag exists; bombing does not yet damage)
* [ ] Trade (resource imports/exports), convoys and blockade effects
* [ ] Equipment designers (tank/aircraft/ship variants with components)
* [ ] Factory conversion and dismantling
* [ ] Synthetic refineries producing oil/rubber

## 5. Research

* [x] Technology graph with prerequisites and year gating
* [x] Research slots with progress and completion
* [x] Unlocks gating equipment production
* [x] Modifier effects propagating to country and divisions
* [ ] Ahead-of-time penalty as a documented, tested rule (implemented and tunable,
      confidence MODERATE)
* [ ] Research bonus categories (industry/doctrine/electronics specialisations)
* [ ] Doctrines as a distinct progression with army/navy/air trees

## 6. Land warfare

* [x] Division templates with battalions and support companies
* [x] Statistics derived from composition and equipment actually present
* [x] Division state: organisation, strength, equipment, manpower, experience,
      entrenchment, planning, supply, fuel
* [x] Training queue with manpower and equipment consumption and readiness gating
* [x] Deployment to a province
* [x] Movement over the province graph with terrain/weather/infrastructure cost
* [x] Incremental combat: soft/hard attack, defence, breakthrough, armour, piercing,
      hardness, combat width, reinforcement, terrain, weather, experience, planning,
      commander, supply
* [x] Organisation and strength damage, equipment and manpower losses
* [x] Retreat with legal destination or destruction
* [x] Encirclement emerging from supply connectivity
* [x] Front lines computed from adjacency and war state
* [x] Offensive orders with target lines and planning accumulation
* [x] Fallback lines, garrison orders
* [x] Combat debugger with per-division modifier breakdown
* [ ] Battle plan execution automation (plans are drawn and prepared; execution is
      still per-division movement orders)
* [ ] Combat tactics (tactic selection and counters)
* [ ] Commander traits, skills growing with experience
* [ ] Strategic redeployment, paradrops, naval invasions
* [ ] Attrition beyond weather (terrain and out-of-supply attrition exist via supply
      and weather; explicit attrition modifiers are not modelled)

## 7. Logistics

* [x] Supply sources: capital and supply hubs
* [x] Route propagation over controlled territory (Dijkstra per source)
* [x] Capacity from railway level and infrastructure
* [x] Distance falloff with hub radius and range penalty
* [x] Per-province supply level, source and bottleneck
* [x] Per-division supply and fuel distribution
* [x] Supply debugger (route, per-step capacity, delivered amount, bottleneck)
* [x] Organisation recovery and movement penalties driven by supply
* [ ] Ports and overseas supply
* [ ] Convoys and naval interdiction of supply
* [ ] Motorisation of supply distribution (army motorisation level is stored and
      settable; it does not yet affect the network)
* [ ] Rail damage and repair

## 8. Air warfare

* [ ] Air wings as entities with base, model, mission, region
* [ ] Missions: superiority, interception, CAS, strategic bombing, logistics strike,
      naval strike, port strike, recon, transport
* [ ] Sorties, detection, interception, mission efficiency
* [ ] Air superiority affecting land combat and movement
* [ ] Aircraft losses and replacement
* [ ] Air bases with capacity and range

## 9. Naval warfare

* [ ] Ships as entities with classes, components and statistics
* [ ] Task forces and fleets with missions
* [ ] Sea regions, detection, visibility, positioning, screening
* [ ] Convoy raiding/escort, patrol, strike force, minelaying/minesweeping
* [ ] Naval invasion workflow (transport, landing, supply)
* [ ] Ports, repair, naval range and fuel

## 10. Politics, events and focus

* [x] Political power accumulation and spending on laws
* [x] Laws changing modifiers and consumer goods
* [ ] Focus trees (prerequisites, exclusions, bypass, effects, AI weighting)
* [ ] Events (triggers, options, effects, chains, scopes)
* [ ] Decisions (visibility, cost, timers, targeting, completion, AI evaluation)
* [ ] National spirits and government/advisor slots
* [ ] Ideology drift, elections and coups

## 11. Diplomacy and war

* [x] Relations with drift
* [x] Factions with leaders, membership, joining/leaving and leader succession
* [x] Guarantees and military access (flags that propagate on war declaration)
* [x] War declaration with coalitions (puppets, overlords, faction members,
      guarantors)
* [x] War entities with participants, goals and casualties
* [x] Capitulation and territorial transfer
* [x] Peace offers that end wars and move territory according to control
* [ ] Non-aggression pacts, lend-lease, volunteers, expeditionary forces
* [ ] Peace conference with multiple claimants and score allocation
* [ ] War goals beyond annex/puppet/state claim (liberation, demilitarisation)

## 12. Occupation

* [x] Owner vs. controller with occupation of enemy provinces
* [x] Resistance and compliance growth with garrison requirement
* [x] Occupation transferring state control when all provinces agree
* [ ] Occupation policies and their effect on resistance
* [ ] Garrison division assignment and suppression
* [ ] Sabotage and revolt events

## 13. Intelligence

* [ ] Agencies, networks, operatives, counterintelligence, cryptology, operations

## 14. AI

* [x] Layered architecture (industry, research, production, military, diplomacy)
* [x] Acts exclusively through the command system
* [x] Scored decisions with recorded reasons
* [x] Construction, research, production decisions
* [x] Recruitment, deployment, army creation, front/offensive orders, movement
* [x] Diplomacy: faction joining, war declarations under favourable force ratios,
      peace offers
* [x] No state mutation outside commands, no free resources
* [ ] Naval and air layers (depend on those systems)
* [ ] Explicit recovery behaviour for encirclement/fuel/equipment crises beyond the
      posture scoring

## 15. Persistence and determinism

* [x] Versioned save files with per-subsystem hashes
* [x] Load verification reporting the first mismatching subsystem
* [x] Command log persistence and replay metadata
* [x] Determinism oracle (world hash) with a same-seed test
* [ ] Save migration paths between versions (rejection path implemented, migration
      table is a documented stub)
* [ ] Autosave rotation and named slots

## 16. Modding

* [~] All content loads from JSON (countries, map, equipment, technologies, laws,
      buildings, templates)
* [ ] Deterministic mod load order and override rules
* [ ] Mod test pack proving add-country/add-focus/add-event/add-decision without
      recompiling
* [ ] Data validator reporting missing/duplicate ids and broken references (basic
      validation exists; no standalone validator tool)

## 17. Multiplayer

* [ ] Command-stream architecture is networkable (commands serialize) but no network
      transport, no lockstep, no desync tooling

## 18. UI / UX

* [x] Browser client: canvas map with political/control/supply/terrain/front overlays
* [x] Production, construction, research, military, diplomacy and log panels
* [x] Division selection, movement orders, training and deployment
* [x] Army creation, front/offensive/fallback/garrison orders, stance, motorisation
* [x] Alerts for idle research slots, unassigned factories, unassigned divisions,
      ready divisions, resource shortages
* [x] Tooltips with province ownership, control, supply and garrison information
* [x] Time controls (pause, speeds 1-5) and in-game save
* [x] Battle detail window consuming the combat debugger
* [x] Supply route visualisation for the selected province (route, capacity per step,
      bottleneck, per-division supply/fuel)
* [x] Hotkeys (space, 1-5, tab letters, arrow-key panning, escape to clear selection)
* [~] Division selection: click to select, shift-click to assign to the first army
* [ ] Selection groups, multi-select and order queues
* [ ] Accessibility: UI scale, colour-blind palettes, remappable controls

## 19. Tooling and diagnostics

* [x] `--headless` observer runs with summaries, per-tick metrics and audit
* [x] World auditor with invariant violation reports
* [x] `--hashes` subsystem hash report
* [x] Save/load round-trip stress mode (`--save-every-days`)
* [x] AI decisions carry numeric reasons in the snapshot
* [x] Inspectors: `--inspect-country`, `--inspect-province`, `--inspect-supply`,
      `--inspect-battle`
* [x] Release gate script (`scripts/verify.sh`) and stress ladder
      (`scripts/stress.sh` with `docs/benchmarks/history.tsv`)
* [ ] Profiler captures and benchmark history beyond the tick-time series
* [ ] Map/data editor tooling
