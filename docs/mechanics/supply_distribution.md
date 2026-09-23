# Supply distribution

Status: implemented, tested (`tests/test_supply.cpp`, `GOLDEN_006`).
Confidence: MODERATE for formula shape, LOW for constants. The important property -
supply is a flow over a real graph, so cutting the graph starves the pocket - is
structural and verified.

## Inputs

Province control, adjacency, terrain, infrastructure and railway levels, supply hubs,
the capital, division supply demand, and the motorisation level of the army a
division belongs to.

## Rules

```
source capacity = 10 * (1 + 0.15 * railway_level) * (1 + 0.05 * infrastructure)
edge cost       = max(1, terrain_cost(target) / (1 + 0.15*rail_target + 0.05*infra_target))
propagation     = one Dijkstra per source over land edges through provinces
                  controlled by the country or its co-belligerents
delivered(p)    = max over sources of capacity / (1 + supply_range_penalty * hops)
demand(p)       = 0.1 + Σ supply_use of divisions standing in p
supply_level(p) = clamp(delivered / demand, 0, 1)
division.supply = supply_level(location) / (1 + range_penalty * max(0, dist - hub_radius))
```

Fuel is drawn from `Country::fuel` in ascending division id order after supply, so
armoured and motorised formations consume fuel only when they are supplied.

## Outputs

`Province::supply_level`, `Province::supply_source`, `Province::supply_bottleneck`,
`Division::supply`, `Division::fuel`.

## Failure states / edge cases

* capital captured: remaining hubs carry the network; if none, supply is 0
* hub captured: its capacity leaves the network the same tick
* pocket / encirclement: no controlled path to any source -> level 0, no organisation
  recovery, weather attrition continues
* overseas army (no naval supply yet): currently unsupplied - tracked as NAVY
  dependency, not hidden
* zero divisions in a province: demand floor 0.1 keeps the level meaningful
* division in enemy-controlled ground: supply 0

## Debugging

`explain_supply_route` returns the path with per-step capacity, the delivered amount
and the bottleneck province, which is what the client shows for a selected division.
