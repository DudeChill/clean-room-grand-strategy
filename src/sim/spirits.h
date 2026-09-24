#pragma once
// National spirits and advisors (spec section 56): permanent named modifiers that shape
// a country, and the political appointments a country buys with political power.

#include <string>
#include <vector>

#include "core/types.h"
#include "data/content.h"

namespace hoi {

struct Game;

// Spirits.
bool spirit_available(const Game& g, CountryId country, uint32_t spirit);
bool spirit_add(Game& g, CountryId country, uint32_t spirit);
// Raw grant used by script effects: skips the availability trigger and any cost, but
// still requires a free slot (slots are an invariant, not a preference).
bool spirit_grant(Game& g, CountryId country, uint32_t spirit);
bool spirit_remove(Game& g, CountryId country, uint32_t spirit);
[[nodiscard]] bool has_spirit(const World& w, CountryId country, const std::string& key);
// Free spirit slots (SpiritDef::slots) with the ones already held.
int spirit_slots_free(const Game& g, CountryId country);

// Advisors.
bool advisor_available(const Game& g, CountryId country, uint32_t advisor);
bool advisor_appoint(Game& g, CountryId country, uint32_t advisor);
// Raw appointment for script effects: no political-power cost and no trigger check,
// but a free advisor slot is still required.
bool advisor_grant(Game& g, CountryId country, uint32_t advisor);
void advisor_dismiss(Game& g, CountryId country, uint32_t advisor);
[[nodiscard]] bool has_advisor(const World& w, CountryId country, const std::string& key);
int advisor_slots_free(const Game& g, CountryId country);

// AI: appoint advisors and add spirits the country can afford and benefit from.
void ai_spirit_advisor_layer(Game& g, Country& c);

}  // namespace hoi
