#pragma once
// Research: technology state that propagates into real systems (spec section 49).
//
// Completing a technology applies its modifiers to the country and unlocks the
// equipment and building keys it names, which industry can then produce.

#include <vector>

#include "core/types.h"
#include "data/content.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// True when every prerequisite is completed and the tech is not already known.
bool tech_available(const Game& g, CountryId country, TechId tech);

// Research days required for a technology for this country (year scaling plus
// research-speed modifiers).
double tech_cost_days(const Game& g, CountryId country, const TechId tech);

// Applies a completed technology: modifiers, unlocked equipment/buildings.
void apply_tech_effects(Game& g, Country& country, TechId tech);

// Advances research slots one hour: progress, completion, effect propagation.
void phase_research(Game& g);

// True when the country can build `key` (technology gate).
bool building_unlocked(const Game& g, CountryId country, const std::string& building_key);

// True when the country may produce `equipment` (technology gate).
bool equipment_unlocked(const Game& g, CountryId country, EquipmentId equipment);

}  // namespace hoi
