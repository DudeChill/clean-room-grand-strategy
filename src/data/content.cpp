// Content database loading (spec section 168).
//
// All files are optional in shape (unknown keys are ignored) but required in
// existence: load_content returns false only for a missing/unparsable file, a
// duplicate key, or an entry without a key. Everything else is recorded in
// Content::load_errors as "<file>:<key>: <reason>" so a modder can fix it without
// a debugger.

#include "data/content.h"

#include <cctype>
#include <string>
#include <vector>

namespace hoi {
namespace {

// Names of data keys are matched case- and separator-insensitively against the
// engine enum names ("DivisionAttack", "division_attack" and "division attack"
// all resolve to the same ModifierKind). This keeps data readable while the
// engine keeps one canonical spelling.
std::string normalize_name(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

template <typename NameFn>
int match_enum_name(NameFn name_fn, int count, const std::string& key) {
    const std::string want = normalize_name(key);
    if (want.empty()) return -1;
    for (int i = 0; i < count; ++i) {
        if (normalize_name(name_fn(i)) == want) return i;
    }
    return -1;
}

int match_modifier(const std::string& key) {
    return match_enum_name(
        [](int i) { return modifier_kind_name(static_cast<ModifierKind>(i)); },
        static_cast<int>(ModifierKind::Count), key);
}

int match_equipment_category(const std::string& key) {
    return match_enum_name(
        [](int i) { return equipment_category_name(static_cast<EquipmentCategory>(i)); },
        static_cast<int>(EquipmentCategory::Count), key);
}

int match_building_kind(const std::string& key) {
    return match_enum_name(
        [](int i) { return building_kind_name(static_cast<BuildingKind>(i)); },
        static_cast<int>(BuildingKind::Count), key);
}

// Parses a modifier object: {"DivisionAttack": 0.05, ...}.
Modifiers parse_modifiers(const Json& j, std::vector<std::string>* errors,
                          const std::string& file, const std::string& key) {
    Modifiers m;
    if (!j.is_object()) return m;
    for (const auto& item : j.object_items()) {
        const int idx = match_modifier(item.first);
        if (idx < 0) {
            errors->push_back(file + ":" + key + ": unknown modifier " + item.first);
            continue;
        }
        m.add(static_cast<ModifierKind>(idx), item.second.as_double(0.0));
    }
    return m;
}

void parse_resource_costs(const Json& j, double out[RESOURCE_COUNT],
                          std::vector<std::string>* errors, const std::string& file,
                          const std::string& key) {
    for (int i = 0; i < RESOURCE_COUNT; ++i) out[i] = 0.0;
    if (!j.is_object()) return;
    for (const auto& item : j.object_items()) {
        const int idx = match_enum_name([](int i) { return resource_name(static_cast<Resource>(i)); },
                                        RESOURCE_COUNT, item.first);
        if (idx < 0) {
            errors->push_back(file + ":" + key + ": unknown resource " + item.first);
            continue;
        }
        out[idx] = item.second.as_double(0.0);
    }
}

bool load_json_file(const std::string& path, Json* out, std::string* err) {
    std::string parse_err;
    if (Json::parse_file(path, out, &parse_err)) return true;
    if (err) *err = path + ": " + (parse_err.empty() ? std::string("cannot read file") : parse_err);
    return false;
}

}  // namespace

SimConstants SimConstants::from_json(const Json& j) {
    SimConstants c;
    if (!j.is_object()) return c;
    auto num = [&j](const char* key, double fallback) {
        return j.has(key) ? j[key].as_double(fallback) : fallback;
    };
    c.ic_per_military_factory = num("ic_per_military_factory", c.ic_per_military_factory);
    c.ic_per_civilian_factory = num("ic_per_civilian_factory", c.ic_per_civilian_factory);
    c.ic_per_dockyard = num("ic_per_dockyard", c.ic_per_dockyard);
    c.efficiency_start = num("efficiency_start", c.efficiency_start);
    c.efficiency_cap_base = num("efficiency_cap_base", c.efficiency_cap_base);
    c.efficiency_cap_growth_per_day =
        num("efficiency_cap_growth_per_day", c.efficiency_cap_growth_per_day);
    c.efficiency_growth_per_day = num("efficiency_growth_per_day", c.efficiency_growth_per_day);
    c.switch_same_archetype_retention =
        num("switch_same_archetype_retention", c.switch_same_archetype_retention);
    c.resource_shortage_floor = num("resource_shortage_floor", c.resource_shortage_floor);
    c.consumer_goods_base = num("consumer_goods_base", c.consumer_goods_base);
    c.fuel_per_oil = num("fuel_per_oil", c.fuel_per_oil);
    c.fuel_storage_per_factory = num("fuel_storage_per_factory", c.fuel_storage_per_factory);
    c.synthetic_oil_per_refinery_per_day =
        num("synthetic_oil_per_refinery_per_day", c.synthetic_oil_per_refinery_per_day);
    c.synthetic_rubber_per_refinery_per_day =
        num("synthetic_rubber_per_refinery_per_day", c.synthetic_rubber_per_refinery_per_day);

    c.air_base_capacity_per_level =
        num("air_base_capacity_per_level", c.air_base_capacity_per_level);
    c.air_sortie_hours = num("air_sortie_hours", c.air_sortie_hours);
    c.air_cas_effect = num("air_cas_effect", c.air_cas_effect);
    c.air_superiority_effect = num("air_superiority_effect", c.air_superiority_effect);
    c.air_bombing_industry_damage =
        num("air_bombing_industry_damage", c.air_bombing_industry_damage);
    c.air_logistics_strike_damage =
        num("air_logistics_strike_damage", c.air_logistics_strike_damage);
    c.air_anti_air_bombing_reduction =
        num("air_anti_air_bombing_reduction", c.air_anti_air_bombing_reduction);
    c.air_anti_air_combat_loss_factor =
        num("air_anti_air_combat_loss_factor", c.air_anti_air_combat_loss_factor);
    c.air_combat_scale = num("air_combat_scale", c.air_combat_scale);
    c.air_aircraft_durability = num("air_aircraft_durability", c.air_aircraft_durability);
    c.air_agility_weight = num("air_agility_weight", c.air_agility_weight);
    c.air_combat_defence_floor = num("air_combat_defence_floor", c.air_combat_defence_floor);
    c.air_combat_roll_base = num("air_combat_roll_base", c.air_combat_roll_base);
    c.air_experience_per_combat_hour =
        num("air_experience_per_combat_hour", c.air_experience_per_combat_hour);
    c.air_experience_per_mission_hour =
        num("air_experience_per_mission_hour", c.air_experience_per_mission_hour);
    c.air_cas_organisation_damage =
        num("air_cas_organisation_damage", c.air_cas_organisation_damage);
    c.air_cas_strength_damage = num("air_cas_strength_damage", c.air_cas_strength_damage);
    c.air_bombing_power_unit = num("air_bombing_power_unit", c.air_bombing_power_unit);
    c.air_logistics_power_unit = num("air_logistics_power_unit", c.air_logistics_power_unit);
    c.air_support_min_modifier = num("air_support_min_modifier", c.air_support_min_modifier);
    c.air_support_max_modifier = num("air_support_max_modifier", c.air_support_max_modifier);
    c.air_mission_weight_contested =
        num("air_mission_weight_contested", c.air_mission_weight_contested);
    c.air_mission_weight_support =
        num("air_mission_weight_support", c.air_mission_weight_support);

    c.naval_base_capacity_per_level = num("naval_base_capacity_per_level", c.naval_base_capacity_per_level);
    c.naval_detection_scale = num("naval_detection_scale", c.naval_detection_scale);
    c.naval_combat_roll_base = num("naval_combat_roll_base", c.naval_combat_roll_base);
    c.naval_combat_scale = num("naval_combat_scale", c.naval_combat_scale);
    c.naval_org_damage_scale = num("naval_org_damage_scale", c.naval_org_damage_scale);
    c.naval_torpedo_large_hull_bonus = num("naval_torpedo_large_hull_bonus", c.naval_torpedo_large_hull_bonus);
    c.naval_sub_detection_penalty = num("naval_sub_detection_penalty", c.naval_sub_detection_penalty);
    c.naval_aa_carrier_air_factor = num("naval_aa_carrier_air_factor", c.naval_aa_carrier_air_factor);
    c.naval_air_attacks_per_hour = num("naval_air_attacks_per_hour", c.naval_air_attacks_per_hour);
    c.naval_screen_share_cap = num("naval_screen_share_cap", c.naval_screen_share_cap);
    c.naval_retreat_strength_threshold = num("naval_retreat_strength_threshold", c.naval_retreat_strength_threshold);
    c.naval_retreat_org_threshold = num("naval_retreat_org_threshold", c.naval_retreat_org_threshold);
    c.naval_repair_per_hour = num("naval_repair_per_hour", c.naval_repair_per_hour);
    c.naval_repair_org_per_hour = num("naval_repair_org_per_hour", c.naval_repair_org_per_hour);
    c.naval_repair_cost_fuel = num("naval_repair_cost_fuel", c.naval_repair_cost_fuel);
    c.naval_repair_cost_stockpile_share = num("naval_repair_cost_stockpile_share", c.naval_repair_cost_stockpile_share);
    c.naval_fuel_use_per_hour = num("naval_fuel_use_per_hour", c.naval_fuel_use_per_hour);
    c.naval_training_experience_per_hour = num("naval_training_experience_per_hour", c.naval_training_experience_per_hour);
    c.naval_combat_experience_per_hour = num("naval_combat_experience_per_hour", c.naval_combat_experience_per_hour);
    c.naval_raid_convoy_damage = num("naval_raid_convoy_damage", c.naval_raid_convoy_damage);
    c.naval_raid_control_cut = num("naval_raid_control_cut", c.naval_raid_control_cut);
    c.naval_escort_protection = num("naval_escort_protection", c.naval_escort_protection);
    c.naval_max_engagement_ships = num("naval_max_engagement_ships", c.naval_max_engagement_ships);
    c.naval_large_hull_hp = num("naval_large_hull_hp", c.naval_large_hull_hp);
    c.naval_base_supply_per_level = num("naval_base_supply_per_level", c.naval_base_supply_per_level);
    c.naval_supply_sea_range_penalty = num("naval_supply_sea_range_penalty", c.naval_supply_sea_range_penalty);
    c.naval_supply_convoy_use_per_capacity = num("naval_supply_convoy_use_per_capacity", c.naval_supply_convoy_use_per_capacity);
    c.naval_supply_raid_threshold = num("naval_supply_raid_threshold", c.naval_supply_raid_threshold);
    c.naval_invasion_convoys_per_division = num("naval_invasion_convoys_per_division", c.naval_invasion_convoys_per_division);
    c.naval_invasion_hours_per_sea_hop = num("naval_invasion_hours_per_sea_hop", c.naval_invasion_hours_per_sea_hop);
    c.naval_invasion_interception_base = num("naval_invasion_interception_base", c.naval_invasion_interception_base);
    c.naval_invasion_interception_threat_scale = num("naval_invasion_interception_threat_scale", c.naval_invasion_interception_threat_scale);
    c.naval_invasion_escort_mitigation = num("naval_invasion_escort_mitigation", c.naval_invasion_escort_mitigation);

    c.construction_cost_factory = num("construction_cost_factory", c.construction_cost_factory);
    c.construction_cost_infrastructure =
        num("construction_cost_infrastructure", c.construction_cost_infrastructure);
    c.construction_cost_railway = num("construction_cost_railway", c.construction_cost_railway);
    c.construction_cost_supply_hub =
        num("construction_cost_supply_hub", c.construction_cost_supply_hub);
    c.construction_cost_air_base = num("construction_cost_air_base", c.construction_cost_air_base);
    c.construction_cost_naval_base =
        num("construction_cost_naval_base", c.construction_cost_naval_base);
    c.construction_cost_fort = num("construction_cost_fort", c.construction_cost_fort);
    c.construction_cost_radar = num("construction_cost_radar", c.construction_cost_radar);
    c.construction_cost_synthetic =
        num("construction_cost_synthetic", c.construction_cost_synthetic);
    c.construction_level_scaling =
        num("construction_level_scaling", c.construction_level_scaling);
    c.max_factories_per_project =
        num("max_factories_per_project", c.max_factories_per_project);

    c.research_base_days = num("research_base_days", c.research_base_days);
    c.research_year_penalty = num("research_year_penalty", c.research_year_penalty);
    c.research_speed_base = num("research_speed_base", c.research_speed_base);

    c.manpower_growth_per_year_fraction =
        num("manpower_growth_per_year_fraction", c.manpower_growth_per_year_fraction);
    c.recruitable_base = num("recruitable_base", c.recruitable_base);

    c.base_hours_per_province = num("base_hours_per_province", c.base_hours_per_province);
    c.min_division_speed = num("min_division_speed", c.min_division_speed);
    c.river_crossing_penalty = num("river_crossing_penalty", c.river_crossing_penalty);

    c.combat_width_base = num("combat_width_base", c.combat_width_base);
    c.damage_scale = num("damage_scale", c.damage_scale);
    c.org_damage_share = num("org_damage_share", c.org_damage_share);
    c.strength_damage_share = num("strength_damage_share", c.strength_damage_share);
    c.armor_advantage_multiplier =
        num("armor_advantage_multiplier", c.armor_advantage_multiplier);
    c.armor_disadvantage_multiplier =
        num("armor_disadvantage_multiplier", c.armor_disadvantage_multiplier);
    c.org_recovery_base = num("org_recovery_base", c.org_recovery_base);
    c.entrenchment_per_day = num("entrenchment_per_day", c.entrenchment_per_day);
    c.planning_per_day = num("planning_per_day", c.planning_per_day);
    c.planning_max_attack_bonus = num("planning_max_attack_bonus", c.planning_max_attack_bonus);
    c.battle_retreat_org_threshold =
        num("battle_retreat_org_threshold", c.battle_retreat_org_threshold);
    c.max_battles_per_province = num("max_battles_per_province", c.max_battles_per_province);

    c.supply_hub_radius = num("supply_hub_radius", c.supply_hub_radius);
    c.supply_range_penalty = num("supply_range_penalty", c.supply_range_penalty);
    c.supply_demand_per_width = num("supply_demand_per_width", c.supply_demand_per_width);
    c.supply_rail_bonus_per_level = num("supply_rail_bonus_per_level", c.supply_rail_bonus_per_level);
    c.supply_infrastructure_bonus_per_level =
        num("supply_infrastructure_bonus_per_level", c.supply_infrastructure_bonus_per_level);
    c.fuel_demand_per_day = num("fuel_demand_per_day", c.fuel_demand_per_day);

    c.political_power_per_day = num("political_power_per_day", c.political_power_per_day);
    c.stability_drift = num("stability_drift", c.stability_drift);
    c.war_support_drift = num("war_support_drift", c.war_support_drift);

    c.weather_change_chance = num("weather_change_chance", c.weather_change_chance);

    // Air.
    c.air_base_capacity_per_level =
        num("air_base_capacity_per_level", c.air_base_capacity_per_level);
    c.air_sortie_hours = num("air_sortie_hours", c.air_sortie_hours);
    c.air_cas_effect = num("air_cas_effect", c.air_cas_effect);
    c.air_superiority_effect = num("air_superiority_effect", c.air_superiority_effect);
    c.air_bombing_industry_damage =
        num("air_bombing_industry_damage", c.air_bombing_industry_damage);
    c.air_logistics_strike_damage =
        num("air_logistics_strike_damage", c.air_logistics_strike_damage);
    return c;
}

bool load_content(const std::string& data_root, Content* out, std::string* err) {
    if (out == nullptr) {
        if (err) *err = "load_content: null output";
        return false;
    }
    out->load_errors.clear();
    std::vector<std::string>& errors = out->load_errors;
    std::string root = data_root;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();

    const std::string f_constants = root + "/common/constants.json";
    const std::string f_equipment = root + "/common/equipment.json";
    const std::string f_buildings = root + "/common/buildings.json";
    const std::string f_technologies = root + "/common/technologies.json";
    const std::string f_laws = root + "/common/laws.json";
    const std::string f_templates = root + "/common/templates.json";

    Json doc;
    std::string file_err;

    // ---- constants -------------------------------------------------------
    if (!load_json_file(f_constants, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    out->constants = SimConstants::from_json(doc);

    // ---- equipment -------------------------------------------------------
    if (!load_json_file(f_equipment, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    {
        const Json& arr = doc["equipment"];
        if (!arr.is_array()) {
            const std::string msg = f_equipment + ":equipment: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& e = arr[i];
            const std::string key = e["key"].as_string();
            if (key.empty()) {
                const std::string msg = f_equipment + ":<entry " + std::to_string(i) +
                                        ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->equipment_by_key.count(key) != 0) {
                const std::string msg = f_equipment + ":" + key + ": duplicate equipment key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            EquipmentDef def;
            def.id = EquipmentId(static_cast<uint32_t>(out->equipment.size()));
            def.key = key;
            def.name = e["name"].as_string(key);
            const std::string category_name = e["category"].as_string();
            const int cat = match_equipment_category(category_name);
            if (cat < 0 && !category_name.empty()) {
                errors.push_back(f_equipment + ":" + key + ": unknown category " + category_name);
            }
            def.category = cat >= 0 ? static_cast<EquipmentCategory>(cat) : EquipmentCategory::Infantry;
            def.year = static_cast<int>(e["year"].as_int(def.year));
            def.archetype = e["archetype"].as_string();
            def.is_archetype = e.has("is_archetype") ? e["is_archetype"].as_bool(false)
                                                     : def.archetype.empty();
            def.soft_attack = e["soft_attack"].as_double(def.soft_attack);
            def.hard_attack = e["hard_attack"].as_double(def.hard_attack);
            def.air_attack = e["air_attack"].as_double(def.air_attack);
            // Air statistics. A negative value is a data error: it is reported and
            // clamped to zero so invalid state can never reach the simulation.
            auto non_negative = [&](const char* field, double* dst) {
                const double raw = e.has(field) ? e[field].as_double(*dst) : *dst;
                if (raw < 0.0) {
                    errors.push_back(f_equipment + ":" + key + ": negative " + field +
                                     " (" + std::to_string(raw) + ")");
                    *dst = 0.0;
                } else {
                    *dst = raw;
                }
            };
            non_negative("air_defence", &def.air_defence);
            non_negative("ground_attack", &def.ground_attack);
            non_negative("agility", &def.agility);
            non_negative("range", &def.range);
            // Naval statistics. Read through the same clamp-and-report path: a
            // negative value is a data error and is reported with the rest.
            non_negative("naval_attack", &def.naval_attack);
            non_negative("torpedo_attack", &def.torpedo_attack);
            non_negative("sub_detection", &def.sub_detection);
            non_negative("detection", &def.detection);
            non_negative("visibility", &def.visibility);
            def.defense = e["defense"].as_double(def.defense);
            def.breakthrough = e["breakthrough"].as_double(def.breakthrough);
            def.armor = e["armor"].as_double(def.armor);
            def.piercing = e["piercing"].as_double(def.piercing);
            def.hardness = e["hardness"].as_double(def.hardness);
            def.reliability = e["reliability"].as_double(def.reliability);
            def.speed = e["speed"].as_double(def.speed);
            def.max_strength = e["max_strength"].as_double(def.max_strength);
            def.organization = e["organization"].as_double(def.organization);
            def.build_cost = e["build_cost"].as_double(def.build_cost);
            def.fuel_use = e["fuel_use"].as_double(def.fuel_use);
            def.supply_use = e["supply_use"].as_double(def.supply_use);
            def.manpower = e["manpower"].as_double(def.manpower);
            parse_resource_costs(e["resources"], def.resources, &errors, f_equipment, key);
            out->equipment_by_key[key] = def.id;
            out->equipment.push_back(std::move(def));
        }
        // Model archetypes must exist once every entry has an id.
        for (EquipmentDef& def : out->equipment) {
            if (def.archetype.empty()) continue;
            if (out->equipment_by_key.count(def.archetype) == 0) {
                errors.push_back(f_equipment + ":" + def.key + ": unknown archetype " +
                                 def.archetype);
            }
        }
    }

    // ---- buildings (before techs: technologies may unlock building keys) ---
    if (!load_json_file(f_buildings, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    {
        const Json& arr = doc["buildings"];
        if (!arr.is_array()) {
            const std::string msg = f_buildings + ":buildings: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& b = arr[i];
            const std::string key = b["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_buildings + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            BuildingDef def;
            def.key = key;
            def.name = b["name"].as_string(key);
            const std::string kind_name = b["kind"].as_string();
            const int kind = match_building_kind(kind_name);
            if (kind < 0) {
                errors.push_back(f_buildings + ":" + key + ": unknown building kind " + kind_name);
            } else {
                def.kind = static_cast<BuildingKind>(kind);
            }
            def.base_cost = b["base_cost"].as_double(def.base_cost);
            def.per_state = b["per_state"].as_bool(def.per_state);
            def.max_level = static_cast<int>(b["max_level"].as_int(def.max_level));
            out->buildings.push_back(std::move(def));
        }
    }

    // ---- technologies ----------------------------------------------------
    if (!load_json_file(f_technologies, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    {
        const Json& arr = doc["technologies"];
        if (!arr.is_array()) {
            const std::string msg = f_technologies + ":technologies: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& t = arr[i];
            const std::string key = t["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_technologies + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->tech_by_key.count(key) != 0) {
                const std::string msg = f_technologies + ":" + key + ": duplicate technology key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            TechDef def;
            def.id = TechId(static_cast<uint32_t>(out->techs.size()));
            def.key = key;
            def.name = t["name"].as_string(key);
            def.category = t["category"].as_string();
            def.year = static_cast<int>(t["year"].as_int(def.year));
            def.cost_days = t["cost_days"].as_double(def.cost_days);
            def.modifiers = parse_modifiers(t["modifiers"], &errors, f_technologies, key);
            const Json& unlocks_eq = t["unlock_equipment"];
            if (unlocks_eq.is_array()) {
                for (size_t k = 0; k < unlocks_eq.size(); ++k) {
                    const std::string ek = unlocks_eq[k].as_string();
                    if (ek.empty()) continue;
                    if (out->equipment_by_key.count(ek) == 0) {
                        errors.push_back(f_technologies + ":" + key +
                                         ": unlock_equipment references unknown equipment " + ek);
                    }
                    def.unlock_equipment.push_back(ek);
                }
            }
            const Json& unlocks_b = t["unlock_buildings"];
            if (unlocks_b.is_array()) {
                for (size_t k = 0; k < unlocks_b.size(); ++k) {
                    const std::string bk = unlocks_b[k].as_string();
                    if (bk.empty()) continue;
                    bool known = false;
                    for (const BuildingDef& bd : out->buildings) {
                        if (bd.key == bk) { known = true; break; }
                    }
                    if (!known) {
                        errors.push_back(f_technologies + ":" + key +
                                         ": unlock_buildings references unknown building " + bk);
                    }
                    def.unlock_buildings.push_back(bk);
                }
            }
            out->tech_by_key[key] = def.id;
            out->techs.push_back(std::move(def));
        }
        // Prerequisites resolve after every tech id exists.
        for (TechDef& def : out->techs) {
            const Json& arr_t = doc["technologies"];
            for (size_t i = 0; i < arr_t.size(); ++i) {
                if (arr_t[i]["key"].as_string() != def.key) continue;
                const Json& prereq = arr_t[i]["prerequisites"];
                if (!prereq.is_array()) break;
                for (size_t k = 0; k < prereq.size(); ++k) {
                    const std::string pk = prereq[k].as_string();
                    if (pk.empty()) continue;
                    auto it = out->tech_by_key.find(pk);
                    if (it == out->tech_by_key.end()) {
                        errors.push_back(f_technologies + ":" + def.key +
                                         ": unknown prerequisite " + pk);
                        continue;
                    }
                    def.prerequisites.push_back(it->second);
                }
                break;
            }
        }
    }

    // ---- laws ------------------------------------------------------------
    if (!load_json_file(f_laws, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    {
        const Json& arr = doc["laws"];
        if (!arr.is_array()) {
            const std::string msg = f_laws + ":laws: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& l = arr[i];
            const std::string key = l["key"].as_string();
            if (key.empty()) {
                const std::string msg = f_laws + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->law_index.count(key) != 0) {
                const std::string msg = f_laws + ":" + key + ": duplicate law key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            LawDef def;
            def.key = key;
            def.name = l["name"].as_string(key);
            def.kind = static_cast<int>(l["kind"].as_int(def.kind));
            def.level = static_cast<int>(l["level"].as_int(def.level));
            def.cost = l["cost"].as_double(def.cost);
            def.modifiers = parse_modifiers(l["modifiers"], &errors, f_laws, key);
            def.requires_law = l["requires_law"].as_string();
            def.requires_level = static_cast<int>(l["requires_level"].as_int(def.requires_level));
            out->law_index[key] = static_cast<int>(out->laws.size());
            out->laws.push_back(std::move(def));
        }
        // Law prerequisite keys must resolve to laws in the same file.
        for (const LawDef& def : out->laws) {
            if (!def.requires_law.empty() && out->law_index.count(def.requires_law) == 0) {
                errors.push_back(f_laws + ":" + def.key + ": unknown requires_law " +
                                 def.requires_law);
            }
        }
    }

    // ---- division templates ----------------------------------------------
    if (!load_json_file(f_templates, &doc, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    {
        const Json& arr = doc["templates"];
        if (!arr.is_array()) {
            const std::string msg = f_templates + ":templates: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& t = arr[i];
            const std::string key = t["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_templates + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->template_by_key.count(key) != 0) {
                const std::string msg = f_templates + ":" + key + ": duplicate template key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            DivisionTemplate def;
            def.id = TemplateId(static_cast<uint32_t>(out->templates.size()));
            def.key = key;
            def.name = t["name"].as_string(key);
            def.train_days = t["train_days"].as_double(def.train_days);
            const Json& battalions = t["battalions"];
            if (battalions.is_array()) {
                for (size_t b = 0; b < battalions.size(); ++b) {
                    const Json& entry = battalions[b];
                    const std::string eq_key = entry["equipment"].as_string();
                    const EquipmentId eq = out->equipment_id(eq_key);
                    if (!eq.valid() && !eq_key.empty()) {
                        errors.push_back(f_templates + ":" + key +
                                         ": unknown equipment " + eq_key);
                        continue;
                    }
                    BattalionSlot slot;
                    slot.equipment = eq;
                    slot.count = static_cast<int>(entry["count"].as_int(0));
                    slot.support = entry["support"].as_bool(false);
                    if (slot.count <= 0) {
                        errors.push_back(f_templates + ":" + key +
                                         ": battalion with non-positive count");
                        continue;
                    }
                    def.battalions.push_back(slot);
                }
            }
            // Derived stats come from the shared template maths (sim/units.cpp).
            recompute_template_stats(def, out->equipment);
            out->template_by_key[key] = def.id;
            out->templates.push_back(std::move(def));
        }
    }

    if (err) *err = errors.empty() ? std::string() : errors.front();
    return true;
}

}  // namespace hoi
