# LND-005 evidence — land combat resolution

## What was tested

* `tests/test_military.cpp`: template aggregation; terrain movement; incremental
  combat with 3:1 odds (attacker wins, defender retreats to a legal province);
  surrounded defender destroyed; organisation recovery scaling with supply; armour
  advantage multiplier; no negative organisation or non-finite strength.
* `tests/golden.cpp GOLDEN_004_land_combat_resolves`: three attacking divisions
  ordered into a defended frontier province; the battle must start on the first tick
  and the province must change controller within 30 days.
* `tests/golden.cpp GOLDEN_006_encirclement_starves_supply`: a cut-off division's
  supply drops to zero, so it cannot recover organisation.

## What happened

* Battles start when hostile divisions meet or an attack is ordered, and resolve over
  simulated hours; they never resolve in a single tick. A 3:1 attack takes the
  province; an equal fight does not (the defender's defence value dominates, which
  is why the test uses 3:1 rather than a coin flip).
* Retreating defenders move to a legal adjacent province; when no legal retreat
  exists the division is destroyed and removed from the world.
* `Battle::debug` carries one line per engaged division per tick with base attack,
  terrain, supply, planning, commander, experience, final attack, enemy defence and
  the resulting organisation/strength damage.

## How we know it works

The province controller changes only after the defending side leaves the battle,
which is observable in both the test assertions and the client's political overlay.
Damage is a deterministic function of state except for one documented `RNG_COMBAT`
draw per side per battle (the armour-advantage roll), so identical runs stay
hash-identical.

## Edge cases tested

* organisation reaching exactly zero (retreat path, not NaN)
* surrounded defender (destruction path)
* absence of supply (damage scales down, recovery stops)
* armour advantage (damage multiplier visible in the debug lines)

## What remains different

No combat tactics, no commander skill growth, no air support influence (LND-014,
LND-016, AIR-001). Battle plan execution is manual (LND-010).