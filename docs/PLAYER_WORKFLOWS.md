# PLAYER WORKFLOWS

Each workflow is a sequence a player performs through the production interface
(browser client) or the command API. A workflow is only "working" when every step
does something real; a step that dead-ends marks the related feature incomplete
(spec sections 74-77, 105).

## 1. Train and deploy a division

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Open Military, pick a template, press Train | `RecruitDivision` command, manpower reserved | works |
| 2 | Watch the training list | days remaining, strength and equipment fill from the stockpile | works |
| 3 | Wait for "ready" | `training_days_left` reaches 0 | works |
| 4 | Click the division, click a province | `DeployDivision`, division appears on the map | works |
| 5 | Assign to an army | `AssignDivisionToArmy` | works |
| 6 | Set Front or Offensive order | `SetDivisionOrder`, the army's front/offensive line is drawn | works |
| 7 | Move to the front | `MoveDivision` (or offensive order targets) | works |
| Gap | Bulk recruitment of many divisions at once | one command recruits up to 10 | partial |

## 2. Industry ramp-up

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Construction panel: queue a military factory | `StartConstruction`, project appears with cost | works |
| 2 | Watch progress | civilian industry fills each project in queue order | works |
| 3 | Factory completes | state military factory count rises | works |
| 4 | Production panel: assign factories to equipment | `SetProductionLine`, validated against controlled factories | works |
| 5 | Watch efficiency rise | efficiency and cap grow while producing | works |
| 6 | Switch to a better model | retention applied once, output drops then recovers | works |
| Gap | Trade for missing resources | not implemented (IND-008) | gap |
| Gap | Equipment designer variants | not implemented (ECON-002) | gap |

## 3. Research to combat

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Research panel: start an available technology | `StartResearch`, slot occupied | works |
| 2 | Watch progress | days accrue at the research-speed rate | works |
| 3 | Completion | modifiers applied and equipment keys unlocked | works |
| 4 | Produce the unlocked equipment | production line accepts it (gated by unlock) | works |
| 5 | Equip divisions | training and `phase_reinforcement` draw the model the country issues for the slot's family: the one on a live line first, then the best stocked member, with a depot fallback | works |
| 6 | Design a variant | Production panel: choose an archetype, fit one component per slot (locked parts shown disabled), name it, create it | works |
| 7 | Put the variant into production | `CreateEquipmentDesign` registers real equipment; the design appears in the line picker and the Produce button sets a line (factory budget is enforced, so an existing line may need trimming first) | works |
| 8 | Field the variant | existing divisions retrofit as replacement gear arrives; a selected division lists the models it actually holds | works |
| 9 | Observe combat statistics | damage uses the equipment actually present, so the retrofit shows up in the battle results | works |

Verified end to end in v0.8.0: a browser session designed `VEL_armor_1` (cost 12.90,
armour 42, piercing 44) through the Production panel, put it on a line, and watched the
stockpile grow; a 365-day AI run fielded 2,589 units of design equipment across 514
divisions with 0 divisions left on an older model while a newer one sat in the depot.

## 4. Defend and attack

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Create an army and assign divisions | `CreateArmy`, `AssignDivisionToArmy` | works |
| 2 | Assign a general | `AssignGeneral` | works |
| 3 | Draw a front line | `SetDivisionOrder(FrontLine)` from real adjacency | works |
| 4 | Order an offensive | `SetDivisionOrder(Offensive)` with a target | works |
| 5 | Fight | battles start when hostile divisions meet or an attack is ordered | works |
| 6 | Read the battle | battle markers and per-division organisation/strength | partial (no detail window yet: UI-005) |
| 7 | Retreat or be destroyed | illegal retreats destroy the division | works |
| Gap | Plan execution without micromanagement | plans prepare; execution is per-division orders (LND-010) | gap |

## 5. Logistics management

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Observe supply on the map | per-province supply overlay | works |
| 2 | Build a supply hub near the front | `StartConstruction(SupplyHub)` | works |
| 3 | Build railways to raise capacity | `StartConstruction(Railway)` | works |
| 4 | Watch divisions recover | organisation recovery scales with supply | works |
| 5 | Cut the enemy off | encirclement starves supply automatically | works |
| Gap | Motorised distribution (trucks) | command exists, no simulation effect yet (LOG-007) | gap |
| Gap | Overseas supply via ports | not implemented (LOG-006) | gap |

## 6. Diplomacy and war

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | Open Diplomacy, declare war | `DeclareWar`, coalitions from factions/guarantees | works |
| 2 | Fight the war to a decision | control transfer, capitulation thresholds | works |
| 3 | Offer peace | `OfferPeace` ends the war, goals applied per control | works |
| Gap | Lend-lease, volunteers, access negotiation | not implemented (DIP-007) | gap |
| Gap | Multi-party peace conference | not implemented (DIP-001) | gap |

## 7. Observing a campaign (no player commands)

| Step | Action | Result | Status |
|---|---|---|---|
| 1 | `game --days 365 --audit --summary` | all countries played by AI | works |
| 2 | Check invariants | auditor reports violations | works |
| 3 | Check determinism | `--hashes` compared across runs | works |

## Dead ends found by this audit

* No air or naval gameplay exists yet, so any workflow that starts with "build
  aircraft/ships" ends immediately: `AIR-xxx`, `NAV-xxx` (BLOCKER).
* No focus trees: the political progression workflow ends after laws: `POL-001`.
* Battle detail and supply route inspection are available in the model but not the
  client (`UI-005`).
