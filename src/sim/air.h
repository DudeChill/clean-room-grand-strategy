#pragma once
// Air warfare (spec section 51-52). See docs/mechanics/air_warfare.md for the
// specification this implements.
//
// Air control is derived every tick from the wings actually flying in a region, so
// there is never a second source of truth that can disagree with the wings.

#include <vector>

#include "core/types.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Planes a province's air bases can host (air base level * capacity per level).
int air_base_capacity(const Game& g, ProvinceId province);

// Planes currently stationed at a province across all countries.
int planes_stationed_at(const Game& g, ProvinceId province);

// Distance in strategic-region hops between two regions (for range checks); 0 for
// the same region, -1 when there is no land path between them.
int region_distance_hops(const Game& g, RegionId from, RegionId to);

// True when a wing based in `base` may fly a mission in `region` (the region is
// within the model's range of the base and the base is controlled by the owner).
bool wing_can_reach(const Game& g, const AirWing& wing, RegionId region);

// Air control share (0..1) of `country` in `region`, derived from planes present,
// their efficiency and their mission. 0 when nobody flies there for that country.
double air_control_share(const Game& g, CountryId country, RegionId region);

// Convenience for land combat: the additive attack bonus (+) or penalty (-) a side
// receives from air support in the province's region. Attackers benefit from CAS,
// both sides from superiority. `country` is the side's country.
double air_support_modifier(const Game& g, CountryId country, ProvinceId province, bool attacker);

// Advances all air operations by one hour: sortie generation, air combat between
// hostile wings in the same region, mission effects (CAS damage, bombing of
// industry and logistics, reconnaissance), losses, replacements from the stockpile
// and air-control recomputation per region.
void phase_air(Game& g);

// Replacement draw: an air wing takes aircraft from its country's stockpile.
// Returns the number of planes actually delivered.
int reinforce_air_wing(Game& g, AirWing& wing);

// Removes a wing: planes go back to the stockpile and the wing leaves the world.
void disband_air_wing(Game& g, AirWingId id);

}  // namespace hoi