# Politics, manpower and occupation

Status: implemented, tested (`tests/test_politics.cpp`).
Confidence: MODERATE for political power and manpower; LOW for stability/war-support
drift targets and for resistance/compliance rates.

## Inputs

Controlled states and their populations, laws and their costs, war state, modifiers
(from technology and laws), occupation status per state.

## Rules

```
PP/day        = political_power_per_day * (1 + PoliticalPowerGain)
manpower/day  = Σ over controlled states of state population
                * recruitable_base * (1 + RecruitablePopulation) * (1 + ManpowerGrowth) / 365
manpower     -= template manpower when a division is recruited
consumer_goods_ratio = clamp(consumer_goods_base - 0.10 if at war, 0, 1)
stability    += drift toward 0.5 + modifiers, -0.10 while at war, -0.20 more if an
                enemy holds the capital, rate = stability_drift per day
war_support  += drift toward 0.5 + modifiers, +0.10 while at war
resistance   grows while a state is controlled by a non-core country, scaled by
             population; compliance grows slowly toward 1 - resistance;
             garrison_required derives from resistance and population
```

Laws change `Country::law_modifiers` (one additive fraction per law family) and cost
political power at enactment time; the cost check lives in command validation so a
rejected law never costs anything.

## Outputs

`Country::political_power`, `stability`, `war_support`, `manpower`,
`consumer_goods_ratio`, `law_levels`, `law_modifiers`, `State::resistance`,
`State::compliance`, `State::garrison_required`, and `politics`/`law` events.

## Failure states / edge cases

* zero political power: law commands are rejected with `InsufficientResources`
* zero manpower: recruitment is rejected; existing divisions keep fighting
* a country loses all states: manpower growth stops (no owned population)
* capital lost: stability drifts down faster and capitulation is checked daily
* occupied states do not contribute manpower to the occupier's own pool

## Known uncertainty

* Drift targets are heuristics, not reverse-engineered curves.
* National spirits and advisors do not exist yet, so their political-power and
  stability effects cannot be exercised (POL-003).
* Occupation policies and suppression are absent (OCC-003).
