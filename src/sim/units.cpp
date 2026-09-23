// Division templates, derived statistics and the shared modifier vocabulary.
//
// Stat aggregation rules (ARCHITECTURE section 5.4-5.5 plus HOI4 division design):
//  * additive stats (soft/hard attack, defense, breakthrough, HP, economy) sum
//    over battalions, scaled by the battalion's unit count;
//  * armor and piercing are the maximum over battalions (one heavily armoured
//    battalion armours the whole division);
//  * hardness is the unit-count weighted average over battalions;
//  * organisation is the average organisation per battalion;
//  * speed is the minimum over line battalions (support companies never slow a
//    division down);
//  * combat width is 2 per line battalion; support companies add no width.

#include "sim/units.h"

#include <algorithm>
#include <cmath>

#include "core/math.h"

namespace hoi {

namespace {

// Combat width one line battalion occupies.
constexpr double kLineBattalionWidth = 2.0;

}  // namespace

const char* modifier_kind_name(ModifierKind k) {
    switch (k) {
        case ModifierKind::FactoryOutput: return "FactoryOutput";
        case ModifierKind::DockyardOutput: return "DockyardOutput";
        case ModifierKind::ConstructionSpeed: return "ConstructionSpeed";
        case ModifierKind::ResearchSpeed: return "ResearchSpeed";
        case ModifierKind::DivisionOrganization: return "DivisionOrganization";
        case ModifierKind::DivisionAttack: return "DivisionAttack";
        case ModifierKind::DivisionDefense: return "DivisionDefense";
        case ModifierKind::DivisionBreakthrough: return "DivisionBreakthrough";
        case ModifierKind::DivisionRecoveryRate: return "DivisionRecoveryRate";
        case ModifierKind::SupplyConsumption: return "SupplyConsumption";
        case ModifierKind::MaxPlanning: return "MaxPlanning";
        case ModifierKind::PlanningSpeed: return "PlanningSpeed";
        case ModifierKind::ManpowerGrowth: return "ManpowerGrowth";
        case ModifierKind::PoliticalPowerGain: return "PoliticalPowerGain";
        case ModifierKind::FuelGain: return "FuelGain";
        case ModifierKind::TrainingTime: return "TrainingTime";
        case ModifierKind::EquipmentCostFactor: return "EquipmentCostFactor";
        case ModifierKind::RecruitablePopulation: return "RecruitablePopulation";
        case ModifierKind::Stability: return "Stability";
        case ModifierKind::WarSupport: return "WarSupport";
        case ModifierKind::EntrenchmentSpeed: return "EntrenchmentSpeed";
        case ModifierKind::CombatWidth: return "CombatWidth";
        case ModifierKind::ConvoyDefense: return "ConvoyDefense";
        case ModifierKind::Count: return "Count";
    }
    return "Unknown";
}

bool Modifiers::empty() const {
    for (int i = 0; i < static_cast<int>(ModifierKind::Count); ++i) {
        if (v[i] != 0.0) return false;
    }
    return true;
}

void recompute_template_stats(DivisionTemplate& t, const std::vector<EquipmentDef>& equipment) {
    double soft = 0.0;
    double hard = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double hardness_num = 0.0;
    double hardness_den = 0.0;
    double org_num = 0.0;
    double org_den = 0.0;
    double hp = 0.0;
    double width = 0.0;
    double supply = 0.0;
    double fuel = 0.0;
    double cost = 0.0;
    double manpower = 0.0;
    double speed = 0.0;
    bool have_speed = false;

    for (const BattalionSlot& slot : t.battalions) {
        if (slot.count <= 0) continue;
        const EquipmentDef* def = nullptr;
        if (slot.equipment.valid() && slot.equipment.v < equipment.size()) {
            def = &equipment[slot.equipment.v];
        }
        const double n = static_cast<double>(slot.count);

        if (def) {
            soft += def->soft_attack * n;
            hard += def->hard_attack * n;
            defense += def->defense * n;
            breakthrough += def->breakthrough * n;
            if (def->armor > armor) armor = def->armor;
            if (def->piercing > piercing) piercing = def->piercing;
            hardness_num += def->hardness * n;
            org_num += def->organization * n;
            hp += def->max_strength * n;
            supply += def->supply_use * n;
            fuel += def->fuel_use * n;
            cost += def->build_cost * n;
            manpower += def->manpower * n;

            hardness_den += n;
            org_den += n;
        }

        if (!slot.support) {
            width += kLineBattalionWidth * n;
            if (def && (!have_speed || def->speed < speed)) {
                speed = def->speed;
                have_speed = true;
            }
        }
    }

    t.soft_attack = soft;
    t.hard_attack = hard;
    t.defense = defense;
    t.breakthrough = breakthrough;
    t.armor = armor;
    t.piercing = piercing;
    t.hardness = clamp01(safe_div(hardness_num, hardness_den));
    t.max_organization = safe_div(org_num, org_den);
    t.max_strength = hp;
    t.combat_width = width;
    t.speed = have_speed ? speed : 0.0;
    t.supply_use = supply;
    t.fuel_use = fuel;
    t.build_cost = cost;
    t.manpower = manpower;
    // `train_days` is data-driven; the signature has no Content/SimConstants and
    // SimConstants carries no training constant, so the value is preserved.
}

}  // namespace hoi
