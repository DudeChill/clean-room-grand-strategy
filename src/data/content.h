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
    double synthetic_oil_per_refinery_per_day = 2.0;
    double synthetic_rubber_per_refinery_per_day = 1.0;

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
    double construction_level_scaling = 1.25;  // cost factor added per existing level
    double max_factories_per_project = 15.0;  // capacity spreads across queued projects

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

    // Air.
    double air_base_capacity_per_level = 200.0;  // planes a level-1 air base hosts
    double air_sortie_hours = 6.0;               // hours per sortie cycle
    double air_cas_effect = 0.25;                // max additive land-attack bonus from CAS
    double air_superiority_effect = 0.20;        // max additive bonus from air control
    double air_bombing_industry_damage = 0.02;   // factories damaged per bombing hour
    double air_logistics_strike_damage = 0.05;   // railway level damaged per strike hour
    double air_anti_air_bombing_reduction = 0.15;  // damage cut per anti-air level: 1/(1+r*aa)
    double air_anti_air_combat_loss_factor = 0.10;  // extra air-combat losses per AA level
    double air_combat_scale = 0.6;              // attack strength -> hourly aircraft damage
    double air_aircraft_durability = 10.0;      // damage absorbed per aircraft
    double air_agility_weight = 0.25;           // agility advantage multiplier in air combat
    double air_combat_defence_floor = 20.0;     // floor term in the defence-share denominator
    double air_combat_roll_base = 0.5;          // randomness = base + next_double() (0.5..1.5)
    double air_experience_per_combat_hour = 0.002;
    double air_experience_per_mission_hour = 0.0005;
    double air_cas_organisation_damage = 0.05;  // org damage per unit of CAS power
    double air_cas_strength_damage = 0.02;      // HP damage per unit of CAS power
    double air_bombing_power_unit = 10.0;       // strike power per factory-hour
    double air_logistics_power_unit = 50.0;     // strike power per rail-level-hour
    double air_support_min_modifier = -0.25;    // clamp low for air_support_modifier
    double air_support_max_modifier = 0.50;     // clamp high for air_support_modifier
    double air_mission_weight_contested = 1.0;  // air-control weight: superiority/interception
    double air_mission_weight_support = 0.5;    // air-control weight: CAS/bombing/logistics/recon

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
