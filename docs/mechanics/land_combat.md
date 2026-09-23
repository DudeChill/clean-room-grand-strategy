# Land combat

Status: implemented, tested (`tests/test_military.cpp`, `GOLDEN_004`, `GOLDEN_006`).
Confidence: MODERATE. Incremental organisation/strength combat, combat width,
reinforcement, terrain and armour interactions are documented behaviour; the exact
damage constants are tunable.

## Inputs

Attacker and defender division sets for one province, plus terrain, weather, supply,
entrenchment, planning, experience, commander statistics and combat width.

## Rules (per hour, both directions simultaneously)

```
attack(side)   = Σ over engaged divisions of
                 (soft_attack*(1 - enemy_hardness) + hard_attack*enemy_hardness)
                 * (1 + DivisionAttack + planning_bonus + experience + commander)
                 * supply_factor * organization_factor
defense(side)  = Σ over engaged divisions of
                 (defense | breakthrough) * (1 + DivisionDefense) * supply_factor
mitigation     = defense / (defense + attack)
damage         = attack * damage_scale * (1 - mitigation)
org_damage     = damage * org_damage_share
strength_damage= damage * strength_damage_share / hp_scale
planning_bonus = planning * planning_max_attack_bonus   (attacker only)
```

Armour: when the side's armour exceeds the enemy's piercing, that side deals `1.5x`
and takes `0.5x`; when it is less than half the enemy's piercing, the reverse
(shadowed by a single `RNG_COMBAT::chance()` roll per side per battle so results stay
reproducible).

Engagement: divisions enter combat only while the side's used width fits the
terrain's combat width; the rest wait as reserves and rotate in by ascending
division id. At least one division per side always fights.

## Outputs

Organisation and strength losses, equipment and manpower losses, battle progress,
debug lines (`BattleDebugLine`) with every contribution for the debugger, retreat or
destruction, province control transfer on attacker victory.

## Failure states / edge cases

* organisation reaches 0: the division leaves the battle; if no legal retreat
  province exists (all neighbours enemy-controlled or occupied by hostile troops),
  it is destroyed.
* battle in a cut-off province: supply 0, no organisation recovery, attrition from
  weather; attackers keep their supply advantage.
* attacker with no path into the province: stays at the edge, battle opens from
  adjacency.
* province changes controller mid-battle: constructions there are cancelled and
  supply caches invalidated.
* a division can never end a tick with negative organisation or non-finite strength
  (asserted by invariants).

## Known uncertainty

* Tactics (combat tactic selection) are not implemented.
* Commander traits beyond flat statistics are not implemented.
* Air support into combat awaits the air warfare milestone.
