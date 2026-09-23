# Production efficiency

Status: implemented, tested (`tests/test_economy.cpp`, `GOLDEN_001`).
Confidence: MODERATE. The shape of the curve (start low, grow toward a cap that
itself grows, partial retention when switching models) is well documented in public
material; the exact constants are tunable data, not reverse-engineered facts.

## Inputs

* `ProductionLine::factories` - assignment, validated against controlled military
  factories at command time.
* `ProductionLine::efficiency` - 0.10 start, ceiling 1.0.
* `ProductionLine::efficiency_cap` - 0.50 start, grows only while producing.
* `ProductionLine::previous` - pending-switch queue: commands push the outgoing
  model here; the industry phase consumes it and applies retention.
* `SimConstants` - `ic_per_military_factory`, `efficiency_start`,
  `efficiency_cap_base`, `efficiency_cap_growth_per_day`, `efficiency_growth_per_day`,
  `switch_same_archetype_retention`, `resource_shortage_floor`.

## Rules

```
output_per_hour = factories * ic_per_factory / 24 * efficiency * (1 + FactoryOutput)
units_per_hour  = output_per_hour * resource_factor / unit_cost
efficiency     += (cap - efficiency) * efficiency_growth_per_day / 24
cap            += efficiency_cap_growth_per_day / 24 while the line produced > 0
```

Switch retention (applied once, by the industry phase, when it observes
`previous.front() != equipment`):

* same archetype: `efficiency *= 0.70`, `efficiency_cap *= 0.70`
* different archetype: `efficiency = 0.10`, `efficiency_cap = 0.50`

Resource shortage: `resource_factor = min over required resources of
clamp(available/required, resource_shortage_floor, 1)`, where resources are
allocated to lines in `Country::lines` order (deterministic, no priority race).

## Outputs

Equipment enters `Country::equipment_stockpile[equipment]`, `output_today` and
`output_total` accumulate, `resource_shortage` is stored for the UI.

## Failure states / edge cases

* zero factories: line produces nothing, efficiency does not decay on its own
* zero resources: output multiplied by the shortage floor, never zero-division
* equipment switch: retention applies exactly once (single owner: industry)
* country loses all military factories: assignment is re-validated on the next
  command; existing lines keep their assignment but produce nothing

## Known uncertainty

* Exact growth rates and switch retention factors are approximations.
* Consumer goods are modelled as a reduction of available industrial capacity rather
  than factory-level assignment.
