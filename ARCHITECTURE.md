# ARCHITECTURE

Authoritative technical contract for the clean-room grand-strategy engine in this
repository. Code that contradicts this document is a bug in the code, or the
document is updated in the same change.

## 1. Layers

```
player input / AI  ->  Command  ->  validation  ->  simulation phases
                                                       |
                                                  WorldState
                                        +--------------+---------------+
                                        |              |               |
                                      Save          Hash/Audit      UI snapshot
```

* The simulation is authoritative and headless (`game --headless`).
* The UI and the network layer are observers; they never own gameplay state.
* One piece of state has exactly one owner (see `docs/STATE_OWNERSHIP.md`).

## 2. Time

* 1 tick = 1 game hour. 24 ticks/day. `Tick` is a monotonically increasing `uint64`.
* Rendering frame rate never influences simulation: `Game::run_ticks(n)` advances
  exactly `n` hours regardless of wall-clock time.
* Game speed only changes how many ticks are executed per real second.

## 3. Tick order (fixed)

| # | Phase | File | Responsibility |
|---|-------|------|----------------|
| 1 | commands | `sim/commands.cpp` | apply queued player/AI commands, record results |
| 2 | diplomacy | `sim/diplomacy.cpp` | relation drift, war upkeep, capitulation checks |
| 3 | movement | `sim/movement.cpp` | division transit across province edges |
| 4 | combat | `sim/combat.cpp` | start battles, org/strength damage, retreats |
| 5 | territory | `sim/territory.cpp` | control transfer, occupation, capitulation effects |
| 6 | supply | `sim/supply.cpp` | supply network flow -> per-province and per-division supply |
| 6b | air | `sim/air.cpp` | sorties, air combat, missions, losses, air control per region |
| 6c | naval | `sim/navy.cpp`, `sim/invasion.cpp` | detection, engagements, missions, invasions, naval control |
| 7 | industry | `sim/industry.cpp` | resources, production lines, construction, stockpile |
| 8 | research | `sim/research.cpp` | research progress, tech effects |
| 9 | politics | `sim/politics.cpp` | PP, laws, stability, war support, manpower |
| 10 | weather | `sim/weather.cpp` | per-region weather |
| 11 | AI | `sim/ai/ai.cpp` | planning; pushes commands for the next tick |
| 12 | cleanup | `game/game.cpp` | destroy finished battles/dead entities, audit, metrics |

Rules:

* Phases never iterate containers whose order depends on pointer values or hash
  seeds. Entity iteration is always ascending id via `Store::for_each`.
* `std::map` is used where ordered iteration is required; `std::unordered_map` is
  never iterated for gameplay decisions.
* No phase reads a value that a later phase in the same tick wrote, except where
  the table above defines the dependency (supply before industry is intentional:
  divisions consume what logistics delivered this tick).

## 4. Determinism

* All randomness comes from `RngSet` streams: `RNG_COMBAT`, `RNG_AI`, `RNG_EVENTS`,
  `RNG_WEATHER`, `RNG_INTEL`, `RNG_MAP`. Never `rand()`, never time, never address
  values, never iteration order of unordered containers.
* Doubles are compared and hashed bit-exactly (`ByteWriter::f64` writes IEEE bits).
* Cross-platform note: identical compiler/libm on the same architecture is assumed
  for bit-identical runs. Hashes detect any drift; see `docs/KNOWN_DIFFERENCES.md`.
* `world_hash(g)` is the determinism oracle. Two runs with the same seed, scenario
  and command stream must produce identical hashes.

## 5. Formulas

Constants come from `data/common/constants.json` (`SimConstants`). Percentages are
additive fractions summed per scope and applied as `(1 + sum)`.

### 5.1 Industry

```
factory_ic(f)          = 4.5 IC/day per military factory (military), 5.0 civilian, 2.5 dockyard
line_hourly_output     = factories * ic_per_factory/24 * efficiency * (1 + FactoryOutput mods)
resource_factor(line)  = min over required resources r of clamp(available[r] / required[r], floor, 1)
unit_cost(def)         = def.build_cost * (1 + EquipmentCostFactor)
units_per_hour         = line_hourly_output * resource_factor / unit_cost
efficiency_growth      = (cap - eff) * efficiency_growth_per_day / 24
efficiency_cap_growth  = +efficiency_cap_growth_per_day/24 while producing, cap <= 1.0
switch to same archetype: eff *= 0.7 (retention), cap *= 0.7
switch to other archetype: eff = 0.10, cap = 0.50
```

Resource allocation is deterministic: countries in ascending id, lines in
`Country::lines` order, each line takes what it needs until resources are gone.

### 5.2 Construction

```
civ_output_per_hour = civilian_factories * ic_per_civilian/24 * (1 + ConstructionSpeed)
                     * (1 - consumer_goods_ratio)
project cost        = base_cost * (1 + (construction_level_scaling - 1) * existing_level)
```

Cost grows linearly with the levels already present in the target (1.0, 1.25, 1.5, …
for `construction_level_scaling = 1.25`). An exponential curve was tried first and
made late levels in large states unreachable within a campaign. The cost is stamped
onto the project when the command is accepted, so `phase_industry` never recomputes
it; data (`BuildingDef::base_cost`) wins over the constants table.

Projects progress in queue order with the capacity left after consumer goods. Each
project may draw at most `max_factories_per_project` civilian factories (15 in data),
so a full queue builds several things at once instead of everything on the head
project — this is what makes construction visible inside a campaign. On completion the
building level increases in the target state/province and the project is removed.

### 5.3 Research

```
days_needed = tech.cost_days * (1 + ahead_of_time_penalty) / (1 + ResearchSpeed)
progress   += research_speed_per_hour * 24/24
```

`ahead_of_time_penalty = 0.5 * max(0, tech.year - current_year)` (documented as
MODERATE confidence, tunable).

### 5.4 Movement

```
terrain_cost(province) = data table (plains 1.0, forest 1.5, hills 1.6, mountain 3.0,
                                     urban 1.4, marsh 2.0, desert 1.1, jungle 2.5)
hours_per_edge = base_hours_per_province / max(min_speed, division_speed) * terrain_cost(target)
                 * weather_cost * (1 + 0.3 if river crossing)
                 / (1 + 0.05 * infrastructure)
move_progress += 1/hours_per_edge per tick; arrival at >= 1.0
```

Divisions with no valid path stay put. Hostile-controlled provinces may only be
entered when the division has an offensive order (or the province is undefended and
the country is at war with its controller).

### 5.5 Combat

Battles are per province and incremental (hourly). Sides: attackers (divisions in
adjacent provinces with attack orders or hostile presence), defenders (divisions in
the province). Combat width caps how many divisions fight; the rest are reserves
and rotate in as width frees up.

```
attack(side)     = sum over engaged divisions of
                   (soft_attack * (1 - enemy_hardness) + hard_attack * enemy_hardness)
                   * (1 + DivisionAttack + planning_bonus + experience + commander)
                   * supply_factor * organization_factor
defense(side)    = sum over engaged divisions of
                   (defense if defending else breakthrough)
                   * (1 + DivisionDefense) * supply_factor
mitigation       = defense / (defense + attack)             # 0..1
raw_damage       = attack * damage_scale * (1 - mitigation)
org_damage       = raw_damage * org_damage_share / max_org_scale
strength_damage  = raw_damage * strength_damage_share / hp_scale
armor: if side_armor > enemy_piercing -> damage *1.5, incoming *0.5; if
       side_armor < enemy_piercing * 0.5 -> damage *0.5, incoming *1.5
planning_bonus   = planning * planning_max_attack_bonus (planning grows while an
                   offensive plan prepares, reset on execution)
```

A division whose organisation reaches 0 leaves the battle (retreat or destruction
when no legal retreat province exists). When all defenders leave, the attacker takes
control of the province in phase 5 and the battle ends.

### 5.6 Supply

```
sources     = controlled capital province + controlled supply hubs of the country
capacity(s) = 10 * (1 + 0.15 * railway_level) * (1 + 0.05 * infrastructure)   [per source]
propagation = multi-source Dijkstra over land edges of provinces controlled by the
              country (or allies), edge cost = 1 + distance penalty
delivered(p)= max over sources of capacity(s) / (1 + supply_range_penalty * hops)
supply_level= clamp(delivered / demand, 0, 1)
division.supply = supply_level(location), further reduced by distance from the
                  nearest source beyond supply_hub_radius
```

Encirclement is emergent: a province with no path to a source gets 0 supply, and
cut-off divisions stop recovering organisation and take attrition.

### 5.7 Politics

```
PP/day       = political_power_per_day * (1 + PoliticalPowerGain)
manpower/day = sum(controlled states population) * recruitable * (1 + ManpowerGrowth) / 365
stability    += drift toward target from war state, consumer goods, laws
consumer_goods_ratio = consumer_goods_base + law effects - war effects (min 0.0)
```

### 5.8 Weather

Per strategic region, re-evaluated daily: temperature from latitude proxy + season,
then `RNG_WEATHER` decides rain/snow/mud/sandstorm using terrain and temperature.
Effects: movement cost multiplier, combat attack penalty, air (later) and attrition.

### 5.9 Air

```
base capacity(w, province) = province.air_base * air_base_capacity_per_level
wing planes               += replacements drawn from the country's stockpile
sorties(w)                = planes * efficiency / sortie_hours
air combat(w, e)          = attack(w) * (1 - air_defence_share(e)) * agility(w) factor
                            (one documented RNG_COMBAT draw per engagement)
losses(w)                 = damage / durability
efficiency(w)             = clamp01(planes / max_planes)                              (degrades with losses)
air_control(region, c)    = sum over c's wings flying there of planes * efficiency * w(mission)
                            divided by the same total for all countries present
air_support(province)     = CAS term (attackers only) + superiority term (both sides),
                            clamped, consumed by land combat as BattleDebugLine::air_mod
```

`phase_air` runs after `phase_supply` and before `phase_industry`, so aircraft losses
become equipment demand in the same tick's industry step. Land combat reads the
previous hour's air control (combat runs before air in the tick order) — that is
intentional and documented at the call site.

### 5.10 Naval

```
detection(tf)      = sum over ships of detection * (1 - enemy_visibility) * weather
engagement         = both sides detect each other in the same sea zone, one documented
                     RNG_COMBAT draw per pair
positioning(side)  = screening (destroyers) + capital weight + carrier aircraft,
                     modified by detection quality
damage             = guns vs armour, torpedoes vs large hulls, carrier aircraft using
                     the air model's attack values
sinking            = strength 0 -> ship destroyed, crew lost
retreat            = average task-force strength below a threshold -> return to port
repair             = in port, strength recovers over time
naval_control(region, c) = sum over c's task forces there of ship weight * mission
                     weight, divided by the same total for all countries present
```

Ports also act as supply sources (see 5.6): a port contributes capacity scaled by its
naval base level and its sea route, reduced by enemy naval control in that zone, and
draws convoys from the owner's stockpile while it operates. Convoy raiding therefore
starves an overseas theatre through the ordinary supply graph rather than through a
special case.

### 5.10a Equipment variants and replacement

A battalion slot names one equipment *family*, not one exact model:

* `slot_family` maps a slot's equipment to its family (`infantry_equipment` for
  `infantry_equipment_1`, or the archetype's own key when the slot names an archetype).
* `equipment_fits_slot` admits any model of that family the country may field -
  researched, or one of its own designs. Locked models and other countries' designs are
  refused, so a variant can never leak across a border.
* `preferred_slot_model` decides which member a country issues, in this order:
  (1) the member on a live production line that fits - a country fields what it builds;
  (2) otherwise the fieldable member with stock, highest `equipment_stat_score` first;
  (3) otherwise the highest-scoring fieldable member; (4) otherwise none. Rule 1 makes
  the choice independent of the order in which phases run, which is what stops demand
  from oscillating between an old and a new model while a line ramps up.
* `phase_reinforcement` (after `phase_industry`, before the AI) is the replacement
  mechanism: a division below full strength draws
  `max(missing * 0.02, 0.25) * supply` per hour of the model the country issues for each
  of its slot families, falling back to the family's stocked model when the preferred
  model has nothing in the depot this hour. Supply gates it, so a cut-off division does
  not refill - that is what makes encirclement bite. It adds no state: the save format and
  the world hash layout are unchanged.
* The training queue draws the same way (family-grouped, with the same fallback), so new
  divisions and existing ones agree on what the army is equipped with.
* Strength and manpower follow the definition actually consumed, as they always have; a
  division may therefore hold several models of one family at once, and its template's
  requirement is enforced against the family total.
* Rounding: identical IEEE-754 double rules as every other formula here; drawing is
  clamped to the missing amount, so no path can over-fill a slot.

### 5.11 Equipment designs

A design is an equipment archetype plus a fitted component set. The rule is a pure
function of data:

```
design(base, components) = for each fitted component, ascending slot order:
                             stat += component.stat            (every additive field)
                             resources[r] += component.resources[r]
                             cost_add += component.build_cost_add
                             cost_mult *= component.cost_multiplier
                           build_cost = (base.build_cost + cost_add) * cost_mult
                           reliability = clamp(base.reliability + ΣΔ, 0.1, 1.0)
                           every other statistic = max(0, value)   (NaN/Inf rejected)
```

Rules that keep it honest:

* The result is registered as a real `EquipmentDef` (`is_archetype = false`) in
  `Content::equipment` — production lines, stockpiles, division equipment, combat
  and the client read it exactly like an authored model. A design is never a
  modifier applied to the archetype.
* `design_compute` is pure; `design_create` validates first and commits atomically
  (no partial state on failure), so an invalid command cannot corrupt content.
* Keys are deterministic: `"<tag>_<category>_<n>"` with the smallest unused `n`.
* Availability is checked per component: scenario year, the component's optional
  `available` trigger, and any technology naming the component key in
  `unlock_equipment`. Stats never gate; only availability does.
* The AI buys capability for money: it accepts a fitted variant when capability gain
  over the best model it can currently build clears `kDesignMinGainRatio` and the
  cost increase stays inside its affordable margin — the reasoning a player uses.
* Rounding: the same IEEE-754 double rules as every other formula in section 5, and
  results are sanitised (`finite_or`, clamps) so a hostile component set cannot
  produce NaN or negative statistics.

## 6. Data formats

```
data/common/constants.json    SimConstants overrides
data/common/equipment.json    equipment archetypes and models
data/common/technologies.json technologies with prerequisites, unlocks, modifiers
data/common/laws.json         laws with cost and modifiers
data/common/buildings.json    building kinds with base costs and max levels
data/maps/<map>.json          provinces, adjacency, terrain, resources, states, regions
data/scenarios/<name>.json    countries, starting industry, armies, diplomacy, wars
```

Map file schema:

```json
{
  "name": "world",
  "regions": [{"key": "r1", "name": "Region 1", "is_sea": false}],
  "states": [{"key": "s1", "name": "State 1", "region": "r1", "building_slots": 6}],
  "provinces": [
    {"key": "p1", "name": "Province 1", "state": "s1", "region": "r1",
     "terrain": "plains", "adj": ["p2", "p3"], "coastal": false, "is_sea": false,
     "victory_points": 0, "infrastructure": 3, "population": 250000,
     "resources": {"steel": 4}}
  ]
}
```

Scenario schema:

```json
{
  "name": "1936", "map": "maps/world.json", "start_date": "1936-01-01",
  "countries": [
    {"tag": "ALB", "name": "Albania", "ideology": "neutrality", "capital_state": "s1",
     "states": ["s1", "s2"], "civilian_factories": 3, "military_factories": 1,
     "technologies": ["infantry_weapons"], "stockpile": {"infantry_equipment_1": 2000},
     "divisions": [{"template": "infantry_division", "province": "p1", "count": 3}]}
  ],
  "wars": [], "factions": []
}
```

Templates live in `data/common/templates.json` keyed by name; scenario divisions
reference them.

## 7. Commands

Every gameplay action is a `Command` (see `sim/commands.h`). Commands are validated
(pure, no mutation) before being applied, are serialized into the command log, and
are the exact mechanism the AI uses. The command log plus the initial state and seed
reproduce a run exactly (see `save_replay`).

## 8. Save / hashing

* `save_game` writes: magic, version, header (seed, scenario, tick, date), one
  section per `Subsystem` with tag + byte length + FNV-1a hash, then `world_hash`.
* `load_game` rejects unknown versions, verifies per-section hashes, and reports the
  first mismatching subsystem - that report is the desync diagnostic (spec §88).
* Section content is defined by `serialize_subsystem`; adding a field requires
  bumping `SAVE_VERSION` and adding a migration path.

## 9. AI

Layered: Industry, Research, Production, Military, Diplomacy. Each layer runs on its
own interval (default daily) and issues commands. The AI:

* sees only information a player could see (its own state + world-level public state);
* never mutates state directly;
* records `AiReason` entries (score + factors) for the debugger.

## 10. Testing

* `hoi_tests` runs unit, integration, golden, determinism and save/load tests.
* Golden tests are end-to-end scenarios (train a division, take a province, starve a
  cut-off army) that must keep working.
* The determinism test runs the same scenario twice and compares `world_hash`.
* The save round-trip test compares hashes before/after save+load.
