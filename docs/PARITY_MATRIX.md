# PARITY MATRIX

One record per feature inventory item (spec section 9). Statuses use only the
allowed vocabulary (spec section 10). Confidence follows spec section 6. A feature
reaches `VALIDATED` only when every applicable requirement is satisfied (spec
section 11) and an evidence packet exists under `docs/evidence/<id>/`.

Status legend: UNRESEARCHED, RESEARCHING, SPECIFIED, NOT_IMPLEMENTED, PROTOTYPE,
PARTIAL, FUNCTIONAL, PARITY_TESTING, VALIDATED.

## Summary

| ID | Feature | Status | Confidence | UI | AI | Save | Tests |
|---|---|---|---|---|---|---|---|
| SIM-001 | Simulation clock and tick order | PARITY_TESTING | CONFIRMED | yes | - | yes | unit + golden |
| SIM-002 | Deterministic RNG streams | PARITY_TESTING | CONFIRMED | - | yes | yes | unit + golden |
| SIM-003 | Command validation/apply/log | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| SIM-004 | World hashing | PARITY_TESTING | CONFIRMED | - | - | yes | unit + golden |
| SIM-005 | Headless execution | VALIDATED | CONFIRMED | - | yes | - | golden |
| MAP-001 | Province graph | PARITY_TESTING | CONFIRMED | yes | yes | yes | unit + golden |
| MAP-002 | States, cores, slots, manpower pools | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| MAP-003 | Terrain and movement cost | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| MAP-004 | Strategic regions and weather | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| MAP-005 | Pathfinding | PARITY_TESTING | HIGH | yes | yes | - | unit |
| MAP-006 | Front-line detection | FUNCTIONAL | HIGH | yes | yes | - | unit + golden |
| MAP-007 | Encirclement detection | PARITY_TESTING | HIGH | yes | yes | - | unit + golden |
| MAP-008 | Rivers, straits, canals | NOT_IMPLEMENTED | LOW | - | - | - | - |
| COU-001 | Country state and capital | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| COU-002 | Political power, stability, war support | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| COU-003 | Laws | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| COU-004 | Manpower | FUNCTIONAL | HIGH | yes | yes | yes | unit + golden |
| COU-005 | Fuel | PARTIAL | MODERATE | yes | yes | yes | unit |
| COU-006 | Modifier scopes | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| IND-001 | Construction queue | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| IND-002 | Building slots and levels | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| IND-003 | Production lines | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| IND-004 | Production efficiency + switch retention | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| IND-005 | Resource production and consumption | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| IND-006 | Consumer goods | FUNCTIONAL | LOW | yes | yes | yes | unit |
| IND-007 | Equipment stockpile and reinforcement | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| IND-008 | Trade and convoys (land and sea routes, blockade, factory cost) | SPECIFIED | MODERATE | yes | yes | yes | unit + golden |
| IND-009 | Equipment designers | NOT_IMPLEMENTED | LOW | - | - | - | - |
| RES-001 | Technology graph and prerequisites | FUNCTIONAL | HIGH | yes | yes | yes | unit + golden |
| RES-002 | Research slots and progress | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| RES-003 | Unlock propagation to equipment | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| RES-004 | Modifier propagation | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| RES-005 | Doctrines and research bonuses | NOT_IMPLEMENTED | LOW | - | - | - | - |
| LND-001 | Division templates and composition stats | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| LND-002 | Division instance state | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| LND-003 | Training and deployment | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| LND-004 | Movement | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| LND-005 | Land combat resolution | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| LND-006 | Combat width and reinforcement | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| LND-007 | Armour/piercing interaction | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| LND-008 | Organisation/strength/equipment losses | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| LND-009 | Retreat and destruction | PARITY_TESTING | HIGH | yes | yes | - | unit + golden |
| LND-010 | Battle plans (front/offensive/fallback) | PARTIAL | HIGH | yes | yes | yes | unit + golden |
| LND-011 | Entrenchment and planning | FUNCTIONAL | MODERATE | yes | yes | yes | unit |
| LND-012 | Combat debugger | FUNCTIONAL | HIGH | part | - | - | unit |
| LND-013 | Strategic redeployment | NOT_IMPLEMENTED | LOW | - | - | - | - |
| LOG-001 | Supply sources and capacity | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| LOG-002 | Supply propagation and distance falloff | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| LOG-003 | Emergent encirclement starvation | PARITY_TESTING | HIGH | yes | yes | - | unit + golden |
| LOG-004 | Supply debugger | FUNCTIONAL | HIGH | part | - | - | unit |
| LOG-005 | Fuel distribution | FUNCTIONAL | LOW | yes | yes | yes | unit |
| LOG-006 | Ports and overseas supply | NOT_IMPLEMENTED | LOW | - | - | - | - |
| LOG-007 | Motorisation effect | PROTOTYPE | LOW | yes | yes | yes | - |
| AIR-001 | Air wings, bases, missions, air combat | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| AIR-002 | Air superiority feeding land combat | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| AIR-003 | CAS into land battles | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| AIR-004 | Strategic bombing and logistics strike | FUNCTIONAL | LOW | part | yes | yes | unit |
| AIR-005 | Air control per region | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| AIR-006 | Anti-air defence | FUNCTIONAL | LOW | yes | yes | yes | unit |
| AIR-007 | Air detection model | NOT_IMPLEMENTED | LOW | - | - | - | - |
| AIR-008 | Air fuel and pilot manpower | NOT_IMPLEMENTED | LOW | - | - | - | - |
| AIR-009 | Reconnaissance / fog of war | NOT_IMPLEMENTED | LOW | - | - | - | - |
| NAV-001 | Ships, task forces, fleets, sea zones | SPECIFIED | MODERATE | yes | yes | yes | unit + golden |
| NAV-002 | Detection, positioning, screening, engagements | SPECIFIED | MODERATE | yes | yes | - | unit |
| NAV-003 | Missions: patrol, strike force, escort, raid, support, training | SPECIFIED | MODERATE | yes | yes | yes | unit |
| NAV-004 | Naval invasion workflow (load, cross, land, supply) | SPECIFIED | MODERATE | yes | yes | yes | unit + golden |
| NAV-005 | Ports as supply sources, convoys, blockade | SPECIFIED | LOW | yes | yes | yes | unit |
| NAV-006 | Repair and port facilities | SPECIFIED | LOW | yes | yes | yes | unit |
| NAV-007 | Carrier air power | SPECIFIED | LOW | part | yes | yes | unit |
| POL-001 | Focus trees (prerequisites, exclusions, bypass, effects, AI) | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| POL-002 | Events (triggers, options, chains, delays) | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| POL-003 | Decisions (visibility, cost, timers, targeting) | PARITY_TESTING | MODERATE | yes | yes | yes | unit |
| POL-004 | Script engine (triggers, effects, scopes, variables) | PARITY_TESTING | HIGH | part | yes | yes | unit |
| POL-005 | Timed/scripted modifiers feeding the modifier stack | FUNCTIONAL | MODERATE | part | yes | yes | unit |
| POL-007 | Ideology drift, elections, coups | NOT_IMPLEMENTED | LOW | - | - | - | - |
| POL-006 | National spirits and advisors | PARITY_TESTING | MODERATE | yes | yes | yes | unit + observer run |
| DIP-001 | Relations and drift | FUNCTIONAL | MODERATE | part | yes | yes | unit |
| DIP-002 | Factions (join/leave, leader succession) | PARITY_TESTING | HIGH | yes | yes | yes | unit |
| DIP-003 | Guarantees and military access | PARTIAL | MODERATE | - | yes | yes | unit |
| DIP-004 | War declaration and coalitions | PARITY_TESTING | HIGH | yes | yes | yes | unit + golden |
| DIP-005 | Capitulation | PARITY_TESTING | MODERATE | yes | yes | yes | unit + golden |
| DIP-006 | Peace and territorial transfer | FUNCTIONAL | MODERATE | - | yes | yes | unit + golden |
| DIP-007 | Lend-lease, volunteers, expeditionary | NOT_IMPLEMENTED | LOW | - | - | - | - |
| OCC-001 | Owner vs controller, occupation | FUNCTIONAL | HIGH | yes | yes | yes | unit |
| OCC-002 | Resistance, compliance, garrison need | FUNCTIONAL | LOW | part | yes | yes | unit |
| OCC-003 | Occupation policies, suppression | NOT_IMPLEMENTED | LOW | - | - | - | - |
| INT-xxx | Intelligence (all items) | NOT_IMPLEMENTED | LOW | - | - | - | - |
| AI-001 | Layered AI architecture | FUNCTIONAL | HIGH | - | yes | yes | unit |
| AI-002 | AI acts only through commands | PARITY_TESTING | CONFIRMED | - | yes | - | unit |
| AI-003 | AI decision reasons | FUNCTIONAL | HIGH | part | yes | - | unit |
| AI-004 | AI industry/research/production | FUNCTIONAL | MODERATE | - | yes | - | unit |
| AI-005 | AI military (recruit, deploy, fronts, offensives) | FUNCTIONAL | MODERATE | - | yes | - | unit |
| AI-006 | AI diplomacy (faction join, war declaration, peace) | FUNCTIONAL | MODERATE | part | yes | - | unit + 730-day observer run |
| AI-007 | AI air/naval | NOT_IMPLEMENTED | LOW | - | - | - | - |
| SAV-001 | Versioned sectioned save files | PARITY_TESTING | HIGH | yes | - | yes | unit + golden |
| SAV-002 | Per-section hash verification on load | PARITY_TESTING | CONFIRMED | - | - | yes | unit |
| SAV-003 | Command log persistence | FUNCTIONAL | HIGH | - | - | yes | unit |
| SAV-004 | Replay metadata | PARTIAL | MODERATE | - | - | yes | unit |
| SAV-005 | Save migration | SPECIFIED | LOW | - | - | yes | - |
| MOD-xxx | Mod load order, validation, test pack | PARTIAL | MODERATE | - | - | - | unit |
| MP-xxx | Multiplayer | NOT_IMPLEMENTED | LOW | - | - | - | - |
| UI-001 | Browser client and map overlays | FUNCTIONAL | HIGH | yes | - | - | manual |
| UI-002 | Production/construction/research panels | FUNCTIONAL | HIGH | yes | - | - | manual |
| UI-003 | Military panel and orders | FUNCTIONAL | HIGH | yes | - | - | manual |
| UI-004 | Alerts | FUNCTIONAL | HIGH | yes | - | - | manual |
| UI-005 | Battle/supply detail windows | NOT_IMPLEMENTED | LOW | - | - | - | - |
| TOOL-001 | World auditor | PARITY_TESTING | HIGH | - | - | - | unit + golden |
| TOOL-002 | Determinism oracle and hash report | VALIDATED | CONFIRMED | - | - | yes | golden |
| TOOL-003 | Per-tick performance metrics | FUNCTIONAL | HIGH | - | - | - | manual |
| TOOL-004 | Stress ladder and long-run automation | PARTIAL | MODERATE | - | - | - | manual |
| TOOL-005 | Inspectors (country/province/supply/battle) | FUNCTIONAL | HIGH | part | - | - | manual |
| TOOL-006 | Profiler captures and benchmark history | NOT_IMPLEMENTED | LOW | - | - | - | - |

## Promotions and demotions

* `PARTIAL -> FUNCTIONAL` requires the core loop plus integration plus AI support
  plus save/load plus edge cases (spec section 120). Several rows above are held at
  PARTIAL because one of those is missing, e.g. DIP-003 (no UI), SAV-004 (no playback
  driver), LND-010 (execution is manual, not plan-driven).
* `FUNCTIONAL -> PARITY_TESTING` requires reference behaviour substantially
  represented, no major placeholder shortcuts and existing tests.
* `PARITY_TESTING -> VALIDATED` requires an evidence packet. Evidence packets are
  recorded under `docs/evidence/<id>/`; when a packet is missing the row stays at
  PARITY_TESTING even though tests pass - this is deliberate (spec sections 11-12).

## Known structural differences

Documented in `docs/KNOWN_DIFFERENCES.md`. The largest are: air and naval warfare are
absent (AIR-xxx, NAV-xxx), focus trees/events/decisions are absent (POL-*),
trade/convoy economics are absent (IND-008), intelligence is absent (INT-xxx), and
multiplayer transport is absent (MP-xxx). Each is tracked as a discrepancy with a
severity and a resolution plan rather than hidden behind a UI that pretends
otherwise.
