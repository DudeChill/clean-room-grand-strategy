# DISCREPANCIES

Every known behaviour gap, with a stable identifier, severity, reproduction and
status (spec sections 13-15). Nothing is removed from this file because it is
inconvenient: entries close only when corrected, or when superseded by an explicit
documented design decision.

Severities: BLOCKER, MAJOR, MODERATE, MINOR, COSMETIC (spec section 14).

| ID | Severity | Subsystem | Reference behaviour | Current behaviour | Status |
|---|---|---|---|---|---|
| AIR-001 | BLOCKER | Air | Air wings, missions, superiority, CAS, losses | No air simulation at all | OPEN |
| NAV-001 | BLOCKER | Naval | Ships, task forces, detection, missions | No naval simulation at all | OPEN |
| POL-001 | BLOCKER | Politics | Focus trees drive national progression | No focus system | OPEN |
| POL-002 | MAJOR | Politics | Events and decisions with triggers/options | No event/decision scripting | OPEN |
| POL-003 | MAJOR | Politics | National spirits and advisors | Not modelled | OPEN |
| ECON-001 | MAJOR | Economy | Trade, convoys, blockades | Not modelled; resources are domestic only | OPEN |
| ECON-002 | MAJOR | Economy | Equipment designers with components | Equipment is fixed data; no designer | OPEN |
| INT-001 | MAJOR | Intelligence | Agencies, networks, operations | Not modelled | OPEN |
| MP-001 | MAJOR | Multiplayer | Lockstep with desync tooling | Commands serialize but there is no transport | OPEN |
| LND-010 | MODERATE | Land | Battle plans execute themselves once prepared | Plans prepare and display; the player moves divisions explicitly | OPEN |
| LND-014 | MODERATE | Land | Combat tactics modify battle rolls | No tactic selection | OPEN |
| LND-015 | MODERATE | Land | Strategic redeployment and paradrops | Not implemented | OPEN |
| LND-016 | MODERATE | Land | Commander traits and skill growth | Commanders contribute flat statistics | OPEN |
| LOG-006 | MODERATE | Logistics | Ports provide overseas supply and convoys sustain it | Overseas armies are unsupplied; no convoy network | OPEN |
| LOG-007 | MODERATE | Logistics | Motorisation extends hub distribution range | Motorisation level stored and settable, not simulated | OPEN |
| LOG-008 | MINOR | Logistics | Rail damage and repair | Railway level is static after construction | OPEN |
| MAP-008 | MINOR | Map | Rivers as graph edges with crossing penalties | Marsh is used as a crossing proxy | OPEN |
| IND-010 | MINOR | Industry | Factory conversion, dismantling, repair of damage | Damage and repair do not exist yet | OPEN |
| IND-011 | MINOR | Industry | Synthetic refineries and anti-air buildings | Buildable placeholders with no gameplay effect | OPEN |
| RES-005 | MINOR | Research | Doctrines and research bonus categories | Technologies are flat with modifiers | OPEN |
| COU-005 | MINOR | Country | Fuel as a nationally distributed stock | Fuel is produced, stored and drawn in division order | OPEN |
| IND-006 | MODERATE | Industry | Consumer goods modelled per factory assignment | Modelled as a capacity share (ratio) | OPEN |
| SAV-005 | MINOR | Persistence | Save migration between versions | Unknown versions are rejected with an actionable message | OPEN |
| SAV-006 | MINOR | Persistence | Named slots and autosave rotation | Single file, overwritten | OPEN |
| UI-005 | MINOR | UI | Battle detail and supply route visualisation | Combat/supply debug data exists in the model; the client shows only summaries | OPEN |
| UI-006 | MINOR | UI | Hotkeys, selection groups, multi-select | Single selection only | OPEN |
| UI-007 | MINOR | UI | Accessibility: UI scale, colour-blind palettes | Not implemented | OPEN |
| TOOL-005 | MINOR | Tooling | Inspectors and profiler capture history | Tick metrics exist; no inspector commands or captures | OPEN |
| TOOL-006 | MINOR | Tooling | Map and data editors | Data is edited by hand or regenerated | OPEN |
| MOD-002 | MODERATE | Modding | Deterministic mod load order with override diagnostics | Content loads from JSON; no mod layer or conflict reporting | OPEN |
| MOD-003 | MINOR | Modding | Mod test pack | Not present | OPEN |
| AI-007 | MODERATE | AI | AI air and naval doctrine | Layers absent with their systems | OPEN |
| AI-008 | MINOR | AI | Explicit AI crisis recovery behaviour | Posture scoring reacts to force ratios; no dedicated recovery planning | OPEN |
| SIM-006 | MINOR | Simulation | In-game speed control from the command line | Speed control exists in the served client only | OPEN |
| SIM-007 | MINOR | Simulation | Replay playback driver | Command log and replay file are written; playback is not exposed | OPEN |
| OCC-003 | MINOR | Occupation | Occupation policies affect resistance and output | Resistance/compliance exist without policy choice | OPEN |

## Closed

(none yet)

## Reproduction guidance

Applications of the zero-hiding rule (spec section 15): a discrepancy is never
removed because a workaround exists. For example LOG-006 means an overseas campaign
cannot be supplied today; that limitation is stated in the parity matrix
(`LOG-006 NOT_IMPLEMENTED`) and reflected in the AI, which avoids overseas wars it
cannot sustain.
