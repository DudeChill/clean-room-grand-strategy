# UI SCREEN INVENTORY

The client is a browser application (`web/`) driven entirely by the JSON API. Every
control maps to a command; nothing in the client mutates gameplay state directly
(spec section 72).

| Screen / element | Purpose | Displayed information | Controls | Simulation connection | Hotkeys | Known gaps |
|---|---|---|---|---|---|---|
| Top bar | Global state and time control | date, player tag and name, manpower, political power, stability, war support, fuel | pause, speeds 1-5, save, recenter | `/api/time`, `/api/save` | space, 1-5 | no separate monthly/daily step |
| Map canvas | Spatial view of the world | provinces coloured by overlay, victory points, supply hubs, unit counters, battles, front lines, offensives, selection | click to select, click to order, drag to pan, wheel to zoom | reads snapshot arrays; selection drives orders | – | no minimap, no zoom-to-cursor limits beyond 1..24 |
| Overlay selector | Switch map mode | political, control, supply, terrain, fronts | 5 buttons | colours derived from authoritative province state | – | no weather overlay, no resource overlay |
| Tooltip | Province detail on hover | name, terrain, victory points, owner, controller, supply, garrisoned divisions, infrastructure | hover | snapshot | – | no building list per province |
| Toolbar → Recruit/Deploy | Division pipeline | templates with width/org/soft/defence/manpower; training queue with days and strength | Train, click a division then a province to deploy | `RecruitDivision`, `DeployDivision` | – | no batch recruit dialog |
| Production panel | Equipment production | production lines with factories, efficiency, cap, cumulative output, resource satisfaction, stockpile | assign factories to an equipment model, remove a line | `SetProductionLine`, `RemoveProductionLine` | – | no per-line detail (unit cost, resource breakdown) |
| Construction panel | Industry expansion | queue with kind, level, progress bar, cost | queue a building into a state, cancel a project | `StartConstruction`, `CancelConstruction` | – | no per-province placement for province-level buildings (uses the state's first land province) |
| Research panel | Technology | slots with progress, available technologies with cost and category, completed list | start research, cancel research | `StartResearch`, `CancelResearch` | – | no tech tree visualisation, no prerequisites display |
| Military panel | Armies and divisions | templates, training, armies (order, general, size, motorisation), division list with org/strength/location/battle marker | create army, assign division, front/offensive/fallback/garrison orders, stance, motorisation | `CreateArmy`, `AssignDivisionToArmy`, `SetDivisionOrder`, `SetStance`, `MotorizeSupply` | – | no commander assignment UI, no drag-and-drop, no selection groups |
| Air panel | Air warfare | wings with model, planes/establishment, efficiency, losses, base and region, current mission; air-control shares per contested region; air bases available to the player | form a wing from stockpiled aircraft at a controlled air base, change a wing's mission, disband a wing | `CreateAirWing`, `SetAirMission`, `DisbandAirWing`, `/api/state` wings + air_regions | `a` opens the tab | no rebasing UI yet (the command exists), no range preview |
| Air control overlay | Contested air space at a glance | each region coloured by the balance of friendly versus hostile air control | overlay button | `/api/state` air_regions control shares | – | sea regions are excluded |
| Diplomacy panel | Wars and relations | active wars with sides, country table (factories, divisions, war status), laws with cost and enactment | declare war, enact law | `DeclareWar`, `SetLaw` | – | no relation values, no faction management, no peace UI |
| Log panel | Event stream | last 80 world events with tick and kind | scroll | `/api/state` events | – | no filtering |
| Alerts (left) | Actionable situations | idle research slot, unassigned factories, unassigned divisions, ready divisions, resource-short lines | click to jump to the relevant panel | derived from authoritative state each snapshot | – | no alert dismissal or severity ordering |
| Selection panel | Selected province and its units | province stats plus divisions present with org/strength | click a division to select for orders | snapshot + command API | – | no multi-province selection |
| Battle detail | Live battle breakdown | attackers/defenders with org, strength, supply, entrenchment, planning; per-side soft/hard/defence/breakthrough/armour/piercing totals; progress; terrain, river, encirclement; last-tick damage decomposition per division | none (inspection) | `/api/battle?id=N` (reads `Battle::debug`) | – | no historical battle log |
| Supply detail | Why a province is supplied or not | controller, supply level, delivered capacity per hour, bottleneck province, the route with per-step capacity, and each division's supply/fuel | none (inspection) | `/api/supply?province=N` (reads `explain_supply_route`) | – | no map overlay of the route itself |

## Server endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/` , `/app.js`, `/style.css` | GET | client assets |
| `/api/map` | GET | static map: provinces (id, name, x, y, terrain, sea, state, adjacency, hub, vp), states |
| `/api/state` | GET | dynamic snapshot: date, countries, per-province owner/controller/supply arrays, divisions, battles, wars, events, player detail (lines, construction, research, training, armies, generals, templates, laws, alerts, stockpile) |
| `/api/command` | POST | validated command submission (returns ok/error with the rejection reason) |
| `/api/time` | POST | pause and speed |
| `/api/save` | POST | save the session |
| `/api/battle?id=N` | GET | battle detail including per-division state, side totals and the last-tick damage breakdown |
| `/api/supply?province=N` | GET | supply route with per-step capacity, delivered amount, bottleneck and garrison supply/fuel |
| `/api/meta` | GET | seed, scenario, player, pause state, hash report |
| `/api/hashes` | GET | subsystem hash report as text (determinism checking from the browser) |
