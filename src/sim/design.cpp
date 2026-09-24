// Equipment designers (spec section 50).
//
// A country fits unlocked components into an equipment archetype; the engine
// computes the resulting statistics and registers them as a real EquipmentDef, so
// a design flows through production, stockpiles, divisions and combat exactly like
// a pre-authored model.
//
// ---------------------------------------------------------------- the formula --
//
// `design_compute` starts from the archetype's EquipmentDef and folds in every
// fitted component, in the order the caller supplies:
//
//   * every stat field a component carries is ADDED to the archetype's value
//     (soft_attack, hard_attack, air_attack, air_defence, ground_attack, agility,
//      range, naval_attack, torpedo_attack, sub_detection, detection, visibility,
//      armor, piercing, defense, breakthrough, hardness, max_strength, organization,
//      speed, reliability, fuel_use, supply_use, manpower);
//   * `resources[r]` adds to the archetype's per-unit resource cost;
//   * build_cost = (base.build_cost + sum(build_cost_add)) * product(cost_multiplier);
//   * reliability is clamped to [0.1, 1.0]; every other statistic (and cost,
//     resources, fuel/supply/manpower) is clamped to >= 0;
//   * category, archetype, year and the resource/stat baseline come from the base,
//     and is_archetype is false: a design is always a producible model.
//
// Every arithmetic step is sanitised with finite_or so an absurd component set can
// never produce a NaN or infinity. `design_compute` never touches Content.
//
// The AI trades capability for cost: it accepts a fitted design over the best model
// the country can already build when the weighted-statistic capability gain is at
// least kDesignMinGainRatio (10%) AND the build-cost increase is within
// kDesignMaxCostRatio (+25%). Both constants are defined and documented in the AI
// section below; the AiReason factors carry gain, baseline, fitted, cost_delta and
// cost_ratio so the decision is explainable from the log alone.
//
// ------------------------------------------------------------------- the keys --
//
// A design's key is "<tag>_<category>_<n>" where n is the smallest positive integer
// the country has not already used for that category, so keys are deterministic,
// collision-free across countries (the tag is part of them) and independent of the
// display name. `design_exists` matches a design by key OR display name, which is
// what the command layer's duplicate-name rejection needs.

#include "sim/design.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/research.h"
#include "sim/script.h"

namespace hoi {

// ------------------------------------------------------------ designer slots --

const char* component_slot_name(ComponentSlot s) {
    switch (s) {
        case ComponentSlot::Armor: return "armor";
        case ComponentSlot::Weapon: return "weapon";
        case ComponentSlot::Engine: return "engine";
        case ComponentSlot::Airframe: return "airframe";
        case ComponentSlot::Hull: return "hull";
        case ComponentSlot::Special: return "special";
        case ComponentSlot::Count: break;
    }
    return "unknown";
}

bool component_slot_from_name(const std::string& name, ComponentSlot* out) {
    // Case- and separator-insensitive, like the modifier/category name matching in
    // the content loader: "Air Frame", "air_frame" and "Airframe" all resolve.
    auto normalize = [](const std::string& s) {
        std::string normal;
        normal.reserve(s.size());
        for (char ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) normal.push_back(ch);
        }
        return normal;
    };
    static const ComponentSlot kSlots[] = {
        ComponentSlot::Armor, ComponentSlot::Weapon, ComponentSlot::Engine,
        ComponentSlot::Airframe, ComponentSlot::Hull, ComponentSlot::Special,
    };
    const std::string wanted = normalize(name);
    for (ComponentSlot s : kSlots) {
        if (wanted == normalize(component_slot_name(s))) {
            if (out != nullptr) *out = s;
            return true;
        }
    }
    return false;
}

namespace {

// A country scope for a trigger when only `const Game` is available. Triggers are
// pure observers (they never write gameplay state), so this mirrors the existing
// const_country_scope helper used by the spirit/advisor availability checks.
ScriptScope const_country_scope(const Game& g, CountryId country) {
    ScriptScope scope;
    scope.game = const_cast<Game*>(&g);
    scope.country = country;
    return scope;
}

// TechId of an entry of Content::techs: the loaded id when the loader assigned one,
// otherwise the index (the documented table layout). Same rule as research.cpp.
TechId tech_id_at(const Content& content, size_t index) {
    const TechDef& t = content.techs[index];
    return t.id.valid() ? t.id : TechId(static_cast<uint32_t>(index));
}

double non_negative(double v) { return std::max(0.0, finite_or(v, 0.0)); }

// Clamp every numeric field of `e` to its documented range. Applied once, after the
// additive pass, so a component can push a value out of range and still land on a
// legal, finite result.
void sanitize_equipment(EquipmentDef& e) {
    e.soft_attack = non_negative(e.soft_attack);
    e.hard_attack = non_negative(e.hard_attack);
    e.air_attack = non_negative(e.air_attack);
    e.air_defence = non_negative(e.air_defence);
    e.ground_attack = non_negative(e.ground_attack);
    e.agility = non_negative(e.agility);
    e.range = non_negative(e.range);
    e.naval_attack = non_negative(e.naval_attack);
    e.torpedo_attack = non_negative(e.torpedo_attack);
    e.sub_detection = non_negative(e.sub_detection);
    e.detection = non_negative(e.detection);
    e.visibility = non_negative(e.visibility);
    e.defense = non_negative(e.defense);
    e.breakthrough = non_negative(e.breakthrough);
    e.armor = non_negative(e.armor);
    e.piercing = non_negative(e.piercing);
    e.hardness = non_negative(e.hardness);
    e.speed = non_negative(e.speed);
    e.max_strength = non_negative(e.max_strength);
    e.organization = non_negative(e.organization);
    e.build_cost = non_negative(e.build_cost);
    e.fuel_use = non_negative(e.fuel_use);
    e.supply_use = non_negative(e.supply_use);
    e.manpower = non_negative(e.manpower);
    for (int r = 0; r < RESOURCE_COUNT; ++r) e.resources[r] = non_negative(e.resources[r]);
    // Reliability is the one stat with a floor as well as a ceiling: a design can
    // never be more reliable than "perfect" nor drop to (or below) zero.
    e.reliability = std::min(1.0, std::max(0.1, finite_or(e.reliability, 0.5)));
}

// Deterministic key for the next design of `category` the country owns.
std::string next_design_key(const Game& g, const Country& c, EquipmentCategory category) {
    const std::string prefix = c.tag + "_" + equipment_category_name(category) + "_";
    std::set<long> used;
    for (uint32_t di : c.designs) {
        const EquipmentDesign* d = g.content.design(di);
        if (d == nullptr || d->key.rfind(prefix, 0) != 0) continue;
        long n = 0;
        bool digits = !d->key.empty() && d->key.size() > prefix.size();
        for (size_t i = prefix.size(); digits && i < d->key.size(); ++i) {
            const char ch = d->key[i];
            if (ch < '0' || ch > '9') {
                digits = false;
                break;
            }
            n = n * 10 + (ch - '0');
        }
        if (digits && n > 0) used.insert(n);
    }
    long n = 1;
    while (used.count(n) != 0 ||
           g.content.equipment_by_key.count(prefix + std::to_string(n)) != 0) {
        ++n;
    }
    return prefix + std::to_string(n);
}

}  // namespace

// ----------------------------------------------------------- unlocking rules ---

bool component_unlocked(const Game& g, CountryId country, uint32_t component) {
    const ComponentDef* comp = g.content.component(component);
    const Country* c = g.world.country(country);
    if (comp == nullptr || c == nullptr) return false;

    // Year gate: the component must have arrived by the current scenario year.
    if (g.world.date.year < comp->year) return false;

    // Trigger gate: a null trigger means "always available"; anything else must
    // pass in a country scope.
    if (!comp->available.is_null() &&
        !eval_trigger(const_country_scope(g, country), comp->available)) {
        return false;
    }

    // Technology gate: a technology that names the component key in
    // `unlock_equipment` makes it gated; the component is unlocked when such a
    // technology is completed and stays available when no technology names it
    // (mirrors equipment_unlocked).
    bool gated = false;
    for (size_t i = 0; i < g.content.techs.size(); ++i) {
        const TechDef& t = g.content.techs[i];
        for (const std::string& key : t.unlock_equipment) {
            if (key != comp->key) continue;
            gated = true;
            if (c->research.has_tech(tech_id_at(g.content, i))) return true;
        }
    }
    return !gated;
}

std::vector<uint32_t> unlocked_components(const Game& g, CountryId country,
                                          EquipmentCategory category) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < g.content.components.size(); ++i) {
        const ComponentDef& comp = g.content.components[i];
        if (comp.category != category) continue;
        const uint32_t index = static_cast<uint32_t>(i);
        if (component_unlocked(g, country, index)) out.push_back(index);
    }
    return out;
}

bool design_available(const Game& g, CountryId country, EquipmentId archetype) {
    const EquipmentDef* base = g.content.equipment_def(archetype);
    if (base == nullptr || !base->is_archetype) return false;
    if (!equipment_unlocked(g, country, archetype)) return false;
    // The content has no per-slot "required" concept: an archetype is designable as
    // soon as the country has at least one unlocked component of its category (and
    // the archetype is producible). Requiring any specific slot family would need a
    // required flag on ComponentDef, which the frozen header does not carry.
    return !unlocked_components(g, country, base->category).empty();
}

// ---------------------------------------------------------------- computing ----

EquipmentDef design_compute(const Game& g, EquipmentId archetype,
                            const std::vector<std::pair<ComponentSlot, uint32_t>>& components,
                            const std::string& key, const std::string& name) {
    EquipmentDef out;
    const EquipmentDef* base = g.content.equipment_def(archetype);
    if (base == nullptr) {
        out.key = key;
        out.name = name;
        out.id = EquipmentId{};
        return out;
    }
    out = *base;
    out.id = EquipmentId{};  // registration assigns the real id
    out.key = key;
    out.name = name;
    out.is_archetype = false;
    // The parent-archetype key a producible model is grouped by. Archetype content
    // often leaves its own `archetype` string empty (it IS the archetype), so a
    // design of it must carry the archetype's key instead: production searches for
    // upgrades within `archetype`, and an empty string would hide the design from
    // that comparison.
    out.archetype = base->archetype.empty() ? base->key : base->archetype;

    double cost_add = 0.0;
    double cost_multiplier = 1.0;
    for (const auto& fitted : components) {
        const ComponentDef* comp = g.content.component(fitted.second);
        if (comp == nullptr) continue;
        out.soft_attack += comp->soft_attack;
        out.hard_attack += comp->hard_attack;
        out.air_attack += comp->air_attack;
        out.air_defence += comp->air_defence;
        out.ground_attack += comp->ground_attack;
        out.agility += comp->agility;
        out.range += comp->range;
        out.naval_attack += comp->naval_attack;
        out.torpedo_attack += comp->torpedo_attack;
        out.sub_detection += comp->sub_detection;
        out.detection += comp->detection;
        out.visibility += comp->visibility;
        out.armor += comp->armor;
        out.piercing += comp->piercing;
        out.defense += comp->defense;
        out.breakthrough += comp->breakthrough;
        out.hardness += comp->hardness;
        out.max_strength += comp->max_strength;
        out.organization += comp->organization;
        out.speed += comp->speed;
        out.reliability += comp->reliability;
        out.fuel_use += comp->fuel_use;
        out.supply_use += comp->supply_use;
        out.manpower += comp->manpower;
        for (int r = 0; r < RESOURCE_COUNT; ++r) out.resources[r] += comp->resources[r];
        cost_add += comp->build_cost_add;
        cost_multiplier *= comp->cost_multiplier;
    }

    const double raw_cost =
        (finite_or(base->build_cost, 0.0) + cost_add) * finite_or(cost_multiplier, 1.0);
    out.build_cost = finite_or(raw_cost, base->build_cost);
    sanitize_equipment(out);
    return out;
}

// ---------------------------------------------------------------- creating -----

uint32_t design_create(Game& g, CountryId country, const std::string& name,
                       EquipmentId archetype,
                       const std::vector<std::pair<ComponentSlot, uint32_t>>& components) {
    Country* c = g.world.country(country);
    if (c == nullptr) return INVALID_ID;

    const EquipmentDef* base = g.content.equipment_def(archetype);
    if (base == nullptr || !base->is_archetype) return INVALID_ID;
    if (name.empty()) return INVALID_ID;
    if (design_exists(g, name)) return INVALID_ID;
    if (!design_available(g, country, archetype)) return INVALID_ID;

    // Validate the fitted components before touching anything: every slot distinct,
    // every component known, matching the archetype's category and unlocked.
    std::set<uint32_t> seen_slots;
    for (const auto& fitted : components) {
        const uint32_t slot = static_cast<uint32_t>(fitted.first);
        if (slot >= static_cast<uint32_t>(ComponentSlot::Count)) return INVALID_ID;
        if (!seen_slots.insert(slot).second) return INVALID_ID;  // duplicate slot
        const ComponentDef* comp = g.content.component(fitted.second);
        if (comp == nullptr) return INVALID_ID;
        if (comp->category != base->category) return INVALID_ID;
        if (!component_unlocked(g, country, fitted.second)) return INVALID_ID;
    }

    const std::string key = next_design_key(g, *c, base->category);
    if (g.content.equipment_by_key.count(key) != 0) return INVALID_ID;

    EquipmentDef produced = design_compute(g, archetype, components, key, name);
    const EquipmentId produced_id(static_cast<uint32_t>(g.content.equipment.size()));
    produced.id = produced_id;

    EquipmentDesign design;
    design.index = static_cast<uint32_t>(g.content.designs.size());
    design.key = key;
    design.name = name;
    design.country = country;
    design.archetype = archetype;
    design.year = std::max(base->year, g.world.date.year);
    design.components = components;
    design.produced = produced_id;

    // Commit. Content::equipment grows by one, so every country's stockpile table
    // must grow with it before a production line can index the new model.
    g.content.equipment.push_back(std::move(produced));
    g.content.equipment_by_key[key] = produced_id;
    g.content.designs.push_back(std::move(design));
    g.content.design_index[key] = g.content.designs.back().index;
    g.content.design_of_equipment[produced_id.v] = g.content.designs.back().index;
    c->designs.push_back(g.content.designs.back().index);
    g.world.countries.for_each([&](CountryId, Country& other) {
        if (other.equipment_stockpile.size() < g.content.equipment.size()) {
            other.equipment_stockpile.resize(g.content.equipment.size(), 0.0);
        }
    });

    g.log_event("design", c->tag + " created design '" + name + "' (" + key + ")", country);
    return g.content.designs.back().index;
}

bool design_exists(const Game& g, const std::string& key) {
    for (const EquipmentDesign& d : g.content.designs) {
        if (d.key == key || d.name == key) return true;
    }
    return false;
}

// ---------------------------------------------------------------------- AI -----
//
// The design planner thinks the way a player does: fitting components buys
// capability for money. A model's `capability` is a weighted sum of its statistics
// (weights named below and surfaced in AiReason::factors). The planner accepts a
// fitted design over the best model the country can already build when BOTH hold:
//
//   * capability gain >= kDesignMinGainRatio (10%) over the best buildable model of
//     that category, and
//   * build cost <= the reference model's cost * (1 + kDesignMaxCostRatio) (+25%).
//
// A fit that only buys cost without proportional capability, or that blows past the
// affordable margin, is rejected. The reference cost is the best buildable model's
// price, falling back to the archetype's when the country has no model yet.
//
// It designs only for categories the country already produces, picks the best
// unlocked archetype of that category, greedily fits the best improving component
// per slot (ascending slot order) while staying under the cost margin, and queues
// one CreateEquipmentDesign command per qualifying category. Every command is
// validated before it is queued, so the AI can never issue an illegal design.
namespace {

constexpr size_t kMaxDesignReasons = 16;

// Relative capability improvement over the best available model that justifies a
// new design (10%).
constexpr double kDesignMinGainRatio = 0.10;

// How much more a fitted design may cost than the reference model before the planner
// refuses it (+25%). Capability must come with an affordable price tag.
constexpr double kDesignMaxCostRatio = 0.25;

double stat_weight(const EquipmentDef& e) {
    return e.soft_attack * 1.0 + e.hard_attack * 1.0 + e.air_attack * 1.0 +
           e.air_defence * 0.5 + e.ground_attack * 1.0 + e.agility * 0.5 +
           e.defense * 0.5 + e.breakthrough * 0.5 + e.armor * 0.5 + e.piercing * 0.5 +
           e.naval_attack * 1.0 + e.torpedo_attack * 1.0 + e.detection * 0.5 +
           e.sub_detection * 0.5 + e.speed * 0.1 + e.organization * 0.1 +
           e.max_strength * 0.5 + e.reliability * 10.0;
}

// Capability of a model: the weighted statistic sum, independent of price.
double design_capability(const EquipmentDef& e) { return stat_weight(e); }

// The best (highest capability) unlocked archetype of a category, or an invalid id
// when the country cannot design from that category at all.
EquipmentId best_archetype(const Game& g, CountryId country, EquipmentCategory category) {
    EquipmentId best;
    double best_capability = 0.0;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& def = g.content.equipment[i];
        if (!def.is_archetype || def.category != category) continue;
        const EquipmentId id(static_cast<uint32_t>(i));
        if (!design_available(g, country, id)) continue;
        const double capability =
            design_capability(design_compute(g, id, {}, def.key, def.name));
        if (!best.valid() || capability > best_capability) {
            best = id;
            best_capability = capability;
        }
    }
    return best;
}

// A producible model the country already holds, with the numbers the trade-off uses.
struct ModelRef {
    bool found = false;
    double capability = 0.0;
    double cost = 0.0;
};

// Best capability among the producible models of the category the country already
// can build (archetypes excluded: they are abstract and never built).
ModelRef best_existing_model(const Game& g, CountryId country, EquipmentCategory category) {
    ModelRef best;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& def = g.content.equipment[i];
        if (def.is_archetype || def.category != category) continue;
        if (!equipment_unlocked(g, country, EquipmentId(static_cast<uint32_t>(i)))) continue;
        const double capability = design_capability(def);
        if (!best.found || capability > best.capability) {
            best.found = true;
            best.capability = capability;
            best.cost = finite_or(def.build_cost, 0.0);
        }
    }
    return best;
}

void record_design_reason(Game& g, AiReason reason) {
    reason.score = finite_or(reason.score, 0.0);
    for (auto& f : reason.factors) f.second = finite_or(f.second, 0.0);
    AiLayerState& st = g.ai.layer(AiLayer::Industry);
    if (st.last_reasons.size() < kMaxDesignReasons) {
        st.last_reasons.push_back(std::move(reason));
        return;
    }
    size_t weakest = 0;
    for (size_t i = 1; i < st.last_reasons.size(); ++i) {
        if (st.last_reasons[i].score < st.last_reasons[weakest].score) weakest = i;
    }
    if (reason.score > st.last_reasons[weakest].score) {
        st.last_reasons[weakest] = std::move(reason);
    }
}

bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

// A display name that is unique across every design in the world.
std::string next_design_name(const Game& g, const Country& c, EquipmentCategory category) {
    const std::string label = equipment_category_name(category);
    const std::string base_name = c.name.empty() ? c.tag : c.name;
    for (int n = 1; n < 100000; ++n) {
        const std::string name = base_name + " " + label + " " + std::to_string(n);
        if (!design_exists(g, name)) return name;
    }
    return c.tag + "_" + label + "_auto";
}

}  // namespace

void ai_design_layer(Game& g, Country& c) {
    if (!c.alive) return;
    // Designing is a strategic decision, not a daily one: evaluating every category
    // against every unlocked component costs real time, and the answer only changes
    // when a technology or the calendar unlocks something. Once a month is plenty,
    // and it keeps the layer's cost off the daily AI path.
    if (g.world.date.day != 1) return;
    const CountryId country = c.id;

    // Categories this country actually produces, ascending and de-duplicated.
    std::vector<EquipmentCategory> categories;
    for (const ProductionLine& line : c.lines) {
        const EquipmentDef* def = g.content.equipment_def(line.equipment);
        if (def == nullptr) continue;
        if (std::find(categories.begin(), categories.end(), def->category) == categories.end()) {
            categories.push_back(def->category);
        }
    }

    for (EquipmentCategory category : categories) {
        const EquipmentId archetype = best_archetype(g, country, category);
        if (!archetype.valid()) continue;
        const EquipmentDef* base = g.content.equipment_def(archetype);
        const ModelRef existing = best_existing_model(g, country, category);
        // The price a new design is measured against: the best buildable model's, or
        // the archetype's when the country has nothing of the category yet.
        const double reference_cost =
            existing.found && existing.cost > 0.0 ? existing.cost : base->build_cost;
        const bool has_cost_cap = reference_cost > 0.0;
        const double cost_cap = reference_cost * (1.0 + kDesignMaxCostRatio);

        // Greedy component fit: for each slot in ascending order keep the unlocked
        // component that buys the most capability while staying under the cost
        // margin, and only if it improves capability at all.
        std::vector<std::pair<ComponentSlot, uint32_t>> fitted;
        const EquipmentDef empty_model =
            design_compute(g, archetype, fitted, std::string(), std::string());
        double current_capability = design_capability(empty_model);
        for (uint32_t slot_i = 0; slot_i < static_cast<uint32_t>(ComponentSlot::Count);
             ++slot_i) {
            const ComponentSlot slot = static_cast<ComponentSlot>(slot_i);
            uint32_t best_component = INVALID_ID;
            double best_capability = current_capability;
            for (uint32_t index : unlocked_components(g, country, category)) {
                const ComponentDef* comp = g.content.component(index);
                if (comp == nullptr || comp->slot != slot) continue;
                std::vector<std::pair<ComponentSlot, uint32_t>> trial = fitted;
                trial.emplace_back(slot, index);
                const EquipmentDef trial_model =
                    design_compute(g, archetype, trial, std::string(), std::string());
                if (has_cost_cap &&
                    finite_or(trial_model.build_cost, 0.0) > cost_cap + 1e-9) {
                    continue;  // this fit busts the affordable price margin
                }
                const double capability = design_capability(trial_model);
                if (capability > best_capability + 1e-9) {
                    best_capability = capability;
                    best_component = index;
                }
            }
            if (best_component != INVALID_ID) {
                fitted.emplace_back(slot, best_component);
                current_capability = best_capability;
            }
        }
        if (fitted.empty()) continue;  // nothing unlocked to fit: keep the current model

        const EquipmentDef fitted_model =
            design_compute(g, archetype, fitted, std::string(), std::string());
        const double fitted_capability = design_capability(fitted_model);
        const double fitted_cost = finite_or(fitted_model.build_cost, 0.0);
        const double baseline = existing.capability;
        const double gain_ratio =
            baseline > 0.0 ? (fitted_capability - baseline) / baseline
                           : (fitted_capability > 0.0 ? 1.0 : 0.0);
        const double cost_ratio = reference_cost > 0.0 ? fitted_cost / reference_cost : 1.0;
        const bool enough_gain = gain_ratio >= kDesignMinGainRatio;
        const bool affordable = !has_cost_cap || cost_ratio <= 1.0 + kDesignMaxCostRatio;
        if (!enough_gain || !affordable) continue;

        const std::string name = next_design_name(g, c, category);
        Command cmd;
        cmd.type = CommandType::CreateEquipmentDesign;
        cmd.country = country;
        cmd.equipment = archetype;
        cmd.text = name;
        cmd.components.reserve(fitted.size());
        for (const auto& choice : fitted) {
            cmd.components.emplace_back(static_cast<uint8_t>(choice.first), choice.second);
        }
        if (!push_if_valid(g, std::move(cmd))) continue;

        AiReason reason;
        reason.what = std::string("design:") + equipment_category_name(category);
        reason.score = gain_ratio;
        reason.factors = {
            {"gain", gain_ratio},
            {"baseline", baseline},
            {"fitted", fitted_capability},
            {"cost_delta", fitted_cost - (base != nullptr ? base->build_cost : 0.0)},
            {"cost_ratio", cost_ratio},
            {"components", static_cast<double>(fitted.size())},
            {"category", static_cast<double>(static_cast<int>(category))},
        };
        record_design_reason(g, std::move(reason));
    }
}

}  // namespace hoi