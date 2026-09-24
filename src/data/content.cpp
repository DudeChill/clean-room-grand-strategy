// Content database loading (spec section 168).
//
// All files are optional in shape (unknown keys are ignored) but required in
// existence: load_content returns false only for a missing/unparsable file, a
// duplicate key, or an entry without a key. Everything else is recorded in
// Content::load_errors as "<file>:<key>: <reason>" so a modder can fix it without
// a debugger.

#include "data/content.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <set>
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

// ------------------------------------------------ script block validation ----
//
// Focuses, events and decisions carry trigger/effect blocks in the vocabulary of
// src/sim/script.h. The loader resolves every reference they make (focus, event,
// decision, technology, law, state keys) so a modder gets
// "<file>:<key>: <reason>" in Content::load_errors instead of silently dead
// content. Evaluation itself stays in the script engine.

bool is_comparator(const std::string& key) {
    return key == "gte" || key == "gt" || key == "lte" || key == "lt" || key == "eq";
}

int comparator_count(const Json& j) {
    int n = 0;
    for (const auto& item : j.object_items()) {
        if (is_comparator(item.first)) ++n;
    }
    return n;
}

std::string where(const std::string& path) {
    return path.empty() ? std::string() : path + ": ";
}

// `value` is either a bare number (implicit equality) or an object with exactly one
// of gte/gt/lte/lt/eq.
void validate_comparator(const Json& value, std::vector<std::string>* errors,
                         const std::string& file, const std::string& key,
                         const std::string& path) {
    if (value.is_number()) return;
    if (!value.is_object()) {
        errors->push_back(file + ":" + key + ": " + where(path) +
                          "expects a number or a {gte|gt|lte|lt|eq: number} object");
        return;
    }
    if (comparator_count(value) != 1) {
        errors->push_back(file + ":" + key + ": " + where(path) +
                          "needs exactly one of gte/gt/lte/lt/eq");
    }
    for (const auto& item : value.object_items()) {
        if (is_comparator(item.first)) {
            if (!item.second.is_number()) {
                errors->push_back(file + ":" + key + ": " + where(path) + item.first +
                                  " expects a number");
            }
        } else {
            errors->push_back(file + ":" + key + ": " + where(path) + "unexpected key '" +
                              item.first + "'");
        }
    }
}

void validate_trigger(const Json& t, const Content& c,
                      const std::set<std::string>& states,
                      std::vector<std::string>* errors, const std::string& file,
                      const std::string& key, const std::string& path);

void validate_effects(const Json& e, const Content& c,
                      const std::set<std::string>& states,
                      std::vector<std::string>* errors, const std::string& file,
                      const std::string& key, const std::string& path);

void validate_trigger(const Json& t, const Content& c,
                      const std::set<std::string>& states,
                      std::vector<std::string>* errors, const std::string& file,
                      const std::string& key, const std::string& path) {
    if (t.is_null()) return;
    if (!t.is_object()) {
        errors->push_back(file + ":" + key + ": " + where(path) + "trigger must be an object");
        return;
    }
    for (const auto& item : t.object_items()) {
        const std::string& k = item.first;
        const Json& v = item.second;
        const std::string p = path.empty() ? k : path + "." + k;
        if (k == "all" || k == "any") {
            if (!v.is_array()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k + " expects an array");
                continue;
            }
            for (size_t i = 0; i < v.size(); ++i) {
                validate_trigger(v[i], c, states, errors, file, key,
                                 k + "[" + std::to_string(i) + "]");
            }
        } else if (k == "not") {
            if (v.is_array()) {
                for (size_t i = 0; i < v.size(); ++i) {
                    validate_trigger(v[i], c, states, errors, file, key,
                                     "not[" + std::to_string(i) + "]");
                }
            } else {
                validate_trigger(v, c, states, errors, file, key, p);
            }
        } else if (k == "chance") {
            if (!v.is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) + "chance expects a number");
            }
        } else if (k == "is_ai" || k == "at_war" || k == "state_controller_is_owner") {
            if (!v.is_bool()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  " expects true or false");
            }
        } else if (k == "political_power" || k == "stability" || k == "war_support" ||
                   k == "manpower" || k == "fuel" || k == "factories" ||
                   k == "num_divisions" || k == "year" || k == "state_factories") {
            validate_comparator(v, errors, file, key, p);
        } else if (k == "opinion") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "opinion expects an object");
                continue;
            }
            if (comparator_count(v) != 1) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "opinion needs exactly one comparator");
            }
            for (const auto& f : v.object_items()) {
                if (f.first == "target") {
                    if (!f.second.is_string() || f.second.as_string().empty()) {
                        errors->push_back(file + ":" + key + ": " + where(p) +
                                          "opinion.target expects a country tag");
                    }
                } else if (is_comparator(f.first)) {
                    if (!f.second.is_number()) {
                        errors->push_back(file + ":" + key + ": " + where(p) + "opinion." +
                                          f.first + " expects a number");
                    }
                } else {
                    errors->push_back(file + ":" + key + ": " + where(p) +
                                      "opinion has unexpected key '" + f.first + "'");
                }
            }
        } else if (k == "var") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) + "var expects an object");
                continue;
            }
            const std::string name = v["name"].as_string();
            if (name.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) + "var needs a name");
            }
            if (comparator_count(v) != 1) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "var needs exactly one comparator");
            }
            for (const auto& f : v.object_items()) {
                if (f.first == "name") {
                    if (!f.second.is_string()) {
                        errors->push_back(file + ":" + key + ": " + where(p) +
                                          "var.name expects a string");
                    }
                } else if (is_comparator(f.first)) {
                    if (!f.second.is_number()) {
                        errors->push_back(file + ":" + key + ": " + where(p) + "var." + f.first +
                                          " expects a number");
                    }
                } else {
                    errors->push_back(file + ":" + key + ": " + where(p) +
                                      "var has unexpected key '" + f.first + "'");
                }
            }
        } else if (k == "ideology" || k == "at_war_with" || k == "has_flag" ||
                   k == "has_country_flag" || k == "state_has_flag" ||
                   k == "date_after" || k == "date_before") {
            if (!v.is_string() || v.as_string().empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  " expects a non-empty string");
            }
        } else if (k == "has_tech") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "has_tech expects a technology key");
            } else if (c.tech_by_key.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "has_tech references unknown technology " + ref);
            }
        } else if (k == "completed_focus") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "completed_focus expects a focus key");
            } else if (c.focus_index.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "completed_focus references unknown focus " + ref);
            }
        } else if (k == "has_decision") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "has_decision expects a decision key");
            } else if (c.decision_index.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "has_decision references unknown decision " + ref);
            }
        } else if (k == "owns_state" || k == "controls_state") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  " expects a state key");
            } else if (!states.empty() && states.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  " references unknown state " + ref);
            }
        } else {
            errors->push_back(file + ":" + key + ": " + where(p) + "unknown trigger key '" + k +
                              "'");
        }
    }
}

void validate_effects(const Json& e, const Content& c,
                      const std::set<std::string>& states,
                      std::vector<std::string>* errors, const std::string& file,
                      const std::string& key, const std::string& path) {
    if (e.is_null()) return;
    if (!e.is_object()) {
        errors->push_back(file + ":" + key + ": " + where(path) + "effect block must be an object");
        return;
    }
    for (const auto& item : e.object_items()) {
        const std::string& k = item.first;
        const Json& v = item.second;
        const std::string p = path.empty() ? k : path + "." + k;
        if (k == "add_political_power" || k == "add_stability" || k == "add_war_support" ||
            k == "add_manpower" || k == "add_fuel") {
            if (!v.is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k + " expects a number");
            }
        } else if (k == "add_opinion") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_opinion expects an object");
                continue;
            }
            const std::string target = v["target"].as_string();
            if (target.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_opinion.target expects a country tag");
            }
            if (!v["value"].is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_opinion.value expects a number");
            }
        } else if (k == "declare_war") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "declare_war expects an object");
                continue;
            }
            if (v["target"].as_string().empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "declare_war.target expects a country tag");
            }
            for (const char* flag : {"annex", "puppet"}) {
                if (v.has(flag) && !v[flag].is_bool()) {
                    errors->push_back(file + ":" + key + ": " + where(p) + "declare_war." + flag +
                                      " expects true or false");
                }
            }
        } else if (k == "add_tech") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_tech expects a technology key");
            } else if (c.tech_by_key.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_tech references unknown technology " + ref);
            }
        } else if (k == "set_law") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "set_law expects a law key");
            } else if (c.law_index.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "set_law references unknown law " + ref);
            }
        } else if (k == "add_modifier") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier expects an object");
                continue;
            }
            const std::string kind = v["kind"].as_string();
            if (kind.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier.kind expects a modifier name");
            } else if (match_modifier(kind) < 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier references unknown modifier " + kind);
            }
            if (!v["value"].is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier.value expects a number");
            }
            if (v.has("days") && !v["days"].is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier.days expects a number");
            }
            if (v.has("source") && !v["source"].is_string()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_modifier.source expects a string");
            }
        } else if (k == "complete_focus") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "complete_focus expects a focus key");
            } else if (c.focus_index.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "complete_focus references unknown focus " + ref);
            }
        } else if (k == "trigger_event") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "trigger_event expects an object");
                continue;
            }
            const std::string ref = v["key"].as_string();
            if (ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "trigger_event.key expects an event key");
            } else if (c.event_index.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "trigger_event references unknown event " + ref);
            }
            if (v.has("days") && !v["days"].is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "trigger_event.days expects a number");
            }
        } else if (k == "set_flag" || k == "clear_flag") {
            if (!v.is_string() || v.as_string().empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  " expects a non-empty string");
            }
        } else if (k == "set_variable" || k == "add_to_variable") {
            if (!v.is_object()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k + " expects an object");
                continue;
            }
            if (v["name"].as_string().empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  ".name expects a variable name");
            }
            if (!v["value"].is_number()) {
                errors->push_back(file + ":" + key + ": " + where(p) + k +
                                  ".value expects a number");
            }
        } else if (k == "add_claim") {
            const std::string ref = v.as_string();
            if (!v.is_string() || ref.empty()) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_claim expects a state key");
            } else if (!states.empty() && states.count(ref) == 0) {
                errors->push_back(file + ":" + key + ": " + where(p) +
                                  "add_claim references unknown state " + ref);
            }
        } else {
            errors->push_back(file + ":" + key + ": " + where(p) + "unknown effect key '" + k +
                              "'");
        }
    }
}

// State keys ("s12") are declared by the map files, which the loader reads only to
// build the valid-key set; the scenario assigns ids in file order.
std::set<std::string> collect_state_keys(const std::string& root) {
    namespace fs = std::filesystem;
    std::set<std::string> keys;
    const std::string dir = root + "/maps";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return keys;
    std::vector<std::string> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string p = entry.path().string();
        if (p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0) files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    for (const std::string& f : files) {
        Json map;
        std::string parse_err;
        if (!Json::parse_file(f, &map, &parse_err)) continue;  // map errors are scenario's job
        const Json& arr = map["states"];
        if (!arr.is_array()) continue;
        for (size_t i = 0; i < arr.size(); ++i) {
            const std::string k = arr[i]["key"].as_string();
            if (!k.empty()) keys.insert(k);
        }
    }
    return keys;
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
    c.focus_progress_speed = num("focus_progress_speed", c.focus_progress_speed);
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

    // ---- national focuses ------------------------------------------------
    // Every data/common/focuses/*.json in sorted file order, so focus indices (and
    // therefore save files) are stable. Each file is {"tree": "...", "focuses": []}.
    std::vector<std::string> focus_src;
    {
        namespace fs = std::filesystem;
        const std::string dir = root + "/common/focuses";
        std::error_code ec;
        // Optional content: a minimal data set may ship no focus trees at all.
        // Only a file that exists and does not parse is a hard error.
        std::vector<std::string> files;
        if (fs::is_directory(dir, ec)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const std::string p = entry.path().string();
                if (p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0) files.push_back(p);
            }
        }
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) {
            Json fd;
            if (!load_json_file(f, &fd, &file_err)) {
                errors.push_back(file_err);
                if (err) *err = file_err;
                return false;
            }
            const std::string default_tree = fd["tree"].as_string("shared");
            const Json& arr = fd["focuses"];
            if (!arr.is_array()) {
                errors.push_back(f + ":focuses: expected an array");
                continue;
            }
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& fj = arr[i];
                const std::string key = fj["key"].as_string();
                if (key.empty()) {
                    errors.push_back(f + ":<entry " + std::to_string(i) + ">: missing key");
                    continue;
                }
                if (out->focus_index.count(key) != 0) {
                    errors.push_back(f + ":" + key + ": duplicate focus key");
                    continue;
                }
                FocusDef def;
                def.index = static_cast<uint32_t>(out->focuses.size());
                def.key = key;
                def.name = fj["name"].as_string(key);
                def.tree = fj["tree"].as_string(default_tree);
                def.x = static_cast<int>(fj["x"].as_int(def.x));
                def.y = static_cast<int>(fj["y"].as_int(def.y));
                def.days = fj["days"].as_double(def.days);
                if (!std::isfinite(def.days) || def.days <= 0.0) {
                    errors.push_back(f + ":" + key + ": days must be a positive number");
                    def.days = 70.0;
                }
                const Json& prereq = fj["prerequisites"];
                if (prereq.is_array()) {
                    for (size_t k = 0; k < prereq.size(); ++k) {
                        const std::string pk = prereq[k].as_string();
                        if (!pk.empty()) def.prerequisites.push_back(pk);
                    }
                }
                const Json& excl = fj["mutually_exclusive"];
                if (excl.is_array()) {
                    for (size_t k = 0; k < excl.size(); ++k) {
                        const std::string ek = excl[k].as_string();
                        if (!ek.empty()) def.mutually_exclusive.push_back(ek);
                    }
                }
                def.available = fj["available"];
                def.bypass = fj["bypass"];
                def.effects = fj["effects"];
                def.ai_weight = fj["ai_weight"].as_double(def.ai_weight);
                out->focus_index[key] = def.index;
                out->focuses.push_back(std::move(def));
                focus_src.push_back(f);
            }
        }
    }

    // ---- events ----------------------------------------------------------
    const std::string f_events = root + "/common/events.json";
    std::vector<std::string> event_src;
    if (std::filesystem::exists(f_events)) {
        Json fd;
        if (!load_json_file(f_events, &fd, &file_err)) {
            errors.push_back(file_err);
            if (err) *err = file_err;
            return false;
        }
        const Json& arr = fd["events"];
        if (!arr.is_array()) {
            const std::string msg = f_events + ":events: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& ej = arr[i];
            const std::string key = ej["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_events + ":<entry " + std::to_string(i) + ">: missing key");
                continue;
            }
            if (out->event_index.count(key) != 0) {
                errors.push_back(f_events + ":" + key + ": duplicate event key");
                continue;
            }
            EventDef def;
            def.index = static_cast<uint32_t>(out->events.size());
            def.key = key;
            def.title = ej["title"].as_string(key);
            def.description = ej["description"].as_string();
            def.fire_only_once = ej["fire_only_once"].as_bool(def.fire_only_once);
            def.major = ej["major"].as_bool(def.major);
            def.trigger = ej["trigger"];
            def.immediate = ej["immediate"];
            const Json& options = ej["options"];
            if (options.is_array()) {
                for (size_t k = 0; k < options.size(); ++k) {
                    const Json& oj = options[k];
                    EventOptionDef opt;
                    opt.name = oj["name"].as_string("Option " + std::to_string(k + 1));
                    opt.effects = oj["effects"];
                    opt.ai_weight = oj["ai_weight"].as_double(opt.ai_weight);
                    def.options.push_back(std::move(opt));
                }
            }
            if (def.options.empty()) {
                errors.push_back(f_events + ":" + key + ": event has no options");
            }
            out->event_index[key] = def.index;
            out->events.push_back(std::move(def));
            event_src.push_back(f_events);
        }
    }

    // ---- decisions -------------------------------------------------------
    const std::string f_decisions = root + "/common/decisions.json";
    std::vector<std::string> decision_src;
    if (std::filesystem::exists(f_decisions)) {
        Json fd;
        if (!load_json_file(f_decisions, &fd, &file_err)) {
            errors.push_back(file_err);
            if (err) *err = file_err;
            return false;
        }
        const Json& arr = fd["decisions"];
        if (!arr.is_array()) {
            const std::string msg = f_decisions + ":decisions: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& dj = arr[i];
            const std::string key = dj["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_decisions + ":<entry " + std::to_string(i) + ">: missing key");
                continue;
            }
            if (out->decision_index.count(key) != 0) {
                errors.push_back(f_decisions + ":" + key + ": duplicate decision key");
                continue;
            }
            DecisionDef def;
            def.index = static_cast<uint32_t>(out->decisions.size());
            def.key = key;
            def.name = dj["name"].as_string(key);
            def.description = dj["description"].as_string();
            def.category = static_cast<int>(dj["category"].as_int(def.category));
            def.targets_state = dj["targets_state"].as_bool(def.targets_state);
            def.cost_pp = dj["cost_pp"].as_double(def.cost_pp);
            def.days_remove = static_cast<int>(dj["days_remove"].as_int(def.days_remove));
            def.days_cooldown = static_cast<int>(dj["days_cooldown"].as_int(def.days_cooldown));
            def.visible = dj["visible"];
            def.available = dj["available"];
            def.effects = dj["effects"];
            def.remove_effect = dj["remove_effect"];
            def.ai_weight = dj["ai_weight"].as_double(def.ai_weight);
            if (def.cost_pp < 0.0) {
                errors.push_back(f_decisions + ":" + key + ": cost_pp must not be negative");
                def.cost_pp = 0.0;
            }
            if (def.days_remove < 0 || def.days_cooldown < 0) {
                errors.push_back(f_decisions + ":" + key + ": negative days_remove/days_cooldown");
                def.days_remove = def.days_remove < 0 ? 0 : def.days_remove;
                def.days_cooldown = def.days_cooldown < 0 ? 0 : def.days_cooldown;
            }
            out->decision_index[key] = def.index;
            out->decisions.push_back(std::move(def));
            decision_src.push_back(f_decisions);
        }
    }

    // ---- national spirits ------------------------------------------------
    // Optional file: a data set with no spirits simply ships none. A file that
    // exists but does not parse is a hard error, exactly like the focus trees.
    // Modifiers use the same vocabulary as laws and technologies.
    const std::string f_spirits = root + "/common/spirits.json";
    std::vector<std::string> spirit_src;
    if (std::filesystem::exists(f_spirits)) {
        Json fd;
        if (!load_json_file(f_spirits, &fd, &file_err)) {
            errors.push_back(file_err);
            if (err) *err = file_err;
            return false;
        }
        const Json& arr = fd["spirits"];
        if (!arr.is_array()) {
            const std::string msg = f_spirits + ":spirits: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& sj = arr[i];
            const std::string key = sj["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_spirits + ":<entry " + std::to_string(i) + ">: missing key");
                continue;
            }
            if (out->spirit_index.count(key) != 0) {
                errors.push_back(f_spirits + ":" + key + ": duplicate spirit key");
                continue;
            }
            SpiritDef def;
            def.index = static_cast<uint32_t>(out->spirits.size());
            def.key = key;
            def.name = sj["name"].as_string(key);
            def.description = sj["description"].as_string();
            def.slots = static_cast<int>(sj["slots"].as_int(def.slots));
            if (def.slots < 1) {
                errors.push_back(f_spirits + ":" + key + ": slots must be at least 1");
                def.slots = 1;
            }
            def.available = sj["available"];
            def.modifiers = parse_modifiers(sj["modifiers"], &errors, f_spirits, key);
            def.effects = sj["effects"];
            out->spirit_index[key] = def.index;
            out->spirits.push_back(std::move(def));
            spirit_src.push_back(f_spirits);
        }
    }

    // ---- political advisors ----------------------------------------------
    const std::string f_advisors = root + "/common/advisors.json";
    std::vector<std::string> advisor_src;
    if (std::filesystem::exists(f_advisors)) {
        Json fd;
        if (!load_json_file(f_advisors, &fd, &file_err)) {
            errors.push_back(file_err);
            if (err) *err = file_err;
            return false;
        }
        const Json& arr = fd["advisors"];
        if (!arr.is_array()) {
            const std::string msg = f_advisors + ":advisors: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& aj = arr[i];
            const std::string key = aj["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_advisors + ":<entry " + std::to_string(i) + ">: missing key");
                continue;
            }
            if (out->advisor_index.count(key) != 0) {
                errors.push_back(f_advisors + ":" + key + ": duplicate advisor key");
                continue;
            }
            AdvisorDef def;
            def.index = static_cast<uint32_t>(out->advisors.size());
            def.key = key;
            def.name = aj["name"].as_string(key);
            def.description = aj["description"].as_string();
            def.cost_pp = aj["cost_pp"].as_double(def.cost_pp);
            if (def.cost_pp < 0.0) {
                errors.push_back(f_advisors + ":" + key + ": cost_pp must not be negative");
                def.cost_pp = 0.0;
            }
            def.available = aj["available"];
            def.modifiers = parse_modifiers(aj["modifiers"], &errors, f_advisors, key);
            out->advisor_index[key] = def.index;
            out->advisors.push_back(std::move(def));
            advisor_src.push_back(f_advisors);
        }
    }

    // ---- cross-reference validation --------------------------------------
    // Runs after every file is registered so a focus may reference an event or a
    // decision declared later, and vice versa.
    const std::set<std::string> state_keys = collect_state_keys(root);
    for (size_t i = 0; i < out->focuses.size(); ++i) {
        const FocusDef& d = out->focuses[i];
        const std::string file = i < focus_src.size() ? focus_src[i] : std::string();
        for (const std::string& p : d.prerequisites) {
            if (out->focus_index.count(p) == 0) {
                errors.push_back(file + ":" + d.key + ": unknown prerequisite " + p);
            }
        }
        for (const std::string& p : d.mutually_exclusive) {
            if (out->focus_index.count(p) == 0) {
                errors.push_back(file + ":" + d.key +
                                 ": mutually_exclusive references unknown focus " + p);
            }
        }
        validate_trigger(d.available, *out, state_keys, &errors, file, d.key, "available");
        validate_trigger(d.bypass, *out, state_keys, &errors, file, d.key, "bypass");
        validate_effects(d.effects, *out, state_keys, &errors, file, d.key, "effects");
    }
    for (size_t i = 0; i < out->events.size(); ++i) {
        const EventDef& d = out->events[i];
        const std::string file = i < event_src.size() ? event_src[i] : std::string();
        validate_trigger(d.trigger, *out, state_keys, &errors, file, d.key, "trigger");
        validate_effects(d.immediate, *out, state_keys, &errors, file, d.key, "immediate");
        for (size_t k = 0; k < d.options.size(); ++k) {
            validate_effects(d.options[k].effects, *out, state_keys, &errors, file, d.key,
                             "options[" + std::to_string(k) + "].effects");
        }
    }
    for (size_t i = 0; i < out->decisions.size(); ++i) {
        const DecisionDef& d = out->decisions[i];
        const std::string file = i < decision_src.size() ? decision_src[i] : std::string();
        validate_trigger(d.visible, *out, state_keys, &errors, file, d.key, "visible");
        validate_trigger(d.available, *out, state_keys, &errors, file, d.key, "available");
        validate_effects(d.effects, *out, state_keys, &errors, file, d.key, "effects");
        validate_effects(d.remove_effect, *out, state_keys, &errors, file, d.key, "remove_effect");
    }
    // Spirits and advisors are country-scoped: their triggers take no state scope,
    // so the state-key cross-reference set is deliberately empty here. Everything
    // else in the trigger/effect vocabulary (shape, known modifiers, referenced
    // technologies and focuses) is still checked.
    const std::set<std::string> no_states;
    for (size_t i = 0; i < out->spirits.size(); ++i) {
        const SpiritDef& d = out->spirits[i];
        const std::string file = i < spirit_src.size() ? spirit_src[i] : std::string();
        validate_trigger(d.available, *out, no_states, &errors, file, d.key, "available");
        validate_effects(d.effects, *out, no_states, &errors, file, d.key, "effects");
    }
    for (size_t i = 0; i < out->advisors.size(); ++i) {
        const AdvisorDef& d = out->advisors[i];
        const std::string file = i < advisor_src.size() ? advisor_src[i] : std::string();
        validate_trigger(d.available, *out, no_states, &errors, file, d.key, "available");
    }

    if (err) *err = errors.empty() ? std::string() : errors.front();
    return true;
}

}  // namespace hoi
