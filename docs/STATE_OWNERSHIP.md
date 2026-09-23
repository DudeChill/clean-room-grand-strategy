# STATE OWNERSHIP

One piece of state, one owner. Everything else reads or issues commands.
Duplicated authoritative state is a bug (spec section 25).

| State | Owner | Storage |
|---|---|---|
| Province owner / controller | world | `World::provinces[].owner/controller` |
| Province terrain, adjacency, infra, VP, resources | world | `World::provinces[]` |
| Supply level, supply source, bottleneck | supply phase | `World::provinces[].supply_*` |
| State owner / controller / factories / slots | world | `World::states[]` |
| Occupation resistance / compliance / garrison | diplomacy phase | `World::states[]` |
| Weather per region | weather phase | `World::regions[].rain/snow/...` |
| Country budgets, laws, research, stockpile, lines | world | `World::countries[]` |
| Country modifiers (base/tech/law/national) | research + politics | `Country::*_modifiers` |
| Division location, org, strength, equipment, supply | military phases | `World::divisions[]` |
| Division training progress | training phase | `Division::training_days_left`, `Country::training` |
| Army composition and orders | command layer | `World::armies[]` |
| Battle participants, damage, progress | combat phase | `World::battles[]` |
| War participants and goals | diplomacy phase | `World::wars[]` |
| Relations, factions | diplomacy phase | `World::relations`, `World::factions` |
| Characters (generals) | world + command layer | `World::characters[]` |
| RNG streams | core | `Game::rng` |
| Command log (replay source) | command layer | `Game::log` |
| AI planning state | AI phase | `Game::ai` |

Rules:

* A phase may only mutate state it owns, plus state whose owner explicitly delegates
  (documented in `ARCHITECTURE.md`).
* UI never owns state: it renders snapshots and sends commands.
* No subsystem keeps a private cache of another subsystem's state; if caching is
  ever required it must be derivable and invalidated by the owner.
