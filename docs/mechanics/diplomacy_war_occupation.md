# Diplomacy, war, capitulation and occupation

Status: implemented, tested (`tests/test_politics.cpp`, `GOLDEN_011`).
Confidence: HIGH for coalitions and war bookkeeping; MODERATE for capitulation
thresholds and peace resolution; LOW for relation drift rates.

## Inputs

Relations, factions (leader + members), guarantees, military-access flags, puppet and
overlord links, war goals, controlled territory, factory counts, capital position.

## Rules

```
coalition(side)  = self + puppets + overlord + faction members + countries that
                   guaranteed the defender (guarantees are consumed on use);
                   a country is never placed on both sides of the same war
declare_war      = creates the War with goals, enrols participants in Country::wars,
                   sets at_war and relation.at_war, drops relations to <= -50 and
                   clears guarantees/NAP/access between the new enemies
countries_at_war = true iff an active war has the two on opposite sides
capitulation     = capital state controlled by an enemy (or a country its owner is at
                   war with, that is not a co-belligerent) AND at least 70% of the
                   country's starting factories lost
capitulate       = all territory the loser still controls transfers to the winner,
                   its divisions are destroyed, its wars close, puppets transfer,
                   alive = false, a "capitulation" event is logged
peace            = a war ends when one side holds the war-goal states or the aggressor
                   lost its capital; goals that are satisfiable from current control
                   are applied as state transfers
resistance       = grows while a state is controlled by a non-core country
compliance       = grows slowly toward 1 - resistance
garrison_required= derived from resistance and state population
```

## Outputs

`World::wars`, `World::relations`, `World::factions`, `Country::wars`, `Country::at_war`,
`Country::alive`, province/state ownership and control, `State::resistance`,
`State::compliance`, `State::garrison_required`, war/faction/occupation events.

## Failure states / edge cases

* declaring war on a faction member pulls the whole faction in (verified)
* a country already at war with the target: rejected with `AlreadyAtWar`
* capitulating country with no remaining territory: removed from play, no dangling
  war participants (audit checks every participant exists)
* captured province: constructions there are cancelled and supply caches cleared the
  same tick
* capital lost but industry intact above the threshold: no capitulation, the country
  keeps fighting (stability drifts down faster)

## Known uncertainty

* The 70% factory threshold and the relation drift rates are approximations.
* Peace conferences with multiple claimants, war score and puppeting choices are not
  modelled (DIP-001); peace is a bilateral application of goals.
* Lend-lease, volunteers and expeditionary forces are absent (DIP-007).
