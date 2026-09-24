// The scripting engine: one recursive evaluator over `Json` shared by focus trees,
// events and decisions (spec sections 57, 58, 59, 91).
//
// Design rules:
//  * `eval_trigger` is pure except for the `chance` trigger, which draws from
//    RngStream::Events by design; nothing else in a trigger mutates state.
//  * `apply_effects` handles every effect atomically. An effect either resolves all
//    of its inputs and applies once, or it is reported and skipped - it never
//    leaves half of itself behind, and it never throws.
//  * Comparison values are objects: {"gte": 50}. A bare number is accepted as `eq`
//    for the common case; a string where a number is expected is a content error.
//  * Boolean combinators are `all`/`any` (arrays) and `not` (object, or array which
//    is ANDed then negated). A bare `true`/`false` is a valid trigger.
//  * Unknown keys are content errors: a trigger key evaluates false, an effect key
//    is reported once through the game log and ignored. Neither is ever fatal, and
//    neither mutates state.
//
// Nothing here knows a country tag, a focus key or a technology key: every lookup
// goes through Content or World.

#include "sim/script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/rng.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/diplomacy.h"
#include "sim/events.h"
#include "sim/focus.h"
#include "sim/industry.h"
#include "sim/politics.h"
#include "sim/research.h"
#include "sim/spirits.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {
namespace {

// Maximum nesting for `all`/`any`/`not` and for effect-driver recursion
// (complete_focus -> effects -> complete_focus ...). Content that loops is a data
// bug; the guard keeps it a reported no-op instead of a stack overflow.
constexpr int kMaxScriptDepth = 32;
thread_local int g_effect_depth = 0;

// Case- and separator-insensitive match against the engine enum names, exactly as
// content.cpp matches modifier names ("DivisionAttack", "division_attack" and
// "division attack" all resolve to the same ModifierKind).
std::string normalize_name(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

int match_modifier_kind(const std::string& key) {
    const std::string want = normalize_name(key);
    if (want.empty()) return -1;
    for (int i = 0; i < static_cast<int>(ModifierKind::Count); ++i) {
        if (normalize_name(modifier_kind_name(static_cast<ModifierKind>(i))) == want) return i;
    }
    return -1;
}

std::string describe_key(const std::string& key, const std::string& reason) {
    return describe_script_error(key, reason);
}

// Reports a content problem once per distinct message. Logging is not part of the
// deterministic simulation, so the dedupe set is deliberately process-wide.
void report_problem(const std::string& key, const std::string& reason) {
    static std::set<std::string> reported;
    const std::string message = describe_key(key, reason);
    if (reported.insert(message).second) log_write(LogLevel::Warn, message);
}

void report_unknown(const std::string& kind, const std::string& key) {
    report_problem(key, "unknown " + kind + " key");
}

// A finite JSON number, rejecting strings, bools and NaN/Inf.
bool finite_number(const Json& v, double* out) {
    if (!v.is_number()) return false;
    const double d = v.as_double();
    if (!std::isfinite(d)) return false;
    *out = d;
    return true;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

CountryId find_country(const Game& g, const std::string& tag) {
    CountryId found;
    g.world.countries.for_each([&](CountryId id, const Country& c) {
        if (!found.valid() && c.alive && c.tag == tag) found = id;
    });
    return found;
}

// A state key resolves in this order:
//   1. an exact State::name match (ascending id, first hit);
//   2. "s<digits>" or bare "<digits>" -> StateId(digits - 1), the map key/creation
//      order convention used by the scenario map file.
// World has no state-key index yet, so a display name that differs from the map key
// is only reachable through the numeric form. (See the wiring note in the report.)
StateId find_state(const Game& g, const std::string& key) {
    if (key.empty()) return StateId{};
    const World& w = g.world;
    StateId found;
    w.states.for_each([&](StateId id, const State& s) {
        if (!found.valid() && s.name == key) found = id;
    });
    if (found.valid()) return found;

    const char* p = key.c_str();
    if (*p == 's' || *p == 'S') ++p;
    if (*p == '\0') return StateId{};
    long long n = 0;
    for (; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return StateId{};
        n = n * 10 + (*p - '0');
        if (n > 1000000000LL) return StateId{};
    }
    if (n < 1) return StateId{};
    const StateId candidate(static_cast<uint32_t>(n - 1));
    return w.states.alive(candidate) ? candidate : StateId{};
}

// Parses "YYYY-MM-DD"; returns false on anything else.
bool parse_script_date(const std::string& s, GameDate* out) {
    if (s.size() < 10) return false;
    int vals[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos == start) return false;
        long v = 0;
        for (size_t k = start; k < pos; ++k) v = v * 10 + (s[k] - '0');
        vals[i] = static_cast<int>(v);
        if (i < 2 && (pos >= s.size() || s[pos] != '-')) return false;
        ++pos;
    }
    if (vals[0] < 1 || vals[1] < 1 || vals[1] > 12 || vals[2] < 1 || vals[2] > 31) return false;
    out->year = vals[0];
    out->month = static_cast<uint8_t>(vals[1]);
    out->day = static_cast<uint8_t>(vals[2]);
    out->hour = 0;
    return true;
}

bool has_completed_focus(const Country& c, uint32_t focus) {
    return std::find(c.completed_focuses.begin(), c.completed_focuses.end(), focus) !=
           c.completed_focuses.end();
}

bool has_active_decision(const Country& c, uint32_t decision) {
    return std::find(c.active_decisions.begin(), c.active_decisions.end(), decision) !=
           c.active_decisions.end();
}

bool has_country_flag(const Country& c, const std::string& flag) {
    return std::find(c.country_flags.begin(), c.country_flags.end(), flag) != c.country_flags.end();
}

bool has_state_flag(const State& s, const std::string& flag) {
    return std::find(s.flags.begin(), s.flags.end(), flag) != s.flags.end();
}

int64_t country_year(const Game& g) { return g.world.date.year; }

double country_factories(const Game& g, CountryId country) {
    int civ = 0, mil = 0, dock = 0;
    count_factories(g.world, country, &civ, &mil, &dock);
    return static_cast<double>(civ + mil + dock);
}

double country_divisions(const Game& g, Country& c) {
    double n = 0.0;
    for (DivisionId did : c.divisions) {
        if (g.world.divisions.alive(did)) n += 1.0;
    }
    return n;
}

// ------------------------------------------------------------ comparators -----

// Evaluates a comparator against `actual`. `skip_a`/`skip_b` name keys the caller
// already consumed (e.g. `target` in an opinion object). Sets *ok = false for a
// malformed comparator or a non-numeric bound.
bool eval_comparator(double actual, const Json& cmp, const char* skip_a, const char* skip_b,
                     bool* ok) {
    if (cmp.is_number()) {
        double want = 0.0;
        if (!finite_number(cmp, &want)) {
            *ok = false;
            return false;
        }
        *ok = true;
        return actual == want;
    }
    if (!cmp.is_object()) {
        *ok = false;
        return false;
    }
    bool any = false;
    bool result = true;
    for (const auto& item : cmp.object_items()) {
        const std::string& k = item.first;
        if ((skip_a != nullptr && k == skip_a) || (skip_b != nullptr && k == skip_b)) continue;
        double want = 0.0;
        if (!finite_number(item.second, &want)) {
            *ok = false;
            return false;
        }
        bool one = false;
        if (k == "gte") one = actual >= want;
        else if (k == "gt") one = actual > want;
        else if (k == "lte") one = actual <= want;
        else if (k == "lt") one = actual < want;
        else if (k == "eq") one = std::fabs(actual - want) <= 1e-9;
        else {
            *ok = false;
            return false;
        }
        any = true;
        if (!one) result = false;
    }
    if (!any) {
        *ok = false;
        return false;
    }
    *ok = true;
    return result;
}

// Evaluates a comparator that follows `key` in a trigger; reports malformed values.
bool comparator_trigger(const ScriptScope& sc, const std::string& key, const Json& value,
                        double actual) {
    bool ok = false;
    const bool result = eval_comparator(actual, value, nullptr, nullptr, &ok);
    if (!ok) {
        report_problem(key, "expected a comparator object like {\"gte\": 50}");
        return false;
    }
    (void)sc;
    return result;
}

// -------------------------------------------------------------- triggers ------

bool eval_trigger_internal(const ScriptScope& sc, const Json& t, int depth);

// ANDs an array of triggers (a single trigger object is accepted as a one-element
// array, which is what `not` needs).
bool eval_all_of(const ScriptScope& sc, const Json& value, int depth) {
    if (value.is_array()) {
        bool result = true;
        for (const Json& item : value.array_items()) {
            if (!eval_trigger_internal(sc, item, depth + 1)) result = false;
        }
        return result;
    }
    return eval_trigger_internal(sc, value, depth + 1);
}

bool eval_any_of(const ScriptScope& sc, const Json& value, int depth) {
    if (!value.is_array()) {
        report_problem("any", "expected an array of triggers");
        return false;
    }
    for (const Json& item : value.array_items()) {
        if (eval_trigger_internal(sc, item, depth + 1)) return true;
    }
    return false;
}

// True when `value` is a bool and equals `actual`; anything else is a content error.
bool bool_trigger(const std::string& key, const Json& value, bool actual) {
    if (!value.is_bool()) {
        report_problem(key, "expected true or false");
        return false;
    }
    return value.as_bool() == actual;
}

bool eval_country_number(const ScriptScope& sc, const std::string& key, const Json& value,
                         double actual) {
    return comparator_trigger(sc, key, value, actual);
}

bool eval_trigger_key(const ScriptScope& sc, const std::string& key, const Json& value, int depth) {
    Game* g = sc.game;
    const Country* c = g != nullptr ? g->world.country(sc.country) : nullptr;

    // Combinators and randomness first: they do not need a country.
    if (key == "all") return eval_all_of(sc, value, depth);
    if (key == "any") return eval_any_of(sc, value, depth);
    if (key == "not") return !eval_all_of(sc, value, depth);
    if (key == "chance") {
        double p = 0.0;
        if (g == nullptr || !finite_number(value, &p)) {
            report_problem(key, "expected a number between 0 and 1");
            return false;
        }
        return eval_random_chance(*g, p);
    }
    if (key == "date_after" || key == "date_before") {
        if (g == nullptr || !value.is_string()) {
            report_problem(key, "expected a \"YYYY-MM-DD\" string");
            return false;
        }
        GameDate want;
        if (!parse_script_date(value.as_string(), &want)) {
            report_problem(key, "expected a \"YYYY-MM-DD\" string");
            return false;
        }
        const int64_t now = date_to_days(g->world.date);
        const int64_t then = date_to_days(want);
        return key == "date_after" ? now >= then : now <= then;
    }

    if (c == nullptr || g == nullptr) {
        report_problem(key, "trigger needs a valid country scope");
        return false;
    }

    if (key == "political_power") return eval_country_number(sc, key, value, c->political_power);
    if (key == "stability") return eval_country_number(sc, key, value, c->stability);
    if (key == "war_support") return eval_country_number(sc, key, value, c->war_support);
    if (key == "manpower") return eval_country_number(sc, key, value, c->manpower);
    if (key == "fuel") return eval_country_number(sc, key, value, c->fuel);
    if (key == "factories") return eval_country_number(sc, key, value, country_factories(*g, sc.country));
    if (key == "num_divisions") {
        return eval_country_number(sc, key, value, country_divisions(*g, *g->world.country(sc.country)));
    }
    if (key == "year") return eval_country_number(sc, key, value, static_cast<double>(country_year(*g)));

    if (key == "is_ai") return bool_trigger(key, value, g->is_ai(sc.country));
    if (key == "at_war") return bool_trigger(key, value, c->at_war);

    if (key == "ideology") {
        if (!value.is_string()) {
            report_problem(key, "expected an ideology name string");
            return false;
        }
        return normalize_name(ideology_name(c->ideology)) == normalize_name(value.as_string());
    }
    if (key == "at_war_with") {
        if (!value.is_string()) {
            report_problem(key, "expected a country tag string");
            return false;
        }
        const CountryId other = find_country(*g, value.as_string());
        if (!other.valid()) {
            report_problem(key, "unknown country tag '" + value.as_string() + "'");
            return false;
        }
        return g->world.at_war(sc.country, other);
    }
    if (key == "has_tech") {
        if (!value.is_string()) {
            report_problem(key, "expected a technology key string");
            return false;
        }
        const TechId tech = g->content.tech_id(value.as_string());
        return tech.valid() && c->research.has_tech(tech);
    }
    if (key == "completed_focus") {
        if (!value.is_string()) {
            report_problem(key, "expected a focus key string");
            return false;
        }
        const uint32_t focus = g->content.focus_id(value.as_string());
        return focus != INVALID_FOCUS && has_completed_focus(*c, focus);
    }
    if (key == "has_flag" || key == "has_country_flag") {
        if (!value.is_string()) {
            report_problem(key, "expected a flag name string");
            return false;
        }
        return has_country_flag(*c, value.as_string());
    }
    if (key == "has_decision") {
        if (!value.is_string()) {
            report_problem(key, "expected a decision key string");
            return false;
        }
        const uint32_t decision = g->content.decision_id(value.as_string());
        return decision != INVALID_ID && has_active_decision(*c, decision);
    }
    if (key == "owns_state" || key == "controls_state") {
        if (!value.is_string()) {
            report_problem(key, "expected a state key string");
            return false;
        }
        const StateId state = find_state(*g, value.as_string());
        const State* st = state.valid() ? g->world.state(state) : nullptr;
        if (st == nullptr) {
            report_problem(key, "unknown state key '" + value.as_string() + "'");
            return false;
        }
        return key == "owns_state" ? st->owner == sc.country : st->controller == sc.country;
    }
    if (key == "opinion") {
        if (!value.is_object()) {
            report_problem(key, "expected {\"target\": \"TAG\", \"gte\": 25}");
            return false;
        }
        CountryId other = sc.target_country;
        const Json& target = value.at("target");
        if (target.is_string()) {
            other = find_country(*g, target.as_string());
        } else if (!target.is_null()) {
            report_problem(key, "target must be a country tag string");
            return false;
        }
        if (!other.valid()) {
            report_problem(key, "no opinion target (set \"target\" or a country scope)");
            return false;
        }
        const Relation* rel = g->world.find_relation(sc.country, other);
        const double actual = rel != nullptr ? rel->value : 0.0;
        bool ok = false;
        const bool result = eval_comparator(actual, value, "target", nullptr, &ok);
        if (!ok) report_problem(key, "expected a comparator with a target");
        return result;
    }
    if (key == "var") {
        if (!value.is_object()) {
            report_problem(key, "expected {\"name\": \"x\", \"gte\": 1}");
            return false;
        }
        const Json& name = value.at("name");
        if (!name.is_string() || name.as_string().empty()) {
            report_problem(key, "expected a non-empty variable name");
            return false;
        }
        double actual = 0.0;
        const auto it = g->world.script_vars.find(name.as_string());
        if (it != g->world.script_vars.end()) actual = it->second;
        bool ok = false;
        const bool result = eval_comparator(actual, value, "name", nullptr, &ok);
        if (!ok) report_problem(key, "expected a comparator with a name");
        return result;
    }

    // State-scoped triggers.
    if (key == "state_controller_is_owner" || key == "state_factories" || key == "state_has_flag") {
        const State* st = sc.state.valid() ? g->world.state(sc.state) : nullptr;
        if (st == nullptr) {
            report_problem(key, "trigger needs a state scope");
            return false;
        }
        if (key == "state_controller_is_owner") {
            return bool_trigger(key, value, st->controller.valid() && st->controller == st->owner);
        }
        if (key == "state_factories") {
            return eval_country_number(sc, key, value, static_cast<double>(st->total_factories()));
        }
        if (!value.is_string()) {
            report_problem(key, "expected a flag name string");
            return false;
        }
        return has_state_flag(*st, value.as_string());
    }

    report_unknown("trigger", key);
    return false;
}

bool eval_trigger_internal(const ScriptScope& sc, const Json& t, int depth) {
    if (t.is_null()) return true;
    if (depth > kMaxScriptDepth) {
        report_problem("trigger", "nesting deeper than the script engine allows");
        return false;
    }
    if (!t.is_object()) {
        report_problem("trigger", "trigger must be a JSON object");
        return false;
    }
    // Every key of a multi-key trigger object must hold (AND). Evaluation does not
    // short-circuit so one pass reports every bad key.
    bool result = true;
    for (const auto& item : t.object_items()) {
        if (!eval_trigger_key(sc, item.first, item.second, depth)) result = false;
    }
    return result;
}

// --------------------------------------------------------------- effects ------

void apply_effect_key(const ScriptScope& sc, const std::string& key, const Json& value);

// Resolves the country the effect applies to; reports and returns nullptr when the
// scope has none.
Country* effect_country(const ScriptScope& sc, const std::string& key) {
    if (sc.game == nullptr) {
        report_problem(key, "effect needs a game");
        return nullptr;
    }
    Country* c = sc.game->world.country(sc.country);
    if (c == nullptr || !c->alive) {
        report_problem(key, "effect needs a valid, alive country scope");
        return nullptr;
    }
    return c;
}

void apply_add_political_power(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    double v = 0.0;
    if (c == nullptr) return;
    if (!finite_number(value, &v)) {
        report_problem(key, "expected a number");
        return;
    }
    c->political_power = std::max(0.0, c->political_power + v);
}

void apply_stability(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    double v = 0.0;
    if (c == nullptr) return;
    if (!finite_number(value, &v)) {
        report_problem(key, "expected a number");
        return;
    }
    c->stability = clamp01(c->stability + v);
}

void apply_war_support(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    double v = 0.0;
    if (c == nullptr) return;
    if (!finite_number(value, &v)) {
        report_problem(key, "expected a number");
        return;
    }
    c->war_support = clamp01(c->war_support + v);
}

void apply_add_manpower(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    double v = 0.0;
    if (c == nullptr) return;
    if (!finite_number(value, &v)) {
        report_problem(key, "expected a number");
        return;
    }
    c->manpower = std::max(0.0, c->manpower + v);
}

void apply_add_fuel(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    double v = 0.0;
    if (c == nullptr) return;
    if (!finite_number(value, &v)) {
        report_problem(key, "expected a number");
        return;
    }
    c->fuel = std::max(0.0, c->fuel + v);
}

void apply_add_opinion(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    if (!value.is_object()) {
        report_problem(key, "expected {\"target\": \"TAG\", \"value\": 10}");
        return;
    }
    CountryId other = sc.target_country;
    const Json& target = value.at("target");
    if (target.is_string()) {
        other = find_country(*g, target.as_string());
    } else if (!target.is_null()) {
        report_problem(key, "target must be a country tag string");
        return;
    }
    double delta = 0.0;
    if (!finite_number(value.at("value"), &delta)) {
        report_problem(key, "expected a numeric value");
        return;
    }
    if (!other.valid() || other == sc.country) {
        report_problem(key, "no valid opinion target");
        return;
    }
    Relation& rel = g->world.relation(sc.country, other);
    rel.value = std::max(-100.0, std::min(100.0, rel.value + delta));
}

void apply_declare_war(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    Country* c = effect_country(sc, key);
    if (c == nullptr || !value.is_object()) {
        report_problem(key, "expected {\"target\": \"TAG\", \"annex\": false, \"puppet\": false}");
        return;
    }
    const Json& target = value.at("target");
    if (!target.is_string()) {
        report_problem(key, "target must be a country tag string");
        return;
    }
    const CountryId other = find_country(*g, target.as_string());
    const Country* oc = other.valid() ? g->world.country(other) : nullptr;
    if (oc == nullptr || !oc->alive || other == sc.country) {
        report_problem(key, "invalid war target '" + target.as_string() + "'");
        return;
    }
    WarGoal goal;
    goal.claimant = sc.country;
    goal.target = other;
    goal.state = sc.state.valid() ? sc.state : oc->capital;
    goal.annex_country = value.at("annex").as_bool(false);
    goal.puppet = value.at("puppet").as_bool(false);
    std::vector<WarGoal> goals;
    goals.push_back(goal);
    declare_war(*g, sc.country, other, goals);
}

void apply_add_tech(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a technology key string");
        return;
    }
    const TechId tech = g->content.tech_id(value.as_string());
    if (!tech.valid() || g->content.tech_def(tech) == nullptr) {
        report_problem(key, "unknown technology '" + value.as_string() + "'");
        return;
    }
    if (c->research.has_tech(tech)) return;  // atomic no-op: already known
    // apply_tech_effects marks the technology completed and propagates its
    // modifiers and unlocks; it is idempotent, so it owns both halves.
    apply_tech_effects(*g, *c, tech);
}

void apply_set_law(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a law key string");
        return;
    }
    const LawDef* law = g->content.law(value.as_string());
    if (law == nullptr) {
        report_problem(key, "unknown law '" + value.as_string() + "'");
        return;
    }
    apply_law_change(*g, *c, law->kind, law->level);
}

void apply_add_modifier(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_object()) {
        report_problem(key, "expected {\"kind\": \"DivisionAttack\", \"value\": 0.05, \"days\": 90}");
        return;
    }
    const Json& kind_json = value.at("kind");
    if (!kind_json.is_string()) {
        report_problem(key, "kind must be a modifier kind name");
        return;
    }
    const int kind = match_modifier_kind(kind_json.as_string());
    if (kind < 0) {
        report_problem(key, "unknown modifier kind '" + kind_json.as_string() + "'");
        return;
    }
    double amount = 0.0;
    if (!finite_number(value.at("value"), &amount)) {
        report_problem(key, "modifier value must be a finite number");
        return;
    }
    int days = -1;
    const Json& days_json = value.at("days");
    if (days_json.is_number()) {
        const int64_t d = days_json.as_int(-1);
        days = d < 0 ? -1 : static_cast<int>(d);
    } else if (!days_json.is_null()) {
        report_problem(key, "days must be a number");
        return;
    }
    TimedModifier tm;
    tm.source = value.at("source").as_string("script");
    tm.mods.set(static_cast<ModifierKind>(kind), amount);
    tm.days_left = days;
    c->timed_modifiers.push_back(tm);
}

void apply_complete_focus(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a focus key string");
        return;
    }
    const uint32_t focus = g->content.focus_id(value.as_string());
    if (focus == INVALID_FOCUS) {
        report_problem(key, "unknown focus '" + value.as_string() + "'");
        return;
    }
    if (has_completed_focus(*c, focus)) return;
    focus_complete(*g, sc.country, focus);
}

void apply_trigger_event(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    if (!value.is_object()) {
        report_problem(key, "expected {\"key\": \"event\", \"days\": 7}");
        return;
    }
    const Json& event_key = value.at("key");
    if (!event_key.is_string()) {
        report_problem(key, "key must be an event key string");
        return;
    }
    const uint32_t event = g->content.event_id(event_key.as_string());
    if (event == INVALID_ID) {
        report_problem(key, "unknown event '" + event_key.as_string() + "'");
        return;
    }
    int days = 0;
    const Json& days_json = value.at("days");
    if (days_json.is_number()) {
        days = static_cast<int>(days_json.as_int(0));
        if (days < 0) days = 0;
    } else if (!days_json.is_null()) {
        report_problem(key, "days must be a number");
        return;
    }
    if (days > 0) {
        DelayedEvent pending;
        pending.country = sc.country;
        pending.event = event;
        pending.due = g->world.tick + static_cast<Tick>(days) * TICKS_PER_DAY;
        g->world.delayed_events.push_back(pending);
        return;
    }
    fire_event(*g, sc.country, event);
}

void apply_set_flag(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_string() || value.as_string().empty()) {
        report_problem(key, "expected a non-empty flag name string");
        return;
    }
    if (!has_country_flag(*c, value.as_string())) c->country_flags.push_back(value.as_string());
}

void apply_clear_flag(const ScriptScope& sc, const std::string& key, const Json& value) {
    Country* c = effect_country(sc, key);
    if (c == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a flag name string");
        return;
    }
    c->country_flags.erase(std::remove(c->country_flags.begin(), c->country_flags.end(),
                                       value.as_string()),
                           c->country_flags.end());
}

void apply_set_variable(const ScriptScope& sc, const std::string& key, const Json& value,
                        bool add_to_existing) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    if (!value.is_object()) {
        report_problem(key, "expected {\"name\": \"x\", \"value\": 1}");
        return;
    }
    const Json& name = value.at("name");
    if (!name.is_string() || name.as_string().empty()) {
        report_problem(key, "expected a non-empty variable name");
        return;
    }
    double amount = 0.0;
    if (!finite_number(value.at("value"), &amount)) {
        report_problem(key, "variable value must be a finite number");
        return;
    }
    double& slot = g->world.script_vars[name.as_string()];
    slot = add_to_existing ? slot + amount : amount;
}

void apply_add_claim(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a state key string");
        return;
    }
    const StateId id = find_state(*g, value.as_string());
    State* state = id.valid() ? g->world.state(id) : nullptr;
    if (state == nullptr) {
        report_problem(key, "unknown state key '" + value.as_string() + "'");
        return;
    }
    // A claim is what the peace settlement reads: the claimant is added to the
    // state's core owners, once.
    if (std::find(state->core_owners.begin(), state->core_owners.end(), sc.country) ==
        state->core_owners.end()) {
        state->core_owners.push_back(sc.country);
    }
}

// Resolves the state an effect applies to; reports and returns nullptr when the
// scope has none.
State* effect_state(const ScriptScope& sc, const std::string& key) {
    if (sc.game == nullptr) {
        report_problem(key, "effect needs a game");
        return nullptr;
    }
    State* state = sc.state.valid() ? sc.game->world.state(sc.state) : nullptr;
    if (state == nullptr) {
        report_problem(key, "effect needs a state scope");
        return nullptr;
    }
    return state;
}

void apply_set_state_flag(const ScriptScope& sc, const std::string& key, const Json& value) {
    State* state = effect_state(sc, key);
    if (state == nullptr) return;
    if (!value.is_string() || value.as_string().empty()) {
        report_problem(key, "expected a non-empty flag name string");
        return;
    }
    if (!has_state_flag(*state, value.as_string())) state->flags.push_back(value.as_string());
}

void apply_clear_state_flag(const ScriptScope& sc, const std::string& key, const Json& value) {
    State* state = effect_state(sc, key);
    if (state == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected a flag name string");
        return;
    }
    state->flags.erase(std::remove(state->flags.begin(), state->flags.end(), value.as_string()),
                       state->flags.end());
}

// Resolves a spirit key; reports and returns INVALID_ID when it names nothing.
uint32_t resolve_spirit(const Game& g, const std::string& key, const Json& value) {
    if (!value.is_string()) {
        report_problem(key, "expected a national spirit key string");
        return INVALID_ID;
    }
    const uint32_t spirit = g.content.spirit_id(value.as_string());
    if (spirit == INVALID_ID || g.content.spirit(spirit) == nullptr) {
        report_problem(key, "unknown national spirit '" + value.as_string() + "'");
        return INVALID_ID;
    }
    return spirit;
}

void apply_add_national_spirit(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    const uint32_t spirit = resolve_spirit(*g, key, value);
    if (spirit == INVALID_ID) return;
    if (has_spirit(g->world, sc.country, value.as_string())) return;  // already held: no-op
    // Effects grant: no trigger gate and no political-power cost, but the slot
    // invariant still holds (spirit_grant refuses when no slot is free).
    if (!spirit_grant(*g, sc.country, spirit)) {
        report_problem(key, "national spirit '" + value.as_string() + "' could not be granted");
    }
}

void apply_remove_national_spirit(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    const uint32_t spirit = resolve_spirit(*g, key, value);
    if (spirit == INVALID_ID) return;
    spirit_remove(*g, sc.country, spirit);
}

void apply_add_advisor(const ScriptScope& sc, const std::string& key, const Json& value) {
    Game* g = sc.game;
    if (effect_country(sc, key) == nullptr) return;
    if (!value.is_string()) {
        report_problem(key, "expected an advisor key string");
        return;
    }
    const uint32_t advisor = g->content.advisor_id(value.as_string());
    if (advisor == INVALID_ID || g->content.advisor(advisor) == nullptr) {
        report_problem(key, "unknown advisor '" + value.as_string() + "'");
        return;
    }
    if (has_advisor(g->world, sc.country, value.as_string())) return;  // already appointed
    // Effects grant the appointment for free; the command layer is what charges
    // political power. advisor_grant still enforces the free-slot invariant.
    if (!advisor_grant(*g, sc.country, advisor)) {
        report_problem(key, "advisor '" + value.as_string() + "' could not be granted");
    }
}

void apply_effect_key(const ScriptScope& sc, const std::string& key, const Json& value) {
    if (key == "add_political_power") return apply_add_political_power(sc, key, value);
    if (key == "add_stability") return apply_stability(sc, key, value);
    if (key == "add_war_support") return apply_war_support(sc, key, value);
    if (key == "add_manpower") return apply_add_manpower(sc, key, value);
    if (key == "add_fuel") return apply_add_fuel(sc, key, value);
    if (key == "add_opinion") return apply_add_opinion(sc, key, value);
    if (key == "declare_war") return apply_declare_war(sc, key, value);
    if (key == "add_tech") return apply_add_tech(sc, key, value);
    if (key == "set_law") return apply_set_law(sc, key, value);
    if (key == "add_modifier") return apply_add_modifier(sc, key, value);
    if (key == "complete_focus") return apply_complete_focus(sc, key, value);
    if (key == "trigger_event") return apply_trigger_event(sc, key, value);
    if (key == "set_flag") return apply_set_flag(sc, key, value);
    if (key == "clear_flag") return apply_clear_flag(sc, key, value);
    if (key == "set_state_flag") return apply_set_state_flag(sc, key, value);
    if (key == "clear_state_flag") return apply_clear_state_flag(sc, key, value);
    if (key == "set_variable") return apply_set_variable(sc, key, value, false);
    if (key == "add_to_variable") return apply_set_variable(sc, key, value, true);
    if (key == "add_claim") return apply_add_claim(sc, key, value);
    if (key == "add_national_spirit") return apply_add_national_spirit(sc, key, value);
    if (key == "remove_national_spirit") return apply_remove_national_spirit(sc, key, value);
    if (key == "add_advisor") return apply_add_advisor(sc, key, value);
    report_unknown("effect", key);
}

}  // namespace

bool eval_trigger(const ScriptScope& scope, const Json& trigger) {
    return eval_trigger_internal(scope, trigger, 0);
}

void apply_effects(const ScriptScope& scope, const Json& effects) {
    if (effects.is_null()) return;
    if (!effects.is_object()) {
        report_problem("effects", "effect block must be a JSON object");
        return;
    }
    if (scope.game == nullptr) {
        report_problem("effects", "effect block needs a game");
        return;
    }
    if (g_effect_depth >= kMaxScriptDepth) {
        report_problem("effects", "effect recursion deeper than the script engine allows");
        return;
    }
    ++g_effect_depth;
    for (const auto& item : effects.object_items()) {
        apply_effect_key(scope, item.first, item.second);
    }
    --g_effect_depth;
}

bool eval_random_chance(Game& g, double chance) {
    if (!(chance > 0.0)) return false;  // also rejects NaN
    if (chance >= 1.0) return true;
    return g.rng.get(RngStream::Events).chance(chance);
}

std::string describe_script_error(const std::string& key, const std::string& reason) {
    return "script '" + key + "': " + reason;
}

}  // namespace hoi