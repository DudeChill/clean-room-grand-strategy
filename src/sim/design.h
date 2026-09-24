#pragma once
// Equipment designers (spec section 50): a country fits components into an archetype,
// the engine computes the resulting statistics and registers a real EquipmentDef, so
// designs flow through production, stockpiles, divisions and combat like any model.

#include <string>
#include <vector>

#include "core/types.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// True when the country may design from this archetype: the archetype exists, the
// country can produce it (technology gate), and at least one component of every
// required slot family is unlocked.
bool design_available(const Game& g, CountryId country, EquipmentId archetype);

// Ids of the components the country may fit for that archetype's category, ascending.
std::vector<uint32_t> unlocked_components(const Game& g, CountryId country,
                                          EquipmentCategory category);

// True when the country may fit this component: the component's year has arrived and
// either it has no trigger, or the trigger passes, and any technology that names the
// component key in `unlock_equipment` has been researched.
bool component_unlocked(const Game& g, CountryId country, uint32_t component);

// Computes the statistics of `archetype` with `components` fitted. Pure: it does not
// touch Content. `components` maps slots to component indices; a slot left out keeps the
// archetype's value.
EquipmentDef design_compute(const Game& g, EquipmentId archetype,
                            const std::vector<std::pair<ComponentSlot, uint32_t>>& components,
                            const std::string& key, const std::string& name);

// Creates a design for the country: computes it, registers the resulting EquipmentDef in
// Content (so production lines and stockpiles can use it immediately) and records it in
// Country::designs. Validates availability and component legality. Returns the new
// design index, or an invalid index on failure (no state change).
uint32_t design_create(Game& g, CountryId country, const std::string& name,
                       EquipmentId archetype,
                       const std::vector<std::pair<ComponentSlot, uint32_t>>& components);

// True when a design of that key already exists for any country.
bool design_exists(const Game& g, const std::string& key);

// AI: build designs that improve the models the country is short of, using components
// it has unlocked; records reasons.
void ai_design_layer(Game& g, Country& c);

}  // namespace hoi
