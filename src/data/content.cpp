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

#include "data/mod.h"

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

int match_resource(const std::string& key) {
    return match_enum_name([](int i) { return resource_name(static_cast<Resource>(i)); },
                           RESOURCE_COUNT, key);
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
        const int idx = match_resource(item.first);
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

// ---------------------------------------------------------------- mod layer --
//
// Content is assembled before it is parsed: every table is one document that the
// mods merge into, so the parsing/validation passes below see a single, already
// resolved view and never have to know where a definition came from. A mod that
// fails validation is rejected before any of its files touch that view, so a bad
// mod cannot leave half-applied content behind.

enum class VKind { Any, Num, Str, Bool, Arr, Obj };

struct VField {
    const char* name;
    VKind kind;
};

const char* kind_name(VKind k) {
    switch (k) {
        case VKind::Num: return "a number";
        case VKind::Str: return "a string";
        case VKind::Bool: return "true or false";
        case VKind::Arr: return "an array";
        case VKind::Obj: return "an object";
        case VKind::Any: break;
    }
    return "a value";
}

// One keyed content table: `path` is relative to the data root, `array` the member
// holding its entries (`focuses` is a directory of files with the same shape).
// `array2` is a second entry list in the same file, used by the intelligence table
// whose one document holds both `operations` and `agency_upgrades`.
struct ContentTable {
    const char* name;
    const char* path;
    const char* array;
    bool directory;
    const char* array2 = "";
};

// Every entry-list member a table accepts, in file order.
std::vector<std::string> table_arrays(const ContentTable& t) {
    std::vector<std::string> out;
    if (t.array[0] != '\0') out.push_back(t.array);
    if (t.array2[0] != '\0') out.push_back(t.array2);
    return out;
}

const std::vector<ContentTable>& content_tables() {
    static const std::vector<ContentTable> kTables = {
        {"constants", "common/constants.json", "", false},
        {"equipment", "common/equipment.json", "equipment", false},
        {"buildings", "common/buildings.json", "buildings", false},
        {"technologies", "common/technologies.json", "technologies", false},
        {"laws", "common/laws.json", "laws", false},
        {"templates", "common/templates.json", "templates", false},
        {"focuses", "common/focuses", "focuses", true},
        {"events", "common/events.json", "events", false},
        {"decisions", "common/decisions.json", "decisions", false},
        {"spirits", "common/spirits.json", "spirits", false},
        {"advisors", "common/advisors.json", "advisors", false},
        {"components", "common/components.json", "components", false},
        // One document, two entry lists: operations run against another country and
        // the agency upgrades that improve a country's service.
        {"intelligence", "common/intelligence.json", "operations", false, "agency_upgrades"},
    };
    return kTables;
}

const ContentTable* table_by_name(const std::string& name) {
    for (const ContentTable& t : content_tables()) {
        if (name == t.name) return &t;
    }
    return nullptr;
}

// `replace_paths` and mod data files both accept a logical table name ("equipment")
// or a data path ("common/equipment.json", "common/focuses/x.json") so the natural
// spelling of either side works.
const ContentTable* table_by_reference(const std::string& ref) {
    if (const ContentTable* t = table_by_name(ref)) return t;
    for (const ContentTable& t : content_tables()) {
        const std::string path = t.path;
        if (ref == path) return &t;
        if (t.directory && ref.rfind(path + "/", 0) == 0) return &t;
    }
    return nullptr;
}

// A .json file inside a mod is valid only when its path names a table.
const ContentTable* table_by_file(const std::string& rel) {
    for (const ContentTable& t : content_tables()) {
        const std::string path = t.path;
        if (!t.directory) {
            if (rel == path) return &t;
        } else if (rel.rfind(path + "/", 0) == 0 && rel.size() > path.size() + 1 &&
                   rel.compare(rel.size() - 5, 5, ".json") == 0) {
            return &t;
        }
    }
    return nullptr;
}

bool required_table(const std::string& name) {
    return name == "constants" || name == "equipment" || name == "buildings" ||
           name == "technologies" || name == "laws" || name == "templates";
}

// The fields a definition may carry, derived from what the parser below reads.
// A field that is absent is fine; a field that is present with the wrong type, or a
// field the parser would ignore, is an error in a mod. The intelligence table hosts
// two different entry shapes in one file, so its `member` array selects the schema.
const std::vector<VField>& table_fields(const std::string& table, const std::string& member = "") {
    if (table == "intelligence") {
        static const std::vector<VField> kOperations = {
            {"key", VKind::Str},           {"name", VKind::Str},
            {"description", VKind::Str},   {"kind", VKind::Str},
            {"days", VKind::Num},          {"pp_cost", VKind::Num},
            {"civilian_cost", VKind::Num}, {"network_required", VKind::Num},
            {"risk", VKind::Num},          {"network_gain", VKind::Num},
            {"research_days", VKind::Num}, {"output_penalty", VKind::Num},
            {"stability_delta", VKind::Num}, {"ideology_shift", VKind::Num},
            {"effect_days", VKind::Num},   {"available", VKind::Obj},
            {"effect", VKind::Obj}};
        static const std::vector<VField> kAgencyUpgrades = {
            {"key", VKind::Str},           {"name", VKind::Str},
            {"description", VKind::Str},   {"year", VKind::Num},
            {"pp_cost", VKind::Num},       {"requires_upgrades", VKind::Arr},
            {"network_growth", VKind::Num}, {"operation_speed", VKind::Num},
            {"crypto_speed", VKind::Num},  {"counter_intel", VKind::Num},
            {"available", VKind::Obj}};
        return member == "agency_upgrades" ? kAgencyUpgrades : kOperations;
    }
    static const std::map<std::string, std::vector<VField>> kFields = {
        {"constants", {}},  // validated against the engine's known constant set
        {"equipment",
         {{"key", VKind::Str},        {"name", VKind::Str},       {"category", VKind::Str},
          {"year", VKind::Num},       {"archetype", VKind::Str},  {"is_archetype", VKind::Bool},
          {"soft_attack", VKind::Num}, {"hard_attack", VKind::Num}, {"air_attack", VKind::Num},
          {"air_defence", VKind::Num}, {"ground_attack", VKind::Num}, {"agility", VKind::Num},
          {"range", VKind::Num},      {"naval_attack", VKind::Num}, {"torpedo_attack", VKind::Num},
          {"sub_detection", VKind::Num}, {"detection", VKind::Num}, {"visibility", VKind::Num},
          {"defense", VKind::Num},    {"breakthrough", VKind::Num}, {"armor", VKind::Num},
          {"piercing", VKind::Num},   {"hardness", VKind::Num},    {"reliability", VKind::Num},
          {"speed", VKind::Num},      {"max_strength", VKind::Num}, {"organization", VKind::Num},
          {"build_cost", VKind::Num}, {"fuel_use", VKind::Num},    {"supply_use", VKind::Num},
          {"manpower", VKind::Num},   {"resources", VKind::Obj}}},
        {"buildings",
         {{"key", VKind::Str},   {"name", VKind::Str},     {"kind", VKind::Str},
          {"base_cost", VKind::Num}, {"per_state", VKind::Bool}, {"max_level", VKind::Num}}},
        {"technologies",
         {{"key", VKind::Str},        {"name", VKind::Str},        {"category", VKind::Str},
          {"year", VKind::Num},       {"cost_days", VKind::Num},   {"modifiers", VKind::Obj},
          {"prerequisites", VKind::Arr}, {"unlock_equipment", VKind::Arr},
          {"unlock_buildings", VKind::Arr}}},
        {"laws",
         {{"key", VKind::Str},      {"name", VKind::Str},        {"kind", VKind::Num},
          {"level", VKind::Num},    {"cost", VKind::Num},        {"modifiers", VKind::Obj},
          {"requires_law", VKind::Str}, {"requires_level", VKind::Num}}},
        {"templates",
         {{"key", VKind::Str}, {"name", VKind::Str}, {"train_days", VKind::Num},
          {"battalions", VKind::Arr}}},
        {"focuses",
         {{"key", VKind::Str},            {"name", VKind::Str},       {"tree", VKind::Str},
          {"x", VKind::Num},              {"y", VKind::Num},          {"days", VKind::Num},
          {"prerequisites", VKind::Arr},  {"mutually_exclusive", VKind::Arr},
          {"available", VKind::Obj},      {"bypass", VKind::Obj},     {"effects", VKind::Obj},
          {"ai_weight", VKind::Num}}},
        {"events",
         {{"key", VKind::Str},      {"title", VKind::Str},        {"description", VKind::Str},
          {"fire_only_once", VKind::Bool}, {"major", VKind::Bool}, {"trigger", VKind::Obj},
          {"immediate", VKind::Obj}, {"options", VKind::Arr}}},
        {"decisions",
         {{"key", VKind::Str},         {"name", VKind::Str},      {"description", VKind::Str},
          {"category", VKind::Num},    {"targets_state", VKind::Bool}, {"cost_pp", VKind::Num},
          {"days_remove", VKind::Num}, {"days_cooldown", VKind::Num},
          {"visible", VKind::Obj},     {"available", VKind::Obj}, {"effects", VKind::Obj},
          {"remove_effect", VKind::Obj}, {"ai_weight", VKind::Num}}},
        {"spirits",
         {{"key", VKind::Str},  {"name", VKind::Str}, {"description", VKind::Str},
          {"slots", VKind::Num}, {"available", VKind::Obj}, {"modifiers", VKind::Obj},
          {"effects", VKind::Obj}}},
        {"advisors",
         {{"key", VKind::Str},  {"name", VKind::Str}, {"description", VKind::Str},
          {"cost_pp", VKind::Num}, {"available", VKind::Obj}, {"modifiers", VKind::Obj}}},
        {"components",
         {{"key", VKind::Str},          {"name", VKind::Str},        {"slot", VKind::Str},
          {"category", VKind::Str},     {"year", VKind::Num},        {"soft_attack", VKind::Num},
          {"hard_attack", VKind::Num},  {"air_attack", VKind::Num},  {"air_defence", VKind::Num},
          {"ground_attack", VKind::Num}, {"agility", VKind::Num},    {"armor", VKind::Num},
          {"piercing", VKind::Num},     {"defense", VKind::Num},     {"breakthrough", VKind::Num},
          {"hardness", VKind::Num},     {"max_strength", VKind::Num}, {"organization", VKind::Num},
          {"speed", VKind::Num},        {"reliability", VKind::Num}, {"range", VKind::Num},
          {"detection", VKind::Num},    {"sub_detection", VKind::Num},
          {"naval_attack", VKind::Num}, {"torpedo_attack", VKind::Num},
          {"visibility", VKind::Num},   {"build_cost_add", VKind::Num},
          {"cost_multiplier", VKind::Num}, {"fuel_use", VKind::Num}, {"supply_use", VKind::Num},
          {"manpower", VKind::Num},     {"resources", VKind::Obj},   {"available", VKind::Obj}}},
    };
    auto it = kFields.find(table);
    return it == kFields.end() ? kFields.at("constants") : it->second;
}

bool kind_ok(const Json& v, VKind k) {
    if (v.is_null()) return true;  // optional blocks may be declared null
    switch (k) {
        case VKind::Any: return true;
        case VKind::Num: return v.is_number();
        case VKind::Str: return v.is_string();
        case VKind::Bool: return v.is_bool();
        case VKind::Arr: return v.is_array();
        case VKind::Obj: return v.is_object();
    }
    return false;
}

void check_named_numbers(const Json& obj, const char* what,
                         int (*known)(const std::string&),
                         std::vector<std::string>* reasons) {
    if (obj.is_null()) return;
    if (!obj.is_object()) {
        reasons->push_back(std::string(what) + " must be an object");
        return;
    }
    for (const auto& item : obj.object_items()) {
        if (known(item.first) < 0) {
            reasons->push_back("unknown " + std::string(what) + " '" + item.first + "'");
        } else if (!item.second.is_number()) {
            reasons->push_back(std::string(what) + " '" + item.first + "' must be a number");
        }
    }
}

void check_string_array(const Json& arr, const char* what, std::vector<std::string>* reasons) {
    if (arr.is_null()) return;  // an absent optional array is not an error
    if (!arr.is_array()) {
        reasons->push_back(std::string(what) + " must be an array of strings");
        return;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_string() || arr[i].as_string().empty()) {
            reasons->push_back(std::string(what) + "[" + std::to_string(i) +
                               "] must be a non-empty string");
        }
    }
}

void check_entry_array(const Json& arr, const char* what, const std::vector<VField>& fields,
                       std::vector<std::string>* reasons) {
    if (arr.is_null()) return;  // an absent optional array is not an error
    if (!arr.is_array()) {
        reasons->push_back(std::string(what) + " must be an array");
        return;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& e = arr[i];
        if (!e.is_object()) {
            reasons->push_back(std::string(what) + "[" + std::to_string(i) + "] must be an object");
            continue;
        }
        for (const auto& item : e.object_items()) {
            const VField* f = nullptr;
            for (const VField& v : fields) {
                if (item.first == v.name) {
                    f = &v;
                    break;
                }
            }
            if (f == nullptr) {
                reasons->push_back(std::string(what) + "[" + std::to_string(i) +
                                   "]: unknown key '" + item.first + "'");
            } else if (!kind_ok(item.second, f->kind)) {
                reasons->push_back(std::string(what) + "[" + std::to_string(i) + "]." +
                                   item.first + " expects " + kind_name(f->kind));
            }
        }
    }
}

// Validates one entry of a mod against the fields the parser reads. `reasons` gets
// one message per problem; an empty list means the entry is safe to merge. `member`
// names the entry list inside a multi-array table (the intelligence file).
void validate_mod_entry(const std::string& table, const std::string& member, const Json& e,
                        std::vector<std::string>* reasons) {
    const std::vector<VField>& fields = table_fields(table, member);
    for (const auto& item : e.object_items()) {
        if (item.first == "add") {
            if (!item.second.is_bool()) reasons->push_back("add must be true or false");
            continue;
        }
        const VField* f = nullptr;
        for (const VField& v : fields) {
            if (item.first == v.name) {
                f = &v;
                break;
            }
        }
        if (f == nullptr) {
            reasons->push_back("unknown key '" + item.first + "'");
        } else if (!kind_ok(item.second, f->kind)) {
            reasons->push_back("key '" + item.first + "' expects " + kind_name(f->kind));
        }
    }
    if (table == "equipment" || table == "components") {
        check_named_numbers(e["resources"], "resource", match_resource, reasons);
    }
    if (table == "technologies" || table == "laws" || table == "spirits" ||
        table == "advisors") {
        check_named_numbers(e["modifiers"], "modifier", match_modifier, reasons);
    }
    if (table == "technologies") {
        check_string_array(e["prerequisites"], "prerequisites", reasons);
        check_string_array(e["unlock_equipment"], "unlock_equipment", reasons);
        check_string_array(e["unlock_buildings"], "unlock_buildings", reasons);
    }
    if (table == "focuses") {
        check_string_array(e["prerequisites"], "prerequisites", reasons);
        check_string_array(e["mutually_exclusive"], "mutually_exclusive", reasons);
    }
    if (table == "templates") {
        static const std::vector<VField> kBattalion = {
            {"equipment", VKind::Str}, {"count", VKind::Num}, {"support", VKind::Bool}};
        check_entry_array(e["battalions"], "battalions", kBattalion, reasons);
    }
    if (table == "events") {
        static const std::vector<VField> kOption = {
            {"name", VKind::Str}, {"effects", VKind::Obj}, {"ai_weight", VKind::Num}};
        check_entry_array(e["options"], "options", kOption, reasons);
    }
    if (table == "intelligence" && member == "agency_upgrades") {
        check_string_array(e["requires_upgrades"], "requires_upgrades", reasons);
    }
}

// One content file merged from the base data root and its mods.
struct SourceFile {
    std::string rel;
    std::string full;  // path used in diagnostics
    std::string table;
    Json doc;
    std::vector<std::string> origin;   // per entry of `array`: which file defined it
    std::vector<std::string> origin2;  // per entry of `array2`, when the table has one
};

SourceFile* find_source(std::vector<SourceFile>* files, const std::string& rel) {
    for (SourceFile& f : *files) {
        if (f.rel == rel) return &f;
    }
    return nullptr;
}

// Loads the base data root. Required tables must exist exactly as before mods.
bool load_base_source(const std::string& root, std::vector<SourceFile>* files, std::string* err) {
    namespace fs = std::filesystem;
    for (const ContentTable& t : content_tables()) {
        if (t.directory) {
            std::error_code ec;
            const std::string dir = root + "/" + t.path;
            std::vector<std::string> names;
            if (fs::is_directory(dir, ec)) {
                for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    const std::string p = entry.path().string();
                    if (p.size() > 5 && p.compare(p.size() - 5, 5, ".json") == 0) {
                        names.push_back(entry.path().filename().string());
                    }
                }
            }
            std::sort(names.begin(), names.end());
            for (const std::string& name : names) {
                SourceFile sf;
                sf.rel = std::string(t.path) + "/" + name;
                sf.full = dir + "/" + name;
                sf.table = t.name;
                if (!load_json_file(sf.full, &sf.doc, err)) return false;
                const Json& arr = sf.doc[t.array];
                for (size_t i = 0; i < (arr.is_array() ? arr.size() : 0); ++i) {
                    sf.origin.push_back(sf.full);
                }
                files->push_back(std::move(sf));
            }
            continue;
        }
        const std::string full = root + "/" + t.path;
        if (!fs::exists(full) && !required_table(t.name)) continue;
        SourceFile sf;
        sf.rel = t.path;
        sf.full = full;
        sf.table = t.name;
        if (!load_json_file(full, &sf.doc, err)) return false;
        if (t.array[0] != '\0') {
            const Json& arr = sf.doc[t.array];
            for (size_t i = 0; i < (arr.is_array() ? arr.size() : 0); ++i) {
                sf.origin.push_back(full);
            }
        }
        if (t.array2[0] != '\0') {
            const Json& arr = sf.doc[t.array2];
            for (size_t i = 0; i < (arr.is_array() ? arr.size() : 0); ++i) {
                sf.origin2.push_back(full);
            }
        }
        files->push_back(std::move(sf));
    }
    return true;
}

// ------------------------------------------------------------- mod apply ----

struct MergeEntry {
    const ContentTable* table;
    std::string array;  // which entry list of the table the entry belongs to
    std::string rel;   // destination when the entry appends
    std::string full;
    std::string key;   // effective key (a leading `add_` is stripped)
    std::string tree;  // focus files: the file's default tree, kept when the
                       // destination file has to be created for an addition
    Json entry;        // normalized: `add` removed, key set to the effective key
    bool append = false;
};

// Collects the .json files a mod ships under <mod>/common, in path order.
void collect_mod_files(const ModManifest& mod, std::vector<std::string>* rels,
                       std::vector<std::string>* fulls) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string base = mod.directory + "/";
    std::vector<std::string> rel;
    std::vector<std::string> full;
    const std::string dir = mod.directory + "/common";
    if (fs::is_directory(dir, ec)) {
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string p = entry.path().string();
            if (p.size() <= 5 || p.compare(p.size() - 5, 5, ".json") != 0) continue;
            std::string r = p.substr(base.size());
            for (char& ch : r) {
                if (ch == '\\') ch = '/';
            }
            rel.push_back(r);
            full.push_back(p);
        }
    }
    // Deterministic order independent of the directory iterator.
    std::vector<size_t> order(rel.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&rel](size_t a, size_t b) { return rel[a] < rel[b]; });
    for (size_t i : order) {
        rels->push_back(rel[i]);
        fulls->push_back(full[i]);
    }
}

bool is_add_key(const std::string& key) {
    return key.size() > 4 && key.compare(0, 4, "add_") == 0;
}

// Applies one mod to the merged source. Returns false when the mod was rejected:
// nothing of it is merged and `events` holds one error per problem.
bool apply_one_mod(const ModManifest& mod, std::vector<SourceFile>* files,
                   std::vector<ModLoadEvent>* events) {
    const auto event = [&](const std::string& table, const std::string& key,
                           const std::string& action, const std::string& detail) {
        events->push_back(ModLoadEvent{mod.name, table, key, action, detail});
    };

    std::vector<std::string> rels;
    std::vector<std::string> fulls;
    collect_mod_files(mod, &rels, &fulls);

    for (const std::string& ref : mod.replace_paths) {
        if (table_by_reference(ref) == nullptr) {
            event(ref, "", "error", "replace_paths names unknown table '" + ref + "'");
        }
    }
    bool skip = events->size() > 0;

    // ---- phase 1: parse and validate every file before anything is merged ----
    std::vector<MergeEntry> plan;
    std::map<std::string, std::set<std::string>> seen;  // table -> keys from this mod
    for (size_t fi = 0; fi < rels.size(); ++fi) {
        const std::string& rel = rels[fi];
        const std::string& full = fulls[fi];
        const ContentTable* table = table_by_file(rel);
        if (table == nullptr) {
            event(rel, "", "unknown_file", "");
            skip = true;
            continue;
        }
        Json doc;
        std::string parse_err;
        if (!Json::parse_file(full, &doc, &parse_err)) {
            event(rel, "", "parse", parse_err.empty() ? "cannot read file" : parse_err);
            skip = true;
            continue;
        }
        if (!doc.is_object()) {
            event(rel, "", "error", "content file must be an object");
            skip = true;
            continue;
        }
        if (table->array[0] == '\0') {
            // constants: every key must be one the engine reads.
            const SourceFile* base = find_source(files, table->path);
            for (const auto& item : doc.object_items()) {
                if (item.first == "add") continue;
                if (!item.second.is_number()) {
                    event(table->name, item.first, "error", "constant must be a number");
                    skip = true;
                    continue;
                }
                if (base != nullptr && !base->doc.has(item.first)) {
                    event(table->name, item.first, "error", "unknown constant");
                    skip = true;
                    continue;
                }
                std::set<std::string>& keys = seen[table->name];
                if (!keys.insert(item.first).second) {
                    event(table->name, item.first, "error", "duplicate key in this mod");
                    skip = true;
                    continue;
                }
                MergeEntry me;
                me.table = table;
                me.rel = rel;
                me.full = full;
                me.key = item.first;
                me.entry = item.second;
                plan.push_back(std::move(me));
            }
            continue;
        }

        // An array table accepts `<array>`, `add_<array>` and (focuses) `tree`. A table
        // with a second entry list (intelligence) accepts both lists and their `add_`
        // forms; `member` records which list the current key feeds.
        std::string member;
        for (const auto& item : doc.object_items()) {
            const std::string& k = item.first;
            member.clear();
            for (const std::string& a : table_arrays(*table)) {
                if (k == a || k == "add_" + a) {
                    member = a;
                    break;
                }
            }
            const bool is_add = !member.empty() && k == "add_" + member;
            const bool is_tree = std::string(table->name) == "focuses" && k == "tree";
            if (member.empty() && !is_tree) {
                event(table->name, k, "error", "unknown table member '" + k + "'");
                skip = true;
                continue;
            }
            if (is_tree) {
                if (!item.second.is_string()) {
                    event(table->name, k, "error", "tree must be a string");
                    skip = true;
                }
                continue;
            }
            if (!item.second.is_array()) {
                event(table->name, k, "error", "expected an array");
                skip = true;
                continue;
            }
            const Json& arr = item.second;
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& e = arr[i];
                if (!e.is_object()) {
                    event(table->name, "<entry " + std::to_string(i) + ">", "error",
                          "entry must be an object");
                    skip = true;
                    continue;
                }
                std::string key = e["key"].as_string();
                if (key.empty()) {
                    event(table->name, "<entry " + std::to_string(i) + ">", "error",
                          "missing key");
                    skip = true;
                    continue;
                }
                MergeEntry me;
                me.table = table;
                me.array = member;
                me.rel = rel;
                me.full = full;
                me.tree = doc["tree"].as_string();
                me.append = is_add || e["add"].as_bool(false) || is_add_key(key);
                if (is_add_key(key)) key = key.substr(4);
                me.key = key;
                std::vector<std::string> reasons;
                validate_mod_entry(table->name, member, e, &reasons);
                for (const std::string& r : reasons) {
                    event(table->name, key, "error", r);
                    skip = true;
                }
                std::set<std::string>& keys = seen[table->name];
                if (!keys.insert(key).second) {
                    event(table->name, key, "error", "duplicate key in this mod");
                    skip = true;
                }
                if (me.append) {
                    for (const SourceFile& sf : *files) {
                        if (sf.table != table->name) continue;
                        const Json& arr2 = sf.doc[member];
                        for (size_t j = 0; j < (arr2.is_array() ? arr2.size() : 0); ++j) {
                            if (arr2[j]["key"].as_string() == key) {
                                event(table->name, key, "error",
                                      "addition collides with an existing definition");
                                skip = true;
                            }
                        }
                    }
                }
                Json normalized = Json::object();
                for (const auto& kv : e.object_items()) {
                    if (kv.first == "add") continue;
                    normalized.set(kv.first, kv.second);
                }
                normalized.set("key", Json(key));
                me.entry = std::move(normalized);
                plan.push_back(std::move(me));
            }
        }
    }
    if (skip) return false;

    // ---- phase 2: replace_paths clears the tables this mod owns outright ----
    for (const std::string& ref : mod.replace_paths) {
        const ContentTable* t = table_by_reference(ref);
        if (t == nullptr) continue;
        for (SourceFile& sf : *files) {
            if (sf.table != t->name) continue;
            if (t->array[0] == '\0') {
                sf.doc = Json::object();
            } else {
                for (const std::string& a : table_arrays(*t)) sf.doc.set(a, Json::array());
            }
            sf.origin.clear();
            sf.origin2.clear();
        }
    }

    // ---- phase 3: merge ----
    // The origin list an entry belongs to follows the entry list it feeds.
    auto origin_of = [](SourceFile* sf, const ContentTable& t,
                        const std::string& arr) -> std::vector<std::string>& {
        return arr == t.array ? sf->origin : sf->origin2;
    };
    for (const MergeEntry& me : plan) {
        const ContentTable& t = *me.table;
        std::string owner;
        size_t owner_index = 0;
        bool found = false;
        if (t.array[0] != '\0') {
            for (const SourceFile& sf : *files) {
                if (sf.table != t.name) continue;
                const Json& arr = sf.doc[me.array];
                for (size_t j = 0; j < (arr.is_array() ? arr.size() : 0); ++j) {
                    if (arr[j]["key"].as_string() == me.key) {
                        owner = sf.rel;
                        owner_index = j;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
        if (t.array[0] == '\0') {
            SourceFile* dst = find_source(files, t.path);
            const bool existed = dst != nullptr && dst->doc.has(me.key);
            if (dst == nullptr) {
                SourceFile sf;
                sf.rel = t.path;
                sf.full = me.full;
                sf.table = t.name;
                sf.doc = Json::object();
                files->push_back(std::move(sf));
                dst = &files->back();
            }
            dst->doc.set(me.key, me.entry);
            event(t.name, me.key, existed ? "replaced" : "added", "");
            continue;
        }
        if (found) {
            SourceFile* dst = find_source(files, owner);
            Json arr = dst->doc[me.array];
            Json rebuilt = Json::array();
            for (size_t j = 0; j < arr.size(); ++j) {
                rebuilt.push_back(j == owner_index ? me.entry : arr[j]);
            }
            dst->doc.set(me.array, std::move(rebuilt));
            origin_of(dst, t, me.array)[owner_index] = me.full;
            event(t.name, me.key, "replaced", "");
        } else {
            SourceFile* dst = find_source(files, me.rel);
            if (dst == nullptr) {
                SourceFile sf;
                sf.rel = me.rel;
                sf.full = me.full;
                sf.table = t.name;
                sf.doc = Json::object();
                // A focus file's `tree` is the default for its entries.
                if (t.name == std::string("focuses")) {
                    sf.doc.set("tree", Json(me.tree.empty() ? std::string("shared") : me.tree));
                }
                files->push_back(std::move(sf));
                dst = &files->back();
            }
            Json arr = dst->doc[me.array];
            if (!arr.is_array()) arr = Json::array();
            arr.push_back(me.entry);
            dst->doc.set(me.array, std::move(arr));
            origin_of(dst, t, me.array).push_back(me.full);
            event(t.name, me.key, "added", "");
        }
    }

    std::stable_sort(events->begin(), events->end(),
                     [](const ModLoadEvent& a, const ModLoadEvent& b) {
                         if (a.table != b.table) return a.table < b.table;
                         return a.key < b.key;
                     });
    return true;
}

void append_lines(const std::string& text, std::vector<std::string>* out) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string line =
            text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty()) out->push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// Discovers every mods root, resolves the global load order and merges the accepted
// mods into `files`. A rejected mod contributes nothing; its diagnostics land in
// `diags` (and in the report when one was passed).
void apply_mods(const std::vector<std::string>& mod_roots, std::vector<SourceFile>* files,
                ModLoadReport* report, std::vector<std::string>* diags) {
    if (mod_roots.empty()) return;
    std::vector<ModManifest> manifests;
    std::vector<std::string> notes;
    for (const std::string& root : mod_roots) {
        std::string err;
        discover_mods(root, &manifests, &err);
        append_lines(err, &notes);
    }
    std::string order_err;
    const std::vector<const ModManifest*> order = resolve_load_order(manifests, &order_err);
    append_lines(order_err, &notes);

    if (report != nullptr) {
        for (const std::string& n : notes) report->notes.push_back(n);
        for (const ModManifest* m : order) report->load_order.push_back(m->name);
    }
    for (const std::string& n : notes) {
        if (report != nullptr) report->errors.push_back(n);
        diags->push_back(n);
    }
    for (const ModManifest* m : order) {
        std::vector<ModLoadEvent> events;
        const bool applied = apply_one_mod(*m, files, &events);
        for (const ModLoadEvent& e : events) {
            if (report != nullptr) report->events.push_back(e);
            if (applied) continue;
            const std::string line = mod_event_text(e);
            if (report != nullptr) report->errors.push_back(line);
            diags->push_back(line);
        }
    }
}

}  // namespace

// Canonical engine spellings for the operation kinds. `match_operation_kind`
// accepts them case- and separator-insensitively, like every other data name in
// the loader, so "Build Network", "build_network" and "buildnetwork" all resolve.
const char* operation_kind_name(OperationKind kind) {
    switch (kind) {
        case OperationKind::BuildNetwork: return "build_network";
        case OperationKind::StealTech: return "steal_tech";
        case OperationKind::SabotageIndustry: return "sabotage_industry";
        case OperationKind::SupportIdeology: return "support_ideology";
        case OperationKind::Destabilise: return "destabilise";
        case OperationKind::CounterIntel: return "counter_intel";
        case OperationKind::Count: break;
    }
    return "unknown";
}

bool match_operation_kind(const std::string& name, OperationKind* out) {
    const std::string want = normalize_name(name);
    if (want.empty()) return false;
    for (int i = 0; i < static_cast<int>(OperationKind::Count); ++i) {
        const OperationKind kind = static_cast<OperationKind>(i);
        if (normalize_name(operation_kind_name(kind)) == want) {
            if (out != nullptr) *out = kind;
            return true;
        }
    }
    return false;
}

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

bool load_content(const std::string& data_root, Content* out, std::string* err,
                  const std::vector<std::string>& mod_roots, ModLoadReport* report) {
    if (out == nullptr) {
        if (err) *err = "load_content: null output";
        return false;
    }
    if (report != nullptr) {
        report->events.clear();
        report->notes.clear();
        report->errors.clear();
        report->load_order.clear();
    }
    out->load_errors.clear();
    std::vector<std::string>& errors = out->load_errors;
    std::string root = data_root;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();

    // The base data root and every accepted mod are merged into one source before
    // anything is parsed, so a rejected mod can never leave half-applied content.
    std::vector<SourceFile> files;
    std::string file_err;
    if (!load_base_source(root, &files, &file_err)) {
        errors.push_back(file_err);
        if (err) *err = file_err;
        return false;
    }
    std::vector<std::string> mod_diags;
    apply_mods(mod_roots, &files, report, &mod_diags);
    errors.insert(errors.end(), mod_diags.begin(), mod_diags.end());

    // ---- constants -------------------------------------------------------
    const SourceFile* sf_constants = find_source(&files, "common/constants.json");
    const Json* doc_constants = sf_constants == nullptr ? nullptr : &sf_constants->doc;
    if (doc_constants == nullptr) {
        const std::string msg = root + "/common/constants.json: cannot read file";
        errors.push_back(msg);
        if (err) *err = msg;
        return false;
    }
    out->constants = SimConstants::from_json(*doc_constants);

    // ---- equipment -------------------------------------------------------
    {
        const SourceFile* sf = find_source(&files, "common/equipment.json");
        const std::string f_equipment = sf == nullptr ? root + "/common/equipment.json" : sf->full;
        const Json& arr = (*sf).doc["equipment"];
        if (!arr.is_array()) {
            const std::string msg = f_equipment + ":equipment: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& e = arr[i];
            const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_equipment;
            const std::string key = e["key"].as_string();
            if (key.empty()) {
                const std::string msg = f_entry + ":<entry " + std::to_string(i) +
                                        ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->equipment_by_key.count(key) != 0) {
                const std::string msg = f_entry + ":" + key + ": duplicate equipment key";
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
                errors.push_back(f_entry + ":" + key + ": unknown category " + category_name);
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
                    errors.push_back(f_entry + ":" + key + ": negative " + field +
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
            parse_resource_costs(e["resources"], def.resources, &errors, f_entry, key);
            out->equipment_by_key[key] = def.id;
            out->equipment.push_back(std::move(def));
        }
        // Model archetypes must exist once every entry has an id.
        for (size_t i = 0; i < out->equipment.size(); ++i) {
            EquipmentDef& def = out->equipment[i];
            if (def.archetype.empty()) continue;
            if (out->equipment_by_key.count(def.archetype) == 0) {
                const std::string& src = i < sf->origin.size() ? sf->origin[i] : f_equipment;
                errors.push_back(src + ":" + def.key + ": unknown archetype " + def.archetype);
            }
        }
    }

    // ---- buildings (before techs: technologies may unlock building keys) ---
    {
        const SourceFile* sf = find_source(&files, "common/buildings.json");
        const std::string f_buildings = sf == nullptr ? root + "/common/buildings.json" : sf->full;
        const Json& arr = (*sf).doc["buildings"];
        if (!arr.is_array()) {
            const std::string msg = f_buildings + ":buildings: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& b = arr[i];
            const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_buildings;
            const std::string key = b["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_entry + ":<entry " + std::to_string(i) + ">: missing key";
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
                errors.push_back(f_entry + ":" + key + ": unknown building kind " + kind_name);
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
    std::vector<std::string> tech_src;
    {
        const SourceFile* sf = find_source(&files, "common/technologies.json");
        const std::string f_technologies =
            sf == nullptr ? root + "/common/technologies.json" : sf->full;
        const Json& doc = (*sf).doc;
        const Json& arr = doc["technologies"];
        if (!arr.is_array()) {
            const std::string msg = f_technologies + ":technologies: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& t = arr[i];
            const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_technologies;
            const std::string key = t["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_entry + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->tech_by_key.count(key) != 0) {
                const std::string msg = f_entry + ":" + key + ": duplicate technology key";
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
            def.modifiers = parse_modifiers(t["modifiers"], &errors, f_entry, key);
            const Json& unlocks_eq = t["unlock_equipment"];
            if (unlocks_eq.is_array()) {
                for (size_t k = 0; k < unlocks_eq.size(); ++k) {
                    const std::string ek = unlocks_eq[k].as_string();
                    if (ek.empty()) continue;
                    // The key is checked against the equipment and component tables in
                    // the cross-reference pass: components load after technologies so a
                    // technology may gate one of them.
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
                        errors.push_back(f_entry + ":" + key +
                                         ": unlock_buildings references unknown building " + bk);
                    }
                    def.unlock_buildings.push_back(bk);
                }
            }
            out->tech_by_key[key] = def.id;
            out->techs.push_back(std::move(def));
            tech_src.push_back(f_entry);
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
                        const std::string& src =
                            i < sf->origin.size() ? sf->origin[i] : f_technologies;
                        errors.push_back(src + ":" + def.key + ": unknown prerequisite " + pk);
                        continue;
                    }
                    def.prerequisites.push_back(it->second);
                }
                break;
            }
        }
    }

    // ---- laws ------------------------------------------------------------
    {
        const SourceFile* sf = find_source(&files, "common/laws.json");
        const std::string f_laws = sf == nullptr ? root + "/common/laws.json" : sf->full;
        const Json& arr = (*sf).doc["laws"];
        if (!arr.is_array()) {
            const std::string msg = f_laws + ":laws: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& l = arr[i];
            const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_laws;
            const std::string key = l["key"].as_string();
            if (key.empty()) {
                const std::string msg = f_entry + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->law_index.count(key) != 0) {
                const std::string msg = f_entry + ":" + key + ": duplicate law key";
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
            def.modifiers = parse_modifiers(l["modifiers"], &errors, f_entry, key);
            def.requires_law = l["requires_law"].as_string();
            def.requires_level = static_cast<int>(l["requires_level"].as_int(def.requires_level));
            out->law_index[key] = static_cast<int>(out->laws.size());
            out->laws.push_back(std::move(def));
        }
        // Law prerequisite keys must resolve to laws in the same file.
        for (size_t i = 0; i < out->laws.size(); ++i) {
            const LawDef& def = out->laws[i];
            if (!def.requires_law.empty() && out->law_index.count(def.requires_law) == 0) {
                const std::string& src = i < sf->origin.size() ? sf->origin[i] : f_laws;
                errors.push_back(src + ":" + def.key + ": unknown requires_law " +
                                 def.requires_law);
            }
        }
    }

    // ---- division templates ----------------------------------------------
    {
        const SourceFile* sf = find_source(&files, "common/templates.json");
        const std::string f_templates =
            sf == nullptr ? root + "/common/templates.json" : sf->full;
        const Json& arr = (*sf).doc["templates"];
        if (!arr.is_array()) {
            const std::string msg = f_templates + ":templates: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& t = arr[i];
            const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_templates;
            const std::string key = t["key"].as_string();
            if (key.empty()) {
                const std::string msg =
                    f_entry + ":<entry " + std::to_string(i) + ">: missing key";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            if (out->template_by_key.count(key) != 0) {
                const std::string msg = f_entry + ":" + key + ": duplicate template key";
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
                        errors.push_back(f_entry + ":" + key +
                                         ": unknown equipment " + eq_key);
                        continue;
                    }
                    BattalionSlot slot;
                    slot.equipment = eq;
                    slot.count = static_cast<int>(entry["count"].as_int(0));
                    slot.support = entry["support"].as_bool(false);
                    if (slot.count <= 0) {
                        errors.push_back(f_entry + ":" + key +
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
    // Every focus file in source order: base files sorted by path, then mod files in
    // resolved load order, so focus indices (and therefore save files) are stable.
    // Each file is {"tree": "...", "focuses": []}.
    std::vector<std::string> focus_src;
    for (const SourceFile& sf : files) {
        if (sf.table != "focuses") continue;
        const std::string& f = sf.full;
        const Json& fd = sf.doc;
        const std::string default_tree = fd["tree"].as_string("shared");
        const Json& arr = fd["focuses"];
        if (!arr.is_array()) {
            errors.push_back(f + ":focuses: expected an array");
            continue;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& fj = arr[i];
            const std::string& f_entry = i < sf.origin.size() ? sf.origin[i] : f;
            const std::string key = fj["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_entry + ":<entry " + std::to_string(i) + ">: missing key");
                continue;
            }
            if (out->focus_index.count(key) != 0) {
                errors.push_back(f_entry + ":" + key + ": duplicate focus key");
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
                errors.push_back(f_entry + ":" + key + ": days must be a positive number");
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
            focus_src.push_back(f_entry);
        }
    }

    // ---- events ----------------------------------------------------------
    std::vector<std::string> event_src;
    {
        const SourceFile* sf = find_source(&files, "common/events.json");
        if (sf != nullptr) {
            const std::string f_events = sf->full;
            const Json& arr = sf->doc["events"];
            if (!arr.is_array()) {
                const std::string msg = f_events + ":events: expected an array";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& ej = arr[i];
                const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_events;
                const std::string key = ej["key"].as_string();
                if (key.empty()) {
                    errors.push_back(f_entry + ":<entry " + std::to_string(i) + ">: missing key");
                    continue;
                }
                if (out->event_index.count(key) != 0) {
                    errors.push_back(f_entry + ":" + key + ": duplicate event key");
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
                    errors.push_back(f_entry + ":" + key + ": event has no options");
                }
                out->event_index[key] = def.index;
                out->events.push_back(std::move(def));
                event_src.push_back(f_entry);
            }
        }
    }

    // ---- decisions -------------------------------------------------------
    std::vector<std::string> decision_src;
    {
        const SourceFile* sf = find_source(&files, "common/decisions.json");
        if (sf != nullptr) {
            const std::string f_decisions = sf->full;
            const Json& arr = sf->doc["decisions"];
            if (!arr.is_array()) {
                const std::string msg = f_decisions + ":decisions: expected an array";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& dj = arr[i];
                const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_decisions;
                const std::string key = dj["key"].as_string();
                if (key.empty()) {
                    errors.push_back(f_entry + ":<entry " + std::to_string(i) +
                                     ">: missing key");
                    continue;
                }
                if (out->decision_index.count(key) != 0) {
                    errors.push_back(f_entry + ":" + key + ": duplicate decision key");
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
                    errors.push_back(f_entry + ":" + key + ": cost_pp must not be negative");
                    def.cost_pp = 0.0;
                }
                if (def.days_remove < 0 || def.days_cooldown < 0) {
                    errors.push_back(f_entry + ":" + key +
                                     ": negative days_remove/days_cooldown");
                    def.days_remove = def.days_remove < 0 ? 0 : def.days_remove;
                    def.days_cooldown = def.days_cooldown < 0 ? 0 : def.days_cooldown;
                }
                out->decision_index[key] = def.index;
                out->decisions.push_back(std::move(def));
                decision_src.push_back(f_entry);
            }
        }
    }

    // ---- national spirits ------------------------------------------------
    // Optional file: a data set with no spirits simply ships none. Modifiers use the
    // same vocabulary as laws and technologies.
    std::vector<std::string> spirit_src;
    {
        const SourceFile* sf = find_source(&files, "common/spirits.json");
        if (sf != nullptr) {
            const std::string f_spirits = sf->full;
            const Json& arr = sf->doc["spirits"];
            if (!arr.is_array()) {
                const std::string msg = f_spirits + ":spirits: expected an array";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& sj = arr[i];
                const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_spirits;
                const std::string key = sj["key"].as_string();
                if (key.empty()) {
                    errors.push_back(f_entry + ":<entry " + std::to_string(i) +
                                     ">: missing key");
                    continue;
                }
                if (out->spirit_index.count(key) != 0) {
                    errors.push_back(f_entry + ":" + key + ": duplicate spirit key");
                    continue;
                }
                SpiritDef def;
                def.index = static_cast<uint32_t>(out->spirits.size());
                def.key = key;
                def.name = sj["name"].as_string(key);
                def.description = sj["description"].as_string();
                def.slots = static_cast<int>(sj["slots"].as_int(def.slots));
                if (def.slots < 1) {
                    errors.push_back(f_entry + ":" + key + ": slots must be at least 1");
                    def.slots = 1;
                }
                def.available = sj["available"];
                def.modifiers = parse_modifiers(sj["modifiers"], &errors, f_entry, key);
                def.effects = sj["effects"];
                out->spirit_index[key] = def.index;
                out->spirits.push_back(std::move(def));
                spirit_src.push_back(f_entry);
            }
        }
    }

    // ---- political advisors ----------------------------------------------
    std::vector<std::string> advisor_src;
    {
        const SourceFile* sf = find_source(&files, "common/advisors.json");
        if (sf != nullptr) {
            const std::string f_advisors = sf->full;
            const Json& arr = sf->doc["advisors"];
            if (!arr.is_array()) {
                const std::string msg = f_advisors + ":advisors: expected an array";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            for (size_t i = 0; i < arr.size(); ++i) {
                const Json& aj = arr[i];
                const std::string& f_entry = i < sf->origin.size() ? sf->origin[i] : f_advisors;
                const std::string key = aj["key"].as_string();
                if (key.empty()) {
                    errors.push_back(f_entry + ":<entry " + std::to_string(i) +
                                     ">: missing key");
                    continue;
                }
                if (out->advisor_index.count(key) != 0) {
                    errors.push_back(f_entry + ":" + key + ": duplicate advisor key");
                    continue;
                }
                AdvisorDef def;
                def.index = static_cast<uint32_t>(out->advisors.size());
                def.key = key;
                def.name = aj["name"].as_string(key);
                def.description = aj["description"].as_string();
                def.cost_pp = aj["cost_pp"].as_double(def.cost_pp);
                if (def.cost_pp < 0.0) {
                    errors.push_back(f_entry + ":" + key + ": cost_pp must not be negative");
                    def.cost_pp = 0.0;
                }
                def.available = aj["available"];
                def.modifiers = parse_modifiers(aj["modifiers"], &errors, f_entry, key);
                out->advisor_index[key] = def.index;
                out->advisors.push_back(std::move(def));
                advisor_src.push_back(f_entry);
            }
        }
    }

    // ---- designer components --------------------------------------------
    // Optional file: a data set with no designers ships none. A file that exists
    // but does not parse is a hard error, exactly like the focus trees. Components
    // load before technologies because a technology may gate a component by naming
    // its key in `unlock_equipment` (alongside equipment keys).
    //
    // A component's stat fields are additive deltas on the archetype it is fitted
    // into; `cost_multiplier` scales the archetype's build cost; `resources` are
    // added per unit; `reliability` is a delta the designer maths clamps into
    // [0.1, 1.0]. Only a negative *cost* is a data error (it would let a design
    // undercut its own archetype for free); negative stat deltas are legitimate
    // (a heavy gun slows the design, a heavy airframe trades agility for armour).
    const SourceFile* sf_components = find_source(&files, "common/components.json");
    if (sf_components != nullptr) {
        const Json& arr = sf_components->doc["components"];
        if (!arr.is_array()) {
            const std::string msg = sf_components->full + ":components: expected an array";
            errors.push_back(msg);
            if (err) *err = msg;
            return false;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const Json& cj = arr[i];
            const std::string& f_components =
                i < sf_components->origin.size() ? sf_components->origin[i] : sf_components->full;
            const std::string key = cj["key"].as_string();
            if (key.empty()) {
                errors.push_back(f_components + ":<entry " + std::to_string(i) +
                                 ">: missing key");
                continue;
            }
            if (out->component_index.count(key) != 0) {
                errors.push_back(f_components + ":" + key + ": duplicate component key");
                continue;
            }
            const std::string slot_name = cj["slot"].as_string();
            ComponentSlot slot = ComponentSlot::Special;
            if (slot_name.empty() || !component_slot_from_name(slot_name, &slot)) {
                errors.push_back(f_components + ":" + key + ": unknown slot " +
                                 (slot_name.empty() ? std::string("(missing)") : slot_name));
                continue;
            }
            const std::string category_name = cj["category"].as_string();
            const int cat = match_equipment_category(category_name);
            if (cat < 0) {
                errors.push_back(f_components + ":" + key + ": unknown category " +
                                 (category_name.empty() ? std::string("(missing)")
                                                        : category_name));
                continue;
            }
            ComponentDef def;
            def.index = static_cast<uint32_t>(out->components.size());
            def.key = key;
            def.name = cj["name"].as_string(key);
            def.slot = slot;
            def.category = static_cast<EquipmentCategory>(cat);
            def.year = static_cast<int>(cj["year"].as_int(def.year));
            def.soft_attack = cj["soft_attack"].as_double(def.soft_attack);
            def.hard_attack = cj["hard_attack"].as_double(def.hard_attack);
            def.air_attack = cj["air_attack"].as_double(def.air_attack);
            def.air_defence = cj["air_defence"].as_double(def.air_defence);
            def.ground_attack = cj["ground_attack"].as_double(def.ground_attack);
            def.agility = cj["agility"].as_double(def.agility);
            def.armor = cj["armor"].as_double(def.armor);
            def.piercing = cj["piercing"].as_double(def.piercing);
            def.defense = cj["defense"].as_double(def.defense);
            def.breakthrough = cj["breakthrough"].as_double(def.breakthrough);
            def.hardness = cj["hardness"].as_double(def.hardness);
            def.max_strength = cj["max_strength"].as_double(def.max_strength);
            def.organization = cj["organization"].as_double(def.organization);
            def.speed = cj["speed"].as_double(def.speed);
            def.reliability = cj["reliability"].as_double(def.reliability);
            def.range = cj["range"].as_double(def.range);
            def.detection = cj["detection"].as_double(def.detection);
            def.sub_detection = cj["sub_detection"].as_double(def.sub_detection);
            def.naval_attack = cj["naval_attack"].as_double(def.naval_attack);
            def.torpedo_attack = cj["torpedo_attack"].as_double(def.torpedo_attack);
            def.visibility = cj["visibility"].as_double(def.visibility);
            def.build_cost_add = cj["build_cost_add"].as_double(def.build_cost_add);
            def.cost_multiplier = cj["cost_multiplier"].as_double(def.cost_multiplier);
            def.fuel_use = cj["fuel_use"].as_double(def.fuel_use);
            def.supply_use = cj["supply_use"].as_double(def.supply_use);
            def.manpower = cj["manpower"].as_double(def.manpower);
            parse_resource_costs(cj["resources"], def.resources, &errors, f_components, key);
            def.available = cj["available"];

            bool invalid_cost = false;
            if (def.build_cost_add < 0.0) {
                errors.push_back(f_components + ":" + key + ": negative build_cost_add");
                invalid_cost = true;
            }
            if (def.cost_multiplier <= 0.0) {
                errors.push_back(f_components + ":" + key +
                                 ": cost_multiplier must be greater than zero");
                invalid_cost = true;
            }
            if (def.fuel_use < 0.0) {
                errors.push_back(f_components + ":" + key + ": negative fuel_use");
                invalid_cost = true;
            }
            if (def.supply_use < 0.0) {
                errors.push_back(f_components + ":" + key + ": negative supply_use");
                invalid_cost = true;
            }
            if (def.manpower < 0.0) {
                errors.push_back(f_components + ":" + key + ": negative manpower");
                invalid_cost = true;
            }
            for (int r = 0; r < RESOURCE_COUNT; ++r) {
                if (def.resources[r] < 0.0) {
                    errors.push_back(f_components + ":" + key + ": negative " +
                                     resource_name(static_cast<Resource>(r)) + " cost");
                    invalid_cost = true;
                }
            }
            if (invalid_cost) continue;

            // A malformed `available` trigger is a data error: report it and skip the
            // entry so invalid content never reaches the designer. Components are
            // country-scoped, so the trigger takes no state scope.
            std::vector<std::string> trigger_errors;
            const std::set<std::string> no_states;
            validate_trigger(def.available, *out, no_states, &trigger_errors, f_components, key,
                             "available");
            if (!trigger_errors.empty()) {
                errors.insert(errors.end(), trigger_errors.begin(), trigger_errors.end());
                continue;
            }

            out->component_index[key] = def.index;
            out->components.push_back(std::move(def));
        }
    }

    // ---- intelligence ----------------------------------------------------
    // Optional file: operations a country may run against another, and the agency
    // upgrades its service can buy. An absent file ships empty tables; a file that
    // does not parse is already a hard error in load_base_source, and a present file
    // whose two entry lists are not arrays is a hard error too. An entry with a
    // structural error is reported and skipped, so no invalid definition reaches the
    // simulation. Triggers and effects go through the same country-scope script pass
    // as spirits and advisors.
    {
        const SourceFile* sf = find_source(&files, "common/intelligence.json");
        if (sf != nullptr) {
            const std::string f_intel = sf->full;
            const Json& doc = sf->doc;
            if (!doc.is_object()) {
                const std::string msg =
                    f_intel + ":intelligence: content file must be an object";
                errors.push_back(msg);
                if (err) *err = msg;
                return false;
            }
            const Json& jops = doc["operations"];
            const Json& jupgrades = doc["agency_upgrades"];
            const std::pair<const char*, const Json*> members[] = {
                {"operations", &jops}, {"agency_upgrades", &jupgrades}};
            for (const auto& member : members) {
                if (!member.second->is_null() && !member.second->is_array()) {
                    const std::string msg =
                        f_intel + ":" + member.first + ": expected an array";
                    errors.push_back(msg);
                    if (err) *err = msg;
                    return false;
                }
            }
            const std::set<std::string> no_states;  // intelligence is country-scoped

            // -- operations -----
            if (jops.is_array()) {
                for (size_t i = 0; i < jops.size(); ++i) {
                    const Json& o = jops[i];
                    const std::string& f_entry =
                        i < sf->origin.size() ? sf->origin[i] : f_intel;
                    if (!o.is_object()) {
                        errors.push_back(f_entry + ":<entry " + std::to_string(i) +
                                         ">: entry must be an object");
                        continue;
                    }
                    const std::string key = o["key"].as_string();
                    if (key.empty()) {
                        errors.push_back(f_entry + ":<entry " + std::to_string(i) +
                                         ">: missing key");
                        continue;
                    }
                    if (out->operation_index.count(key) != 0) {
                        errors.push_back(f_entry + ":" + key + ": duplicate operation key");
                        continue;
                    }
                    const std::string kind_name = o["kind"].as_string();
                    OperationKind kind = OperationKind::BuildNetwork;
                    if (kind_name.empty()) {
                        errors.push_back(f_entry + ":" + key + ": missing kind");
                        continue;
                    }
                    if (!match_operation_kind(kind_name, &kind)) {
                        errors.push_back(f_entry + ":" + key + ": unknown operation kind " +
                                         kind_name);
                        continue;
                    }
                    OperationDef def;
                    def.key = key;
                    def.name = o["name"].as_string(key);
                    def.description = o["description"].as_string();
                    def.kind = kind;
                    def.days = static_cast<int>(o["days"].as_int(def.days));
                    def.pp_cost = o["pp_cost"].as_double(def.pp_cost);
                    def.civilian_cost = o["civilian_cost"].as_double(def.civilian_cost);
                    def.network_required =
                        o["network_required"].as_double(def.network_required);
                    def.risk = o["risk"].as_double(def.risk);
                    def.network_gain = o["network_gain"].as_double(def.network_gain);
                    def.research_days = o["research_days"].as_double(def.research_days);
                    def.output_penalty = o["output_penalty"].as_double(def.output_penalty);
                    def.stability_delta = o["stability_delta"].as_double(def.stability_delta);
                    def.ideology_shift = o["ideology_shift"].as_double(def.ideology_shift);
                    def.effect_days = static_cast<int>(o["effect_days"].as_int(def.effect_days));
                    def.available = o["available"];
                    def.effect = o["effect"];

                    bool invalid = false;
                    auto bad = [&](const std::string& reason) {
                        errors.push_back(f_entry + ":" + key + ": " + reason);
                        invalid = true;
                    };
                    // Effect fields fall into two kinds. Magnitudes (network_gain,
                    // research_days, output_penalty, effect_days) must not be negative:
                    // output_penalty is a *penalty* the engine applies as -penalty to
                    // the target's output. Signed deltas (stability_delta,
                    // ideology_shift) carry their meaning in the sign: a positive
                    // stability_delta helps the target, a negative one destabilises it.
                    if (def.days <= 0) bad("days must be a positive number");
                    if (def.pp_cost < 0.0) bad("negative pp_cost");
                    if (def.civilian_cost < 0.0) bad("negative civilian_cost");
                    if (def.network_required < 0.0 || def.network_required > 100.0) {
                        bad("network_required must be between 0 and 100");
                    }
                    if (def.risk < 0.0 || def.risk > 1.0) bad("risk must be between 0 and 1");
                    if (def.network_gain < 0.0) bad("negative network_gain");
                    if (def.research_days < 0.0) bad("negative research_days");
                    if (def.output_penalty < 0.0) bad("negative output_penalty");
                    if (def.effect_days <= 0) bad("effect_days must be a positive number");
                    std::vector<std::string> script_errors;
                    validate_trigger(def.available, *out, no_states, &script_errors, f_entry,
                                     key, "available");
                    validate_effects(def.effect, *out, no_states, &script_errors, f_entry, key,
                                     "effect");
                    if (!script_errors.empty()) {
                        errors.insert(errors.end(), script_errors.begin(), script_errors.end());
                        invalid = true;
                    }
                    if (invalid) continue;
                    def.index = static_cast<uint32_t>(out->operations.size());
                    out->operation_index[key] = def.index;
                    out->operations.push_back(std::move(def));
                }
            }

            // -- agency upgrades -----
            // Two passes: every upgrade's key must exist before requires_upgrades can
            // resolve, and the requires graph must be an acyclic forest within the
            // file. A rejected entry is dropped whole, so no partial state remains.
            if (jupgrades.is_array()) {
                std::vector<AgencyUpgradeDef> cand(jupgrades.size());
                std::vector<std::string> cand_src(jupgrades.size(), f_intel);
                std::vector<bool> ok(jupgrades.size(), true);
                std::set<std::string> keys_seen;
                for (size_t i = 0; i < jupgrades.size(); ++i) {
                    const Json& u = jupgrades[i];
                    if (i < sf->origin2.size()) cand_src[i] = sf->origin2[i];
                    const std::string& src = cand_src[i];
                    AgencyUpgradeDef& def = cand[i];
                    if (!u.is_object()) {
                        errors.push_back(src + ":<entry " + std::to_string(i) +
                                         ">: entry must be an object");
                        ok[i] = false;
                        continue;
                    }
                    const std::string key = u["key"].as_string();
                    if (key.empty()) {
                        errors.push_back(src + ":<entry " + std::to_string(i) +
                                         ">: missing key");
                        ok[i] = false;
                        continue;
                    }
                    if (!keys_seen.insert(key).second) {
                        errors.push_back(src + ":" + key + ": duplicate agency upgrade key");
                        ok[i] = false;
                        continue;
                    }
                    def.key = key;
                    def.name = u["name"].as_string(key);
                    def.description = u["description"].as_string();
                    def.year = static_cast<int>(u["year"].as_int(def.year));
                    def.pp_cost = u["pp_cost"].as_double(def.pp_cost);
                    def.network_growth = u["network_growth"].as_double(def.network_growth);
                    def.operation_speed = u["operation_speed"].as_double(def.operation_speed);
                    def.crypto_speed = u["crypto_speed"].as_double(def.crypto_speed);
                    def.counter_intel = u["counter_intel"].as_double(def.counter_intel);
                    def.available = u["available"];
                    auto bad = [&](const std::string& reason) {
                        errors.push_back(src + ":" + key + ": " + reason);
                        ok[i] = false;
                    };
                    if (def.year < 1936) bad("year must be 1936 or later");
                    if (def.pp_cost < 0.0) bad("negative pp_cost");
                    if (def.network_growth < 0.0) bad("negative network_growth");
                    if (def.operation_speed < 0.0) bad("negative operation_speed");
                    if (def.crypto_speed < 0.0) bad("negative crypto_speed");
                    if (def.counter_intel < 0.0 || def.counter_intel > 1.0) {
                        bad("counter_intel must be between 0 and 1");
                    }
                    const Json& req = u["requires_upgrades"];
                    if (!req.is_null()) {
                        if (!req.is_array()) {
                            bad("requires_upgrades must be an array of upgrade keys");
                        } else {
                            for (size_t k = 0; k < req.size(); ++k) {
                                if (!req[k].is_string() || req[k].as_string().empty()) {
                                    bad("requires_upgrades[" + std::to_string(k) +
                                        "] must be a non-empty upgrade key");
                                } else {
                                    def.requires_upgrades.push_back(req[k].as_string());
                                }
                            }
                        }
                    }
                    std::vector<std::string> script_errors;
                    validate_trigger(def.available, *out, no_states, &script_errors, src, key,
                                     "available");
                    if (!script_errors.empty()) {
                        errors.insert(errors.end(), script_errors.begin(), script_errors.end());
                        ok[i] = false;
                    }
                }

                // Keys that parsed, so requires_upgrades can resolve in this file.
                std::map<std::string, size_t> by_key;
                for (size_t i = 0; i < cand.size(); ++i) {
                    if (ok[i] && !cand[i].key.empty()) by_key[cand[i].key] = i;
                }
                for (size_t i = 0; i < cand.size(); ++i) {
                    if (!ok[i]) continue;
                    for (const std::string& r : cand[i].requires_upgrades) {
                        if (by_key.count(r) == 0) {
                            errors.push_back(cand_src[i] + ":" + cand[i].key +
                                             ": unknown requires_upgrades " + r);
                            ok[i] = false;
                            break;
                        }
                    }
                }
                // Resolved prerequisite indices, ascending for determinism.
                std::vector<std::vector<size_t>> prereq_idx(cand.size());
                for (size_t i = 0; i < cand.size(); ++i) {
                    if (!ok[i]) continue;
                    for (const std::string& r : cand[i].requires_upgrades) {
                        prereq_idx[i].push_back(by_key.at(r));
                    }
                    std::sort(prereq_idx[i].begin(), prereq_idx[i].end());
                    prereq_idx[i].erase(
                        std::unique(prereq_idx[i].begin(), prereq_idx[i].end()),
                        prereq_idx[i].end());
                }
                // Cycle detection: a grey node reached again closes a cycle. Every
                // member of the cycle is named and dropped.
                std::vector<int> color(cand.size(), 0);  // 0 unvisited, 1 on stack, 2 done
                std::vector<size_t> parent(cand.size(), static_cast<size_t>(-1));
                for (size_t s = 0; s < cand.size(); ++s) {
                    if (!ok[s] || color[s] != 0) continue;
                    std::vector<size_t> stack{s};
                    color[s] = 1;
                    while (!stack.empty()) {
                        const size_t u = stack.back();
                        bool advanced = false;
                        for (size_t p : prereq_idx[u]) {
                            if (!ok[p]) continue;
                            if (color[p] == 0) {
                                color[p] = 1;
                                parent[p] = u;
                                stack.push_back(p);
                                advanced = true;
                                break;
                            }
                            if (color[p] == 1) {
                                std::vector<std::string> names;
                                size_t x = u;
                                while (true) {
                                    ok[x] = false;
                                    names.push_back(cand[x].key);
                                    if (x == p || parent[x] == static_cast<size_t>(-1)) break;
                                    x = parent[x];
                                }
                                std::sort(names.begin(), names.end());
                                std::string list;
                                for (size_t n = 0; n < names.size(); ++n) {
                                    if (n != 0) list += ", ";
                                    list += names[n];
                                }
                                errors.push_back(cand_src[u] + ":" + cand[u].key +
                                                 ": requires_upgrades cycle among " + list);
                                advanced = true;
                                break;
                            }
                        }
                        if (!advanced) {
                            color[u] = 2;
                            stack.pop_back();
                        }
                    }
                }
                // Anything that required a dropped upgrade can never be bought.
                bool changed = true;
                while (changed) {
                    changed = false;
                    for (size_t i = 0; i < cand.size(); ++i) {
                        if (!ok[i]) continue;
                        for (size_t p : prereq_idx[i]) {
                            if (ok[p]) continue;
                            errors.push_back(cand_src[i] + ":" + cand[i].key +
                                             ": requires_upgrades " + cand[p].key +
                                             " is unavailable");
                            ok[i] = false;
                            changed = true;
                            break;
                        }
                    }
                }
                for (size_t i = 0; i < cand.size(); ++i) {
                    if (!ok[i]) continue;
                    cand[i].index = static_cast<uint32_t>(out->agency_upgrades.size());
                    out->agency_upgrade_index[cand[i].key] = cand[i].index;
                    out->agency_upgrades.push_back(std::move(cand[i]));
                }
            }
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
    // Technologies may gate an equipment model or a designer component by key; both
    // tables are complete here, so the reference is resolved now.
    for (size_t i = 0; i < out->techs.size(); ++i) {
        const TechDef& t = out->techs[i];
        const std::string file = i < tech_src.size() ? tech_src[i] : std::string();
        for (const std::string& ek : t.unlock_equipment) {
            if (out->equipment_by_key.count(ek) == 0 && out->component_index.count(ek) == 0) {
                errors.push_back(file + ":" + t.key +
                                 ": unlock_equipment references unknown equipment or component " +
                                 ek);
            }
        }
    }

    if (err) *err = errors.empty() ? std::string() : errors.front();
    return true;
}

}  // namespace hoi
