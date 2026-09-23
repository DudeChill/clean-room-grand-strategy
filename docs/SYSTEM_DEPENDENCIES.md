# SYSTEM DEPENDENCIES

Dependency direction: an arrow means the target must be correct for the source to
be correct. A feature whose dependency is incomplete cannot be promoted past
PARTIAL, and its parity entry must say which dependency is missing.

```
Movement            -> Map graph, Division stats, Weather, Supply (speed penalty)
Combat              -> Division stats, Terrain, Weather, Supply, Equipment,
                       Commanders, Entrenchment, Planning, Combat width
Territory control   -> Combat outcome, Movement, Occupation model
Supply              -> Map graph, Railways/Infrastructure, Hubs, Ports, Control
Industry            -> Resources, Technologies (unlocks), Construction, Laws
Equipment flow      -> Industry, Stockpile, Division reinforcement, Training
Research            -> Content (tech graph), Year, Modifiers, Unlocks
Training            -> Manpower, Equipment stockpile, Industry, Templates
Production lines    -> Equipment definitions, Resources, Factory counts
Construction        -> Civilian industry, Laws (consumer goods), Building slots
Politics            -> Laws, Political power, War state
Diplomacy           -> Relations, Factions, Guarantees, War goals, Capitulation
Capitulation        -> Territory control, Industry (factory baseline), Wars
Peace               -> Wars, Territorial control, War goals
AI                  -> Every system above (it issues the same commands as a player)
Save/Load           -> Every subsystem's serialization
Hashing/Determinism -> Every subsystem's serialization
UI                  -> Snapshots derived from world state + command API
```

## Verified dependency chains

The following chains are exercised end to end by tests, which is what makes the
claim real rather than architectural (spec section 28):

1. **Technology -> equipment -> production -> division -> combat**
   `GOLDEN_007` (research unlocks), `GOLDEN_001` (production), `GOLDEN_002`
   (training consumes equipment), `GOLDEN_004` (combat outcome depends on stats).
2. **Manpower -> training -> deployment -> front assignment**
   `GOLDEN_002`, `GOLDEN_005`.
3. **Territory control -> supply -> organisation recovery -> combat performance**
   `GOLDEN_006` plus the org-recovery test in `tests/test_military.cpp`.
4. **Construction -> factory count -> production capacity**
   `GOLDEN_008`.
5. **Capitulation -> territory transfer -> war resolution**
   `GOLDEN_011`.

Chains that do **not** yet exist and therefore cap related parity entries below
VALIDATED: air support into combat, naval invasion logistics, trade convoys,
intelligence operations, focus trees feeding political effects.
