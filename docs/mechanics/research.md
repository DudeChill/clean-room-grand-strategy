# Research

Status: implemented, tested (`tests/test_economy.cpp`, `GOLDEN_007`).
Confidence: MODERATE. Slots, prerequisite gating, year-based cost and unlock
propagation are documented behaviour; the ahead-of-time penalty constant is an
approximation.

## Inputs

`Content::techs` (key, category, year, cost in days, prerequisites, unlocked
equipment and buildings, modifiers), `Country::research.slots` and
`slots_unlocked`, completed technology list, research-speed modifiers.

## Rules

```
available(tech)  = not completed and every prerequisite completed
days_needed      = tech.cost_days * (1 + ahead_of_time) / (1 + ResearchSpeed)
ahead_of_time    = 0.5 * max(0, tech.year - current_year)      [MODERATE confidence]
progress        += hours_elapsed * research_speed_per_hour
completion       = progress >= days_needed
```

On completion the technology is added to `ResearchState::completed`, its modifiers
are summed into `Country::tech_modifiers`, and its unlocked equipment and building
keys become producible. Unlocks are derived by scanning completed technologies, so no
duplicate unlock state exists that could drift out of sync with the research state.

## Outputs

`tech_modifiers` on the country, equipment availability
(`equipment_unlocked`), building availability (`building_unlocked`), research slot
state, and a `research` event.

## Failure states / edge cases

* all slots occupied: `StartResearch` is rejected with `SlotUnavailable`
* prerequisites missing: rejected with `PrerequisitesMissing`
* cancel: progress is discarded, the slot frees immediately
* zero research speed: progress stays at 0 rather than dividing by zero
* technologies with no unlocks: still valid, they only contribute modifiers

## Known uncertainty

* The ahead-of-time penalty curve is a linear approximation.
* Doctrines and research categories are not modelled (RES-005).
* Research bonuses from national spirits or events cannot exist yet because those
  systems are absent (POL-003, POL-002).
