# Air warfare — specification

Status: **SPECIFIED, NOT IMPLEMENTED** (discrepancy AIR-001, BLOCKER).
Confidence: MODERATE for the model shape (wings, bases, regions, missions, sorties,
detection, losses), LOW for the numeric constants, which will be data.

This document is the contract for the next milestone. It exists so the air system is
built against a specification rather than invented during implementation, and so its
absences are visible instead of implied.

## Inputs

* Air equipment models from content (fighter, CAS, tactical bomber, naval bomber,
  transport) with range, speed, agility, air attack, air defence, ground attack,
  reliability, cost and resource draw.
* Air base levels per province (`Province::air_base`) with capacity per level.
* Strategic regions (`RegionId`) as the operational space, with the weather already
  modelled per region.
* Country state: equipment stockpile, industry (to build aircraft), fuel, manpower.
* Enemy presence: divisions and provinces inside each region, for CAS and
  interdiction targets.

## State

```
AirWing {
  id, country, equipment (model), planes, max_planes,
  base (ProvinceId), region (RegionId), mission (AirMission),
  efficiency (0..1), experience, losses_total, last_sortie_tick
}
Region {
  ...existing weather...
  air_superiority[owner]  -> derived, not stored authoritatively
}
```

Superiority is derived each tick from the wings flying in the region, so it can never
disagree with the wings (no second source of truth).

## Missions

| Mission | Effect |
|---|---|
| Air superiority | contests the region; supplies the superiority value used by CAS/interdiction and by land combat |
| Interception | engages enemy wings flying in the region |
| CAS | adds ground attack into friendly land battles in the region |
| Strategic bombing | damages state buildings (industry, infrastructure) |
| Logistics strike | damages railways and supply hubs, reducing supply capacity |
| Naval strike / port strike | engages ships and ports (requires the naval milestone) |
| Reconnaissance | reveals enemy divisions in the region to the owner |
| Transport / paradrop | moves a division (requires the invasion milestone) |

## Rules (per hour)

```
sortie_rate(w)      = planes * efficiency / sortie_hours
detection(w)        = sum over enemy wings in region of planes * detection_profile(rng)
air_combat(w, e)    = attack(w) * (1 - defence_share(e)) * randomness(RNG_COMBAT)
losses(w)           = damage / aircraft_durability(w), applied to planes
efficiency(w)       = clamp01(planes / max_planes) * (1 - 0.5 * enemy_superiority_share)
superiority(region) = friendly_planes / (friendly_planes + enemy_planes), weighted by
                      efficiency, per side; 0.5 when nobody flies
cas_bonus(battle)   = cas_effect * cas_planes / (cas_planes + enemy_interceptors)
```

Land combat consumes `cas_bonus` as an additive attack modifier in
`compute_side_values` and a defensive malus for the side without air cover; this is
the dependency that makes air power matter without a fake country-wide modifier.

## Integration points

* `phase_air` runs after `phase_supply` and before `phase_industry`: losses become
  equipment demand in the same tick's industry step.
* `phase_combat` reads region superiority and CAS presence.
* `phase_industry` builds aircraft through normal production lines; a wing draws
  replacements from the stockpile.
* `phase_movement` is unaffected this milestone (bombing movement penalties come with
  railway damage, LOG-008).
* AI: an air layer mirroring the naval layer — build the highest-value airframe,
  create wings near the front, assign missions by posture (superiority when
  contested, CAS when attacking, interception when defending), rotate depleted wings.

## Commands

```
CreateAirWing   (country, province=base, equipment=model, value=size in planes)
DeployAirWing   (wing, province=base)
SetAirMission   (wing, region, value=AirMission)
DisbandAirWing  (wing)
```

All validated like every other command: base controlled by the country, air base has
capacity, model unlocked and available in stockpile.

## Edge cases

* base captured: the wing is displaced to the nearest friendly air base or destroyed
  when none exists
* zero planes: the wing is removed and its personnel (manpower) returned
* no fuel: sorties stop; wings stay in place and still defend their own base
* region without any base in range: missions are rejected at command time, not
  silently ignored
* save/load: wings, bases, missions, losses and experience all serialize

## Tests (to be written with the implementation)

* `GOLDEN-AIR-1`: a wing flying superiority in a region raises the land-combat attack
  of a friendly division fighting there, and the enemy suffers the inverse.
* `GOLDEN-AIR-2`: air combat produces plane losses on both sides, and the losses
  appear as equipment demand the next day.
* `GOLDEN-AIR-3`: strategic bombing reduces a state's factory count, and the
  causality test (destroy the wing) removes the effect.
* Determinism: two identical runs with wings flying produce identical world hashes.

## Explicit non-goals for the first pass

* Individual aircraft as entities (wings are the unit of simulation; planes are
  numbers inside a wing).
* Pilot aces and named squadrons.
* Air supply drops (depends on logistics milestone LOG-006).