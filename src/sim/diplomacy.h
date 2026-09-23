#pragma once
// Diplomacy, war and occupation (spec sections 60-63, 174-176).

#include <vector>

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Declares war: creates a War, pulls in faction members, guarantees and puppets,
// and flips `at_war`/relation state. Idempotent per pair.
WarId declare_war(Game& g, CountryId aggressor, CountryId target,
                  const std::vector<WarGoal>& goals);

// True when the two countries are on opposite sides of an active war.
bool countries_at_war(const World& w, CountryId a, CountryId b);

// Faction membership. `join_faction` succeeds when the leader is a different, alive
// country that is not at war with `who`, `who` has no faction yet, and the ideology
// matches the leader's. Returns false without side effects otherwise.
bool join_faction(Game& g, CountryId who, CountryId faction_leader);
bool leave_faction(Game& g, CountryId who);

// Faction id led by `leader`, or 0 when there is none.
uint32_t faction_of(const World& w, CountryId leader);

// Countries fighting alongside `c` in any active war (allies, faction members).
std::vector<CountryId> co_belligerents(const World& w, CountryId c);

// Capitulation check for one country: capital and industry threshold lost.
// Returns true when the country capitulates in this call.
bool check_capitulation(Game& g, CountryId c);

// Transfers all territory of a capitulating country to the winner and removes it
// from play (units destroyed, wars closed, puppets released or transferred).
void capitulate(Game& g, CountryId loser, CountryId winner);

// Peace: ends the war and applies the goals that are satisfiable from current
// territorial control. Returns false when the war cannot end yet.
bool offer_peace(Game& g, WarId war, CountryId proposer);

// Per-tick diplomatic update: relation drift, war state maintenance, capitulation
// checks, peace AI hints.
void phase_diplomacy(Game& g);

// Occupation: resistance/compliance growth and garrison requirement per state.
void phase_occupation(Game& g, CountryId country);

// Updates territorial control for a single province based on who occupies it.
void update_province_control(Game& g, ProvinceId p);

}  // namespace hoi
