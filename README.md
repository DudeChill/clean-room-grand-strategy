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

Current milestone: **0.4 Land Warfare / 0.5 Logistics (work in progress)**.

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

Headless observer run (AI plays every country):

```sh
build/game --days 365 --summary --audit --hashes
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
