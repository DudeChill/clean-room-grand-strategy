#pragma once
// Naval warfare (spec sections 53-55). See docs/mechanics/naval_warfare.md for the
// specification this implements.

#include <vector>

#include "core/types.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Ships a province's naval base can host (naval base level * capacity per level).
int naval_base_capacity(const Game& g, ProvinceId province);

// True when the province is a usable naval base for `country` (coastal, has a naval
// base, controlled by the country or a co-belligerent).
bool is_usable_port(const Game& g, CountryId country, ProvinceId province);

// Sea zone adjacent to a coastal province, or an invalid id when landlocked.
RegionId adjacent_sea_region(const Game& g, ProvinceId province);

// Distance in sea-zone hops between two sea regions; 0 for the same region, -1 when
// there is no sea path.
int sea_region_distance(const Game& g, RegionId from, RegionId to);

// Naval control share (0..1) of `country` in a sea zone, derived from the ships
// present and their mission. Zero when the country has nothing at sea there.
double naval_control_share(const Game& g, CountryId country, RegionId region);

// Sum of a task force's ship statistics, used by the AI and the debugger.
struct TaskForceStats {
    double naval_attack = 0.0;
    double torpedo_attack = 0.0;
    double sub_detection = 0.0;
    double detection = 0.0;
    double visibility = 0.0;
    double armour = 0.0;
    double hull = 0.0;  // total remaining strength-weighted hit points
    int ships = 0;
};

TaskForceStats task_force_stats(const Game& g, TaskForceId id);

// Advances naval operations by one hour: detection between hostile task forces in a
// sea zone, engagement, damage, sinking, retreat to port, repair in port, convoy
// raiding against enemy shipping, fuel consumption, and recomputation of naval
// control per sea zone. Also advances naval invasions.
void phase_naval(Game& g);

// Naval invasion lifecycle: load divisions at a friendly port, cross the sea zone
// (interception damages the convoy), land on the target coast or abort. Called by
// phase_naval; exposed so tests can drive one step.
void phase_naval_invasion(Game& g);

// Launches an invasion for an army (validates ports, sea route and convoy stock).
bool start_naval_invasion(Game& g, ArmyId army, ProvinceId origin, ProvinceId target);

// Cancels an invasion, returning any embarked divisions to the origin port.
void cancel_naval_invasion(Game& g, ArmyId army);

// Removes a ship from the world (sunk or scrapped): leaves its task force, fleet and
// country rosters clean.
void destroy_ship(Game& g, ShipId id);

// Forms a task force of up to `count` ships of `equipment` at a port; used by the
// CreateTaskForce command. Returns the new task force id (invalid on failure).
TaskForceId form_task_force(Game& g, CountryId country, ProvinceId port, EquipmentId equipment,
                            int count, const std::string& name);

}  // namespace hoi
