#pragma once
// Simulation phase functions and the authoritative tick order.
//
// Tick order is fixed and documented (docs/ARCHITECTURE.md). Each phase is a free
// function taking the Game. Phases never reorder state by pointer or hash order;
// every iteration walks entity stores in ascending id.

namespace hoi {

struct Game;

// 1. Apply queued commands (player and AI), recording results.
void phase_commands(Game& g);
// 2. Diplomatic drift, war creation, peace offers, capitulation resolution.
void phase_diplomacy(Game& g);
// 3. Division movement across the province graph.
void phase_movement(Game& g);
// 4. Incremental land combat: organisation/strength damage, reinforcement, retreat.
void phase_combat(Game& g);
// 5. Territorial control, occupation, capitulation of cut-off capitals.
void phase_territory(Game& g);
// 6. Supply network flow, division supply/fuel levels.
void phase_supply(Game& g);
// 6b. Air operations: sorties, air combat, missions, losses and air control.
void phase_air(Game& g);
// 6c. Naval operations: detection, engagements, missions, invasions, control.
void phase_naval(Game& g);
// 6d. Trade: balances, route feasibility, blockade and convoys.
void phase_trade(Game& g);
// 7. Resources, production lines, construction, consumer goods, stockpiles.
void phase_industry(Game& g);
// 8. Research progress and technology effect propagation.
void phase_research(Game& g);
// 8b. Division training: equipment/manpower consumption and readiness.
// Continuous replacement: divisions below strength draw the model the country issues
// for each slot's family from the stockpile. Runs after production so a line's output
// is visible to the same tick, and before the AI, which reads division strength.
void phase_reinforcement(Game& g);

// Intelligence: network growth and decay, operations in flight, decryption. Runs after
// politics so political power spent this tick is already accounted for.
void phase_intelligence(Game& g);

void phase_training(Game& g);
// 9. Political power, laws, stability/war support, manpower growth.
void phase_politics(Game& g);
// 9b. Focus progress and completion, and timed modifier expiry.
void phase_focuses(Game& g);
// 9c. Events (automatic firing, delayed firing) and decision timers.
void phase_events(Game& g);
// 9d. National spirits: upkeep and expiry of spirit effects.
void phase_spirits(Game& g);
// 10. Weather per strategic region.
void phase_weather(Game& g);
// 11. AI planning; issues commands into the queue for the next tick.
void phase_ai(Game& g);
// 12. Entity cleanup, invariant audit (debug), event log trimming.
void phase_cleanup(Game& g);

}  // namespace hoi
