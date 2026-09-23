# Movement and pathfinding

Status: implemented, tested (`tests/test_military.cpp`, `GOLDEN_003`).
Confidence: HIGH for the model (terrain-weighted province transit), MODERATE for
the constants.

## Inputs

Province adjacency, target terrain, target infrastructure, weather of the target's
region, division speed (the minimum over its non-support battalions), supply level
and a path computed by `find_path`.

## Rules

```
terrain_cost   = plains 1.0, forest 1.5, hills 1.6, mountain 3.0, urban 1.4,
                 marsh 2.0, desert 1.1, jungle 2.5
hours_per_edge = base_hours_per_province / max(min_speed, division_speed)
                 * terrain_cost(target)
                 * weather_movement_multiplier(region)
                 * (1 + river_crossing_penalty if river)
                 / (1 + 0.05 * infrastructure(target))
                 * 2 if division supply < 0.2
move_progress += 1 / hours_per_edge per hour; arrival at >= 1.0
```

Pathfinding is Dijkstra over land edges with the same terrain weighting, filtered by
legality (`PathRequest`): `require_controlled` (own or allied territory),
`allow_hostile` (at war with the controller). Ties break by province id, so paths are
deterministic.

## Outputs

`Division::location`, `move_from`, `move_to`, `move_progress`, `path`, `moving`.

## Failure states / edge cases

* path becomes illegal mid-move (province captured): the path is recomputed; if no
  legal route remains the move is cancelled and the division stays put.
* hostile divisions in the target province: the division halts at the edge and the
  combat phase opens a battle instead of teleporting into the province.
* entering enemy-controlled territory requires an offensive order, or the province
  must be undefended while at war with its controller.
* strategic redeployment, naval transport and paradrops are not implemented; the
  parity matrix lists them as gaps rather than silently approximating them.
