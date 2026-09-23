# AI scoring model

Status: implemented, tested (`tests/test_ai.cpp`).
Confidence: HIGH for the mechanism (commands, reasons, no state writes), LOW for the
weights - they are tuning data, and every decision records the factors that produced
it so a bad choice can be diagnosed from the snapshot rather than guessed at.

## Mechanism

* Five layers (Industry, Research, Production, Military, Diplomacy) run on their own
  intervals (default daily; military twice daily). Each layer is a plain function over
  `Game&` plus the AI country.
* Every layer pushes `Command`s into the normal command queue. The AI never mutates
  world state, never receives information a player could not have, and never gets a
  resource a player would not get.
* Each decision appends an `AiReason` (a name, a score, and a factor list) to the
  layer's reason log, which is served to the client and readable in the snapshot.
* Posture (peace / defensive / offensive) is recomputed per country and gates
  aggression.

## Industry layer

Candidate score = base (civilian factory 45, military factory 40, infrastructure 25,
supply hub 15, fort 12) plus situational terms: `+25` at war for military factories,
`+15` in peace for civilian ones, `+30 * clamp(demand/(stock+1), 0, 2)` for equipment
deficits, `+25` for supply distance to the front, `+25` when the capital is at risk,
minus `8 * fort_level` for existing forts. Candidates below 20 are dropped.

## Research layer

`category` weight (industry 35, infantry 30, armour 30, artillery 22 …), plus
`+25` when it unlocks an equipment the country is short of, plus `+15` when it unlocks
equipment a template fields, minus 20 per year ahead of time, plus an economy/combat
weighting over the technology's modifiers multiplied by 1.5 during war.

## Production layer

Need per model (from division templates, stockpile and losses) plus a category bonus
and `+12` when a template fields the model. A model switch requires a statistic gain
of at least 5% that also beats 50% of the efficiency retention value, so the AI does
not thrash production lines.

## Military layer

```
offensive_score = 40*clamp(force_ratio - 0.5, 0, 1.5) + 20*supply + 10*front_present
                  - 40*capital_threat
posture         = offensive when offensive_score >= 45 and force_ratio >= 1.15,
                  fallback when force_ratio < 0.75 or capital_threat >= 1 or
                  occupied share > 30%, otherwise defensive
recruit target  = clamp(4 + 2*military_factories + 2*front_provinces, 4, 80), x1.5 at war
```

## Diplomacy layer

```
faction_join = 20*ideology_match + 35*threat + 10*(1 - force_ratio) + 10*strong_patron
threat       = clamp01(capital_threat*0.5 + occupied_share*2 + war_pressure)
declare_war  = 40*clamp01(force_ratio - 1) + 20*(1 - target_strength_share)
               + 20*offensive - 25*own_threat
gates        = target-specific force ratio (own strength vs the target plus its
               faction) >= 1.5, a shared reachable border, an offensive posture, and
               at most one declaration per country per 30 days (the cooldown is read
               from the command log, so it counts declarations rather than only
               active wars)
threshold    = 45
peace        = 60*max(enemy_taken_share, own_lost_share), offered when the engine's
               decisive-settlement rule is met
```

## Military layer (execution)

```
attacks per run      = 6, movement orders per run = 8, local attack ratio >= 1.0
attack score         = 20 + 40*clamp01(ratio - 1) + 60 if the target is undefended
armies               = up to 24, scaled by division count
```

Army orders alone do not move anything: the layer issues explicit `MoveDivision`
commands along the computed path, and rear divisions are assigned to the thinnest
part of the front line so an enemy cannot simply walk around a defended border.

## Failure behaviour

* A rejected command is recorded in the command log with its rejection reason; the AI
  re-evaluates next interval rather than retrying blindly.
* Force-ratio and threat terms react to encirclement, lost capitals and occupation,
  which makes a collapsing front switch posture to fallback without any scripted
  branch per country.

## Known uncertainty

* Weights are heuristics, not reverse-engineered values.
* No dedicated recovery planning beyond posture (AI-008); no naval or air layers
  because those systems do not exist yet (AI-007).