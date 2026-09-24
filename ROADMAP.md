# ROADMAP

Milestones are quality gates, not dates. A milestone is reached when every listed
exit criterion is demonstrably true (tests, evidence packets, audits), not when
time has passed.

## 0.1 Foundation (reached)

* deterministic headless simulation, fixed tick, RNG streams, world hashing
* command system with validation, logging and replay
* entity store with stable ids and id-ordered iteration
* JSON content loading, binary save/load with versioning
* unit tests for core primitives

## 0.2 Map & Countries (reached)

* province/state/region graph from generated map data
* adjacency-driven pathfinding, front detection, encirclement detection
* scenario loading: countries, ownership, industry, armies, factions
* world auditor and invariant checks

## 0.3 Economy & Industry (in progress)

* resource yields and consumption feeding production lines
* production lines with efficiency, efficiency cap and equipment switching
* construction queue with industrial allocation to completion
* research slots with prerequisites, costs, unlocks and modifier propagation
* stockpiles feeding division reinforcement and training
* trade, fuel and consumer goods modelled from law/war state

## 0.4 Land Warfare (in progress)

* division templates whose stats emerge from battalion composition
* incremental combat: organisation, strength, soft/hard attack, defence,
  breakthrough, armour, piercing, combat width, reinforcement, terrain, weather
* retreat with physical legality, destruction when surrounded
* fronts, offensive lines, fallback lines, planning accumulation
* combat debugger showing every modifier contribution

## 0.5 Logistics (in progress)

* supply sources (capital, hubs) with rail/infrastructure capacity
* graph propagation over controlled territory with distance falloff
* emergent encirclement, supply debugger with route and bottleneck
* fuel production, storage and consumption by motorised/armoured formations

## 0.6 Air & Naval (reached in v0.2.0 and v0.3.0)

Air:

* air wings, bases, range, missions, sorties, losses and replacements
* air control per region feeding land combat, CAS, strategic bombing, logistics
  strike
* anti-air defence; AI air layer; client air panel and overlay

Naval:

* ships, task forces, fleets, detection, positioning, screening, gun/torpedo/carrier
  combat, sinking, retreat and repair
* missions: patrol, strike force, convoy escort and raiding, invasion support,
  training
* ports as supply sources with convoys and blockade; naval invasions end to end
* AI naval layer; client navy panel and sea-zone control colouring

## 0.7 Politics, Diplomacy & Occupation (focus trees in progress)

Politics scripting (in progress, v0.4.0):

* data-driven script engine: triggers, effects, scopes, variables, timers
* national focus trees: prerequisites, exclusions, bypass, effects, AI weighting
* events with options and chains; decisions with cost, timers, targeting
* timed and scripted modifiers feeding the country modifier stack

Earlier in this milestone:

* political power, laws, stability, war support, national focuses
* events and decisions (data-driven scripting)
* resistance, compliance, garrison requirements, occupation policies
* peace conferences operating on real territorial control

## 0.8 AI (in progress)

* layered AI: industry, research, production, military, diplomacy
* reasons reported per decision, no direct state mutation, no free resources
* recovery behaviour under encirclement, shortages and two-front wars

## 0.9 Modding, Multiplayer, Tooling (not started)

* mod load order, data validation, mod test pack
* multiplayer determinism and desync tooling
* map/data editors

## 1.0 Feature-Complete Core Release

Exit criteria: `docs/PARITY_MATRIX.md` shows every core system at FUNCTIONAL or
better with no BLOCKER/MAJOR discrepancies; golden tests, determinism test, save
round-trip test and world audit pass; a full campaign can be played end to end
through the production client without developer commands.
