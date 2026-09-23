# Naval warfare — specification

Status: **SPECIFIED, NOT IMPLEMENTED** (discrepancy NAV-001, BLOCKER).
Confidence: MODERATE for the model shape, LOW for the numeric constants, which will
be data.

## Inputs

* Ship equipment models from content (destroyer, light cruiser, heavy cruiser, battle
  cruiser, battleship, carrier, submarine, convoy, landing craft) with hull, armour,
  guns, torpedoes, anti-air, anti-submarine, speed, detection, visibility, range,
  fuel use, cost and resource draw.
* Naval base levels per province (`Province::naval_base`) and sea provinces with
  `sea_adj` links (the map already provides sea zones and coastlines).
* Country state: stockpiles, industry, fuel, manpower, convoys.
* Ports: coastal land provinces with a naval base; the link between a sea zone and
  the land provinces that touch it.

## State

```
Ship {
  id, country, equipment (model), name, strength (0..1), organisation (0..1),
  experience, fuel, task_force, port (repair), sunk_tick
}
TaskForce {
  id, country, fleet, name, ships[], location (sea zone or port),
  mission (NavalMission), detection_state, fuel_state
}
Fleet { id, country, name, task_forces[], admirals[] }
SeaRegionControl { region, control[(country, share)] }   // derived, air-style
```

Ships are individual entities (they take individual damage, sink individually and
carry names); task forces are the manoeuvre unit; fleets are administrative.

## Missions

| Mission | Effect |
|---|---|
| Patrol | reveals enemy task forces in adjacent sea zones |
| Strike force | intercepts detected enemies in a chosen zone |
| Convoy escort | protects convoys in a zone, engages raiders |
| Convoy raiding | attacks enemy convoys, cutting their trade and supply |
| Naval invasion support | protects a landing operation |
| Minelaying / minesweeping | area denial and clearance in a zone |
| Training | gains experience, no combat intent |

## Rules (per hour)

```
detection(tf)      = sum over ships of detection * (1 - enemy_visibility) * weather
engagement         = when both sides detect each other in the same sea zone
positioning(side)  = shares of screening (destroyers), capital and carrier weight,
                     modified by detection quality and admiral skill
damage             = guns vs armour, torpedoes vs large hulls, carriers via aircraft
                     (reusing the air model's attack values), submarines via detection
sinking            = strength reaching 0; the ship leaves the world, its crew is lost
retreat            = a badly damaged task force withdraws to the nearest friendly port
repair             = in port, strength recovers against dockyard capacity
```

Supply and trade integrate through convoy routes: raiding reduces the delivered
capacity of overseas supply (this is what makes NAV-001 a dependency of LOG-006).

## Integration points

* `phase_naval` runs after `phase_air` and before `phase_industry` (sinkings become
  equipment demand in the same tick).
* Naval invasion: a division in a coastal province is loaded onto transports in an
  adjacent sea zone, crosses (with detection and interception rolls), lands on a
  hostile coast, and the landing province becomes a supply source once a port is
  captured or a beachhead is established.
* `phase_supply` gains overseas routes: capital -> port -> sea zone -> port ->
  land network, with convoy capacity as the edge limit.
* Air missions naval strike and port strike damage ships in a zone or port.
* AI: a naval layer mirroring the air layer — build the best hull the country can
  afford, group into task forces by mission, protect convoys on routes it needs, raid
  the routes its enemies need, and invade only with naval superiority and transports.

## Commands

```
CreateTaskForce (country, province=port, value=size in ships of `equipment`)
AssignShip      (ship, task_force)
SetNavalMission (task_force, region=sea zone, value=NavalMission)
CreateFleet     (country, text=name)
SetFleetOrders  (fleet, value=order kind)
LaunchNavalInvasion (army, province=origin port, province_b=target coast, state=target state)
CancelNavalInvasion (army)
```

## Edge cases

* port captured while ships are repairing: ships are scuttled or displaced to the
  nearest friendly port
* task force with no fuel: it stays in port; raiders and escorts behave accordingly
* invasion fleet intercepted: transports are destroyed and the divisions are lost
* sea zone with no friendly port in range: missions are rejected at command time
* save/load: ships, task forces, fleets, missions, damage and experience all serialize

## Tests (to be written with the implementation)

* `GOLDEN-NAVY-1`: two task forces in one sea zone detect each other, fight, and the
  weaker one retreats or is sunk; the sinking creates equipment demand.
* `GOLDEN-NAVY-2`: convoy raiding reduces the supply delivered to an overseas theatre
  (the LOG-006 causal chain end to end).
* `GOLDEN-NAVY-3`: a naval invasion transports divisions, lands them, and they draw
  supply through the captured port.
* Determinism: two identical runs with fleets at sea produce identical world hashes.

## Explicit non-goals for the first pass

* Individual aircraft on carriers as separate entities (carrier air power is a
  statistic, reusing the air model's combat values).
* Submarine wolfpack automation beyond a mission and detection.
* Mines as persistent entities (minefields are zone-level numbers).