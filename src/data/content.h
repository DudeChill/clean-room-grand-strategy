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
#include "data/mod.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

struct Game;

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

// A national focus. Everything except its position and duration is a script block
// evaluated by the script engine (src/sim/script.h).
struct FocusDef {
    uint32_t index = 0;  // position in Content::focuses
    std::string key;
    std::string name;
    std::string tree;  // country tag or "shared"
    int x = 0;
    int y = 0;
    double days = 70.0;
    std::vector<std::string> prerequisites;         // focus keys
    std::vector<std::string> mutually_exclusive;    // focus keys
    Json available;  // trigger (null = always available)
    Json bypass;     // trigger (null = never bypassed)
    Json effects;    // effect block applied on completion
    double ai_weight = 1.0;  // base weight multiplies the scripted score
};

struct EventOptionDef {
    std::string name;
    Json effects;    // effect block
    double ai_weight = 1.0;
};

struct EventDef {
    uint32_t index = 0;
    std::string key;
    std::string title;
    std::string description;
    bool fire_only_once = true;
    bool major = false;  // pauses the game for a human player
    Json trigger;        // automatic firing condition (null = only fired by effects)
    std::vector<EventOptionDef> options;
    Json immediate;      // effects applied when the event fires, before the choice
};

struct DecisionDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    std::string description;
    int category = 0;
    bool targets_state = false;  // otherwise the decision is country-scoped
    double cost_pp = 0.0;
    int days_remove = 0;   // 0 = permanent while taken
    int days_cooldown = 0; // time before it may be taken again after removal
    Json visible;    // trigger: show the decision at all
    Json available;  // trigger: may be taken now
    Json effects;    // effect block when taken
    Json remove_effect;  // effect block when the timer runs out
    double ai_weight = 1.0;
};

// A national spirit: a permanent named modifier with an availability trigger.
struct SpiritDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    std::string description;
    int slots = 1;      // slots the spirit occupies
    Json available;     // trigger: may the country hold it
    Modifiers modifiers;
    Json effects;       // applied when the spirit is added (optional)
};

// A political advisor: bought with political power, occupies a slot, grants modifiers.
struct AdvisorDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    std::string description;
    double cost_pp = 150.0;
    Json available;     // trigger
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

    // Naval.
    double naval_base_capacity_per_level = 8.0;      // ships a level-1 naval base hosts
    double naval_detection_scale = 1.0;              // detection vs visibility spot chance
    double naval_combat_roll_base = 0.5;             // randomness = base + next_double()
    double naval_combat_scale = 0.60;                // attack power -> strength damage per hour
    double naval_org_damage_scale = 0.30;            // organisation damage per combat power
    double naval_torpedo_large_hull_bonus = 2.0;     // torpedo multiplier vs capital hulls
    double naval_sub_detection_penalty = 0.35;       // surface attack cut vs undetected subs
    double naval_aa_carrier_air_factor = 0.5;        // carrier-air damage cut per AA point
    double naval_air_attacks_per_hour = 0.25;        // carrier sortie power per hour
    double naval_screen_share_cap = 0.5;             // max share of hits screened
    double naval_retreat_strength_threshold = 0.35;  // average strength to retreat
    double naval_retreat_org_threshold = 0.20;
    double naval_repair_per_hour = 0.01;             // strength recovered per hour in port
    double naval_repair_org_per_hour = 0.02;
    double naval_repair_cost_fuel = 0.5;             // fuel per strength point repaired
    double naval_repair_cost_stockpile_share = 0.02; // share of build_cost per strength point
    double naval_fuel_use_per_hour = 0.01;           // fuel per ship per hour at sea
    double naval_training_experience_per_hour = 0.0004;
    double naval_combat_experience_per_hour = 0.002;
    double naval_raid_convoy_damage = 0.02;          // convoys sunk per raider power-hour
    double naval_raid_control_cut = 0.5;             // control multiplier for the raided side
    double naval_escort_protection = 0.5;            // raider damage cut per escort ratio
    double naval_max_engagement_ships = 200;         // ships fighting per zone per hour
    double naval_large_hull_hp = 800.0;              // HP per unit above which a hull is "large" 
    double naval_base_supply_per_level = 0.5;        // port supply capacity per base level
    double naval_supply_sea_range_penalty = 0.15;    // capacity decay per sea-zone hop
    double naval_supply_convoy_use_per_capacity = 0.02;  // convoys drawn per capacity-hour
    double naval_supply_raid_threshold = 2.0;        // raider strength that zeroes a route
    double naval_invasion_convoys_per_division = 10.0;      // convoy units per division embarked
    double naval_invasion_hours_per_sea_hop = 12.0;         // crossing speed, hours per sea hop
    double naval_invasion_interception_base = 0.5;          // base interception chance scale
    double naval_invasion_interception_threat_scale = 200.0;  // threat/(threat+scale)
    double naval_invasion_escort_mitigation = 0.5;          // support task forces cut interception

    // Politics.
    double political_power_per_day = 2.0;
    double focus_progress_speed = 1.0;  // days of focus progress per elapsed day
    double stability_drift = 0.01;
    double war_support_drift = 0.01;

    // Weather.
    double weather_change_chance = 0.15;

    // Trade (spec section 46). These are appended at the END of the struct because
    // the save serializer stores SimConstants as a fixed-order value list.
    double trade_convoy_use_per_unit = 0.05;     // convoy units per resource unit per day (sea)
    double trade_factory_cost_per_unit = 0.125;  // civilian factories tied up per resource unit/day
    double trade_factory_cost_law_scale = 0.10;  // factory cost cut per trade-law level

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

// ----------------------------------------------------------- intelligence ----

enum class OperationKind : uint8_t {
    BuildNetwork = 0,   // raise a network's strength in the target
    StealTech,          // grant research progress in a category
    SabotageIndustry,   // cut the target's factory output for a while
    SupportIdeology,    // shift the target's ruling-party support
    Destabilise,        // cut the target's stability
    CounterIntel,       // hunt enemy networks at home
    Count
};
const char* operation_kind_name(OperationKind kind);
bool match_operation_kind(const std::string& name, OperationKind* out);

// One intelligence operation a country can run against another. Everything is data:
// the engine reads these fields and never special-cases a key.
struct OperationDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    std::string description;
    OperationKind kind = OperationKind::BuildNetwork;
    int days = 30;                 // work days at full pace
    double pp_cost = 0.0;          // political power spent when the operation starts
    double civilian_cost = 0.0;    // civilian factories tied up while it runs
    double network_required = 0.0; // network strength needed in the target
    double risk = 0.1;             // 0..1 chance the network is burned on completion
    // Effects. Exactly which ones a kind uses is data, not code.
    double network_gain = 0.0;     // strength added to the network in the target
    double research_days = 0.0;    // research progress granted (StealTech)
    double output_penalty = 0.0;   // factory output modifier applied to the target
    double stability_delta = 0.0;  // target stability change
    double ideology_shift = 0.0;   // target ruling-party support change
    int effect_days = 90;          // how long the timed effect lasts
    Json available;                // optional trigger (country scope)
    Json effect;                   // optional script effects, run against the target
};

// An agency upgrade: what the country's intelligence service can do. Bought with
// political power, gated by year, technology or a trigger, and by other upgrades.
struct AgencyUpgradeDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    std::string description;
    int year = 1936;
    double pp_cost = 0.0;
    std::vector<std::string> requires_upgrades;  // keys that must be owned first
    double network_growth = 0.0;        // network strength per day, added to the base
    double operation_speed = 0.0;       // fraction faster operations run (0.25 = +25%)
    double crypto_speed = 0.0;          // decryption progress per day, added to the base
    double counter_intel = 0.0;         // enemy network growth cut at home (0..1)
    Json available;                     // optional trigger (country scope)
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
    std::vector<FocusDef> focuses;
    std::map<std::string, uint32_t> focus_index;  // focus key -> index
    std::vector<EventDef> events;
    std::map<std::string, uint32_t> event_index;
    std::vector<DecisionDef> decisions;
    std::map<std::string, uint32_t> decision_index;
    std::vector<ComponentDef> components;
    std::map<std::string, uint32_t> component_index;
    std::vector<EquipmentDesign> designs;  // created at runtime by countries
    std::map<std::string, uint32_t> design_index;
    // Intelligence tables. Operations are the actions a country may run against another;
    // agency upgrades are what its service can do.
    std::vector<OperationDef> operations;
    std::map<std::string, uint32_t> operation_index;
    std::vector<AgencyUpgradeDef> agency_upgrades;
    std::map<std::string, uint32_t> agency_upgrade_index;
    // Reverse link, derived: the equipment a design produces -> that design's index.
    // Built by rebuild_content_index on load and maintained by design_create, so
    // `equipment_unlocked` can answer "is this another country's design?" without
    // scanning the design table in hot paths.
    std::map<uint32_t, uint32_t> design_of_equipment;
    std::vector<SpiritDef> spirits;
    std::map<std::string, uint32_t> spirit_index;
    std::vector<AdvisorDef> advisors;
    std::map<std::string, uint32_t> advisor_index;
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
    [[nodiscard]] const FocusDef* focus(uint32_t index) const {
        return index < focuses.size() ? &focuses[index] : nullptr;
    }
    [[nodiscard]] uint32_t focus_id(const std::string& key) const {
        auto it = focus_index.find(key);
        return it == focus_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const EventDef* event(uint32_t index) const {
        return index < events.size() ? &events[index] : nullptr;
    }
    [[nodiscard]] uint32_t event_id(const std::string& key) const {
        auto it = event_index.find(key);
        return it == event_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const ComponentDef* component(uint32_t index) const {
        return index < components.size() ? &components[index] : nullptr;
    }
    [[nodiscard]] uint32_t component_id(const std::string& key) const {
        auto it = component_index.find(key);
        return it == component_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const EquipmentDesign* design(uint32_t index) const {
        return index < designs.size() ? &designs[index] : nullptr;
    }
    [[nodiscard]] uint32_t design_id(const std::string& key) const {
        auto it = design_index.find(key);
        return it == design_index.end() ? 0xFFFFFFFFu : it->second;
    }
    const OperationDef* operation(uint32_t index) const {
        return index < operations.size() ? &operations[index] : nullptr;
    }
    uint32_t operation_id(const std::string& key) const {
        auto it = operation_index.find(key);
        return it == operation_index.end() ? 0xFFFFFFFFu : it->second;
    }
    const AgencyUpgradeDef* agency_upgrade(uint32_t index) const {
        return index < agency_upgrades.size() ? &agency_upgrades[index] : nullptr;
    }
    uint32_t agency_upgrade_id(const std::string& key) const {
        auto it = agency_upgrade_index.find(key);
        return it == agency_upgrade_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const SpiritDef* spirit(uint32_t index) const {
        return index < spirits.size() ? &spirits[index] : nullptr;
    }
    [[nodiscard]] uint32_t spirit_id(const std::string& key) const {
        auto it = spirit_index.find(key);
        return it == spirit_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const AdvisorDef* advisor(uint32_t index) const {
        return index < advisors.size() ? &advisors[index] : nullptr;
    }
    [[nodiscard]] uint32_t advisor_id(const std::string& key) const {
        auto it = advisor_index.find(key);
        return it == advisor_index.end() ? 0xFFFFFFFFu : it->second;
    }
    [[nodiscard]] const DecisionDef* decision(uint32_t index) const {
        return index < decisions.size() ? &decisions[index] : nullptr;
    }
    [[nodiscard]] uint32_t decision_id(const std::string& key) const {
        auto it = decision_index.find(key);
        return it == decision_index.end() ? 0xFFFFFFFFu : it->second;
    }
};

// Loads engine constants + equipment + technologies + laws + building definitions.
// Returns false when a hard error occurred (missing base file, duplicate id).
//
// `mod_roots` are mods roots (each holding `<root>/<mod>/mod.json`), exactly as the
// CLI `--mods` takes them; they are discovered, ordered deterministically and merged
// after the base content. A mod that fails validation is skipped as a whole and its
// diagnostics are appended to Content::load_errors (and to `report` when one is
// passed) — it never leaves half-applied content behind. An empty list loads the
// base content alone. Definitions already present are replaced in place, entries
// marked `"add"` append, and `replace_paths` tables come only from that mod.
bool load_content(const std::string& data_root, Content* out, std::string* err,
                  const std::vector<std::string>& mod_roots = {},
                  ModLoadReport* report = nullptr);

// Loads a scenario (map, states, countries, starting industry and armies) into a
// freshly constructed world. Country/province/state data is data-driven.
bool load_scenario(const std::string& scenario_path, Content& content, World* world,
                   std::string* err);

// Second scenario pass that needs the finished Game (task forces and other content
// that must be created through the same helpers the commands use). Called by
// Game::create right after load_scenario.
bool load_scenario_forces(const std::string& scenario_path, Game& g, std::string* err);

}  // namespace hoi
