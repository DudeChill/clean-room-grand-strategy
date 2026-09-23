#pragma once
// Land combat: incremental, organisation/strength based (spec sections 32-37).

#include <vector>

#include "core/types.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Statistics of a division instance, derived from its template and the equipment
// it actually has, including organisation modifiers from country technology/laws.
DivisionStats compute_division_stats(const Game& g, const Division& d);

// Effective attack/defense of a side in a battle, including terrain, planning,
// supply, experience, entrenchment and commander contributions. The breakdown is
// written into `debug` when non-null (spec section 35: combat debugger).
struct SideCombatValues {
    double soft_attack = 0.0;
    double hard_attack = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double width = 0.0;
    double hp = 0.0;
};

SideCombatValues compute_side_values(const Game& g, const Battle& b,
                                     const std::vector<DivisionId>& divisions,
                                     bool attacker, std::vector<BattleDebugLine>* debug);

// Creates a battle in `province` between the given sides when none exists.
BattleId start_battle(Game& g, ProvinceId province, CountryId attacker_lead,
                      CountryId defender_lead, const std::vector<DivisionId>& attackers,
                      const std::vector<DivisionId>& defenders);

// Removes `d` from its battle, clearing all references.
void detach_from_battle(Game& g, Division& d);

// Picks a legal retreat province for `d` (adjacent, controlled by its country or an
// ally, not controlled by an enemy, no hostile division present). Returns INVALID
// when the division is destroyed instead.
ProvinceId choose_retreat_province(const Game& g, const Division& d);

// Advances all battles by one hour: reinforcement into combat width, damage,
// retreats, battle end and province control transfer. Also starts new battles for
// divisions ordered to attack.
void phase_combat(Game& g);

// Applies combat losses to a division: strength, equipment and manpower.
void apply_combat_losses(Game& g, Division& d, double org_damage, double strength_damage);

// Organisation recovery for divisions not in combat (supply- and
// entrenchment-dependent).
void recover_organization(Game& g, Division& d);

}  // namespace hoi
