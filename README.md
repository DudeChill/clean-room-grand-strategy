# Clean Room Grand Strategy

A from-scratch, clean-room grand-strategy engine built to reproduce the **observable
mechanics, workflows and scale** of a Hearts of Iron IV-style game: division-based
land warfare, an industrial production simulation, a physical supply network,
research that unlocks real equipment, fronts and battle plans, and an AI that plays
by the same rules as the player.

No proprietary code, assets, prose or data are used. Map, countries, equipment,
technologies and scenarios are original content generated or authored in this
repository, and every mechanic is implemented from observed behaviour and public
documentation.

## Status

Honest, evidence-based status lives in `docs/PARITY_MATRIX.md`. There is no
percentage-complete figure anywhere in this repository: only per-feature states
(`NOT_IMPLEMENTED` … `VALIDATED`) with the evidence that justifies each one.

Current milestone: **0.6 Air and Naval warfare reached** (land, economy, logistics,
air, naval) — focus trees are the last remaining blocker.

Measured on the shipped scenario (2,275 provinces, 332 states, 10 countries, air
wings, fleets and ~80 divisions at start, all AI-controlled): `world audit: OK`,
~8.0 ms per simulated hour in a full year of AI war (336 divisions, 29 wars), and
identical world hashes across repeated runs with the same seed. Re-run it yourself:

```sh
build/game --days 3 --audit --summary --hashes
```

## Build

Requires g++ 12+ (C++20) and CMake 3.20+. No third-party libraries, no network
access at build or run time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
build/hoi_tests          # unit + golden + determinism + save/load tests
```

## Play

Browser client (the playable surface):

```sh
build/game --serve --port 8080 --player VEL --autosave-days 30
# then open http://127.0.0.1:8080
```

The simulation starts paused; press `1`-`5` to set game speed, space to pause. `VEL`
(Veldoria) is the strongest power in the shipped scenario; any tag from
`data/scenarios/1936.json` works.

What you can do in the client:

* **Map**: click a province for terrain, ownership, control, supply and garrison;
  switch overlays (political, control, supply, terrain, fronts); drag to pan, wheel
  to zoom; your own border is outlined in white.
* **Production**: assign military factories to equipment models, watch efficiency
  and resource satisfaction, see the stockpile.
* **Construction**: queue civilian/military factories, dockyards, infrastructure,
  railways, supply hubs, air/naval bases, radar, forts and synthetic refineries;
  each project shows progress against its cost.
* **Research**: fill research slots from the available technologies; completed
  technologies unlock equipment and modifiers.
* **Military**: train divisions from templates, deploy them, form armies, assign
  generals, set front/offensive/fallback/garrison orders, stances and motorisation.
  Click a division to select it, then click a province to order a move.
* **Air**: form wings from stockpiled aircraft at air bases, assign air superiority,
  interception, CAS, strategic bombing, logistics strike or reconnaissance over a
  strategic region, watch air control per region, and see the air term in the battle
  damage breakdown.
* **Navy**: form task forces from ships in your stockpile at a port, give them a
  mission (patrol, strike force, convoy escort, convoy raiding, invasion support,
  training), watch naval control per sea zone, and launch naval invasions from a port
  onto a hostile coast.
* **Diplomacy**: declare war, join a faction, enact laws; watch wars, factions and
  country strength.
* **Details**: selecting a province shows the live battle breakdown (per division
  organisation, strength, planning, entrenchment, and the last tick's damage
  decomposition) and the supply route with per-step capacity and the bottleneck.

Headless observer run (AI plays every country):

```sh
build/game --days 365 --summary --audit --hashes
```

Inspectors answer "why is this happening" from the authoritative state:

```sh
build/game --days 30 --inspect-country VEL      # budgets, lines, queue, research, stockpile
build/game --days 30 --inspect-province 42      # terrain, control, supply, garrison
build/game --days 30 --inspect-supply 42        # supply route with per-step capacity
build/game --days 30 --inspect-battle 3         # battle breakdown with per-division damage
```

Stress ladder and long-run observer automation:

```sh
scripts/stress.sh        # tiers of increasing duration; writes docs/benchmarks/history.tsv
```

Persistence round trip and deterministic verification:

```sh
build/game --days 30 --save-every-days 10 --audit        # save/load every 10 days, hashes must match
build/game --days 30 --hashes                            # subsystem hashes for determinism comparison
```

## Layout

```
src/core/     ids, entity store, RNG streams, hashing, JSON, binary IO, logging, time
src/data/     content database + scenario/map loading (everything historical is data)
src/sim/      world state, commands, phases: diplomacy, movement, combat, territory,
              supply, industry, research, training, politics, weather, AI
src/save/     versioned save/load, subsystem hashing, replays
src/game/     Game aggregate, tick loop, auditor, HTTP server
tools/        genmap (deterministic map generator)
tests/        unit, integration, golden, determinism and persistence tests
web/          browser client (pure observer over the JSON API)
data/         constants, equipment, technologies, laws, buildings, maps, scenarios
docs/         parity matrix, discrepancies, mechanics research, workflows, reviews
```

## Design rules

* The simulation is authoritative and headless; UI and network are observers.
* Every gameplay mutation is a `Command` - validated, serializable, replayable, and
  issued by the AI through the same path the player uses.
* Fixed tick (1 tick = 1 game hour); rendering never influences simulation.
* Deterministic: explicit RNG streams, id-ordered iteration, no hash-order gameplay.
* Hard hashing (`world_hash`, per-subsystem hashes) is the determinism oracle and
  the save/load proof.
* Content is data. Engine code contains no country-specific branches.

See `ARCHITECTURE.md` for the tick order, formulas and data formats, and
`docs/` for parity tracking.

## License and content

MIT (see `LICENSE`). The engine, the client, the generated map and all content are
original work: no proprietary source, art, audio, text or data tables from any
commercial title are used. Every known mechanical approximation is listed in
`docs/DISCREPANCIES.md`, and every feature's status and evidence in
`docs/PARITY_MATRIX.md`.
