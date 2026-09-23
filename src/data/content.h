#pragma once
// Content database: everything that is data rather than engine rule.
//
// Countries, provinces, states, equipment, technologies, laws and scenario setup
// load from JSON so that mods can add or override content without recompiling the
// engine. Engine code addresses content by stable string keys and integer ids,
// never by hardcoded historical knowledge.

#include <map>
#include <string>
#include <vector>

#include "core/json.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct TechDef {
    TechId id;
    std::string key;
    std::string name;
    std::string category;  // "industry", "land_doctrine", "infantry", ...
    int year = 1936;
    double cost_days = 100.0;
    std::vector<TechId> prerequisites;
    std::vector<std::string> unlock_equipment;  // equipment keys unlocked on completion
    std::vector<std::string> unlock_buildings;  // building keys unlocked
    Modifiers modifiers;
};

// Tunable simulation constants. All balance-relevant numbers live here so that
// data can be tuned without touching engine code (spec section 168).
struct SimConstants {
    // Industry.
    double ic_per_military_factory = 4.5;
    double ic_per_civilian_factory = 5.0;
    double ic_per_dockyard = 2.5;
    double efficiency_start = 0.10;
    double efficiency_cap_base = 0.50;
    double efficiency_cap_growth_per_day = 0.001667;
    double efficiency_growth_per_day = 0.20;
    double switch_same_archetype_retention = 0.70;
    double resource_shortage_floor = 0.10;
    double consumer_goods_base = 0.35;
    double fuel_per_oil = 1.0;
    double fuel_storage_per_factory = 200.0;

    // Construction.
    double construction_cost_factory = 10800.0;
    double construction_cost_infrastructure = 3000.0;
    double construction_cost_railway = 4000.0;
    double construction_cost_supply_hub = 6000.0;
    double construction_cost_air_base = 2400.0;
    double construction_cost_naval_base = 3000.0;
    double construction_cost_fort = 2000.0;
    double construction_cost_radar = 2400.0;
    double construction_cost_synthetic = 8000.0;
    double construction_level_scaling = 1.25;  // cost multiplier per existing level

    // Research.
    double research_base_days = 100.0;
    double research_year_penalty = 0.0;  // ahead-of-time penalty per year
    double research_speed_base = 1.0;

    // Manpower.
    double manpower_growth_per_year_fraction = 0.01;  // share of population per year
    double recruitable_base = 0.025;

    // Movement.
    double base_hours_per_province = 24.0;
    double min_division_speed = 1.0;
    double river_crossing_penalty = 0.30;

    // Combat.
    double combat_width_base = 80.0;
    double damage_scale = 0.06;
    double org_damage_share = 0.75;
    double strength_damage_share = 0.25;
    double armor_advantage_multiplier = 1.50;
    double armor_disadvantage_multiplier = 0.50;
    double org_recovery_base = 0.05;         // org per hour when rested and supplied
    double entrenchment_per_day = 0.10;
    double planning_per_day = 0.02;
    double planning_max_attack_bonus = 0.30;
    double battle_retreat_org_threshold = 0.0;
    double max_battles_per_province = 1.0;

    // Logistics.
    double supply_hub_radius = 6.0;         // provinces of effective range
    double supply_range_penalty = 0.10;     // capacity lost per province of distance
    double supply_demand_per_width = 1.0;
    double supply_rail_bonus_per_level = 0.15;
    double supply_infrastructure_bonus_per_level = 0.05;
    double fuel_demand_per_day = 0.02;      // share of division fuel_use per hour scale

    // Politics.
    double political_power_per_day = 2.0;
    double stability_drift = 0.01;
    double war_support_drift = 0.01;

    // Weather.
    double weather_change_chance = 0.15;

    static SimConstants from_json(const Json& j);
};

struct LawDef {
    std::string key;
    std::string name;
    int kind = 0;  // index into Country::law_levels
    int level = 0;
    double cost = 0.0;  // political power
    Modifiers modifiers;
    std::string requires_law;  // law key that must already be enacted (optional)
    int requires_level = -1;
};

struct BuildingDef {
    BuildingKind kind = BuildingKind::CivilianFactory;
    std::string key;
    std::string name;
    double base_cost = 0.0;
    bool per_state = true;
    int max_level = 10;
};

struct Content {
    std::vector<EquipmentDef> equipment;  // indexed by EquipmentId
    std::map<std::string, EquipmentId> equipment_by_key;
    std::vector<DivisionTemplate> templates;  // indexed by TemplateId (scenario templates)
    std::map<std::string, TemplateId> template_by_key;
    std::vector<TechDef> techs;  // indexed by TechId
    std::map<std::string, TechId> tech_by_key;
    std::vector<LawDef> laws;
    std::map<std::string, int> law_index;  // key -> index in `laws`
    std::vector<BuildingDef> buildings;
    SimConstants constants;
    std::vector<std::string> load_errors;  // actionable, file:line + reason

    [[nodiscard]] const EquipmentDef* equipment_def(EquipmentId id) const {
        return id.valid() && id.v < equipment.size() ? &equipment[id.v] : nullptr;
    }
    [[nodiscard]] const DivisionTemplate* template_def(TemplateId id) const {
        return id.valid() && id.v < templates.size() ? &templates[id.v] : nullptr;
    }
    [[nodiscard]] const TechDef* tech_def(TechId id) const {
        return id.valid() && id.v < techs.size() ? &techs[id.v] : nullptr;
    }
    [[nodiscard]] EquipmentId equipment_id(const std::string& key) const {
        auto it = equipment_by_key.find(key);
        return it == equipment_by_key.end() ? EquipmentId{} : it->second;
    }
    [[nodiscard]] TechId tech_id(const std::string& key) const {
        auto it = tech_by_key.find(key);
        return it == tech_by_key.end() ? TechId{} : it->second;
    }
    [[nodiscard]] TemplateId template_id(const std::string& key) const {
        auto it = template_by_key.find(key);
        return it == template_by_key.end() ? TemplateId{} : it->second;
    }
    [[nodiscard]] const LawDef* law(const std::string& key) const {
        auto it = law_index.find(key);
        return it == law_index.end() ? nullptr : &laws[it->second];
    }
};

// Loads engine constants + equipment + technologies + laws + building definitions.
// Returns false when a hard error occurred (missing file, duplicate id).
bool load_content(const std::string& data_root, Content* out, std::string* err);

// Loads a scenario (map, states, countries, starting industry and armies) into a
// freshly constructed world. Country/province/state data is data-driven.
bool load_scenario(const std::string& scenario_path, Content& content, World* world,
                   std::string* err);

}  // namespace hoi
