// National spirits and political advisors (spec section 56).
//
// A national spirit is a permanent, named modifier block with an availability
// trigger and a slot cost; an advisor is a permanent modifier block a country buys
// with political power. Both are data (data/content.h) and both live in the same
// permanent-modifier store, Country::national_spirits, keyed by their content key
// (`TimedModifier::source`), so the country modifier stack treats them like any
// other modifier and tooltips can say where a modifier came from.
//
// This file owns only the engine rule: availability, add/remove, slot accounting,
// the daily consistency pass and the AI that appoints advisors and adopts spirits
// through the normal command queue.
//
// Determinism: countries are walked in ascending id (Store::for_each) and content in
// ascending index; ties in a score break on the lowest index. No unordered container
// is iterated, no randomness is consumed (spirits and advisors are deterministic
// choices) and every score is clamped finite before it can be compared.

#include "sim/spirits.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "core/types.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/script.h"
#include "sim/world.h"

namespace hoi {
namespace {

// -------------------------------------------------------------- helpers ------

ScriptScope country_scope(Game* g, CountryId country) {
    ScriptScope scope;
    scope.game = g;
    scope.country = country;
    return scope;
}

// Pure trigger evaluation for the read-only availability helpers: the script engine
// needs a mutable Game pointer for its RNG, but neither check writes gameplay state.
ScriptScope const_country_scope(const Game& g, CountryId country) {
    ScriptScope scope;
    scope.game = const_cast<Game*>(&g);
    scope.country = country;
    return scope;
}

bool holds_spirit_index(const Country& c, uint32_t index) {
    return std::find(c.spirit_keys.begin(), c.spirit_keys.end(), index) != c.spirit_keys.end();
}

bool holds_advisor_index(const Country& c, uint32_t index) {
    return std::find(c.advisors.begin(), c.advisors.end(), index) != c.advisors.end();
}

bool has_source(const Country& c, const std::string& key) {
    for (const TimedModifier& tm : c.national_spirits) {
        if (tm.source == key) return true;
    }
    return false;
}

void erase_source(Country& c, const std::string& key) {
    size_t keep = 0;
    for (size_t i = 0; i < c.national_spirits.size(); ++i) {
        if (c.national_spirits[i].source == key) continue;
        c.national_spirits[keep++] = c.national_spirits[i];
    }
    c.national_spirits.resize(keep);
}

// Slots consumed by the spirits a country holds. Unknown indices (content changed
// under a live save) occupy nothing; phase_spirits prunes them.
int spirit_used_slots(const Game& g, const Country& c) {
    int used = 0;
    for (uint32_t key : c.spirit_keys) {
        const SpiritDef* def = g.content.spirit(key);
        if (def != nullptr) used += def->slots;
    }
    return used;
}

constexpr size_t kMaxReasons = 32;

// A new reason appends while there is room; once full it replaces the weakest, so
// the debugger shows the decisions that actually drove the country.
void record_reason(Game& g, AiReason reason) {
    reason.score = finite_or(reason.score, 0.0);
    for (auto& f : reason.factors) f.second = finite_or(f.second, 0.0);
    AiLayerState& st = g.ai.layer(AiLayer::Politics);
    if (st.last_reasons.size() < kMaxReasons) {
        st.last_reasons.push_back(std::move(reason));
    } else {
        size_t weakest = 0;
        for (size_t i = 1; i < st.last_reasons.size(); ++i) {
            if (st.last_reasons[i].score < st.last_reasons[weakest].score) weakest = i;
        }
        if (reason.score > st.last_reasons[weakest].score) {
            st.last_reasons[weakest] = std::move(reason);
        }
    }
    ++g.ai.decisions_made;
}

// Every AI action is a command, checked by the command system before it is queued -
// the same path a human's order takes. An illegal command is never queued.
bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

// ------------------------------------------------------------ AI scoring ----
//
// How much one unit of each modifier is worth to an AI country, mirroring the focus
// scorer (src/sim/focus.cpp) so the two layers value the same modifiers the same way.

double modifier_weight(ModifierKind k) {
    switch (k) {
        case ModifierKind::FactoryOutput: return 60.0;
        case ModifierKind::DockyardOutput: return 30.0;
        case ModifierKind::ConstructionSpeed: return 50.0;
        case ModifierKind::ResearchSpeed: return 40.0;
        case ModifierKind::DivisionOrganization: return 80.0;
        case ModifierKind::DivisionAttack: return 60.0;
        case ModifierKind::DivisionDefense: return 60.0;
        case ModifierKind::DivisionBreakthrough: return 40.0;
        case ModifierKind::DivisionRecoveryRate: return 20.0;
        case ModifierKind::SupplyConsumption: return -20.0;
        case ModifierKind::MaxPlanning: return 20.0;
        case ModifierKind::PlanningSpeed: return 15.0;
        case ModifierKind::ManpowerGrowth: return 30.0;
        case ModifierKind::PoliticalPowerGain: return 50.0;
        case ModifierKind::FuelGain: return 25.0;
        case ModifierKind::TrainingTime: return -30.0;
        case ModifierKind::EquipmentCostFactor: return -30.0;
        case ModifierKind::RecruitablePopulation: return 40.0;
        case ModifierKind::Stability: return 45.0;
        case ModifierKind::WarSupport: return 45.0;
        case ModifierKind::EntrenchmentSpeed: return 15.0;
        case ModifierKind::CombatWidth: return 20.0;
        case ModifierKind::ConvoyDefense: return 15.0;
        case ModifierKind::Count: break;
    }
    return 0.0;
}

// Modifiers a country at war needs most (attack, organisation and supply).
bool military_modifier(ModifierKind k) {
    switch (k) {
        case ModifierKind::DivisionOrganization:
        case ModifierKind::DivisionAttack:
        case ModifierKind::DivisionDefense:
        case ModifierKind::DivisionBreakthrough:
        case ModifierKind::DivisionRecoveryRate:
        case ModifierKind::SupplyConsumption:
        case ModifierKind::MaxPlanning:
        case ModifierKind::PlanningSpeed:
        case ModifierKind::EntrenchmentSpeed:
        case ModifierKind::CombatWidth:
        case ModifierKind::ConvoyDefense:
            return true;
        default:
            return false;
    }
}

// Modifiers a country at peace needs most (construction, research, output).
bool economy_modifier(ModifierKind k) {
    switch (k) {
        case ModifierKind::FactoryOutput:
        case ModifierKind::DockyardOutput:
        case ModifierKind::ConstructionSpeed:
        case ModifierKind::ResearchSpeed:
        case ModifierKind::ManpowerGrowth:
        case ModifierKind::PoliticalPowerGain:
        case ModifierKind::FuelGain:
        case ModifierKind::EquipmentCostFactor:
        case ModifierKind::RecruitablePopulation:
            return true;
        default:
            return false;
    }
}

constexpr double kWarPostureBoost = 1.5;   // military modifiers matter more at war
constexpr double kPeacePostureBoost = 1.5;  // industry/research matter more at peace

double modifier_score(const Modifiers& m, bool at_war) {
    double score = 0.0;
    for (int i = 0; i < static_cast<int>(ModifierKind::Count); ++i) {
        const ModifierKind k = static_cast<ModifierKind>(i);
        double w = modifier_weight(k);
        if (at_war && military_modifier(k)) {
            w *= kWarPostureBoost;
        } else if (!at_war && economy_modifier(k)) {
            w *= kPeacePostureBoost;
        }
        score += m.get(k) * w;
    }
    return finite_or(score, 0.0);
}

}  // namespace

// ------------------------------------------------------------------ spirits --

bool spirit_available(const Game& g, CountryId country, uint32_t spirit) {
    const SpiritDef* def = g.content.spirit(spirit);
    if (def == nullptr) return false;
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    if (holds_spirit_index(*c, spirit)) return false;
    if (!def->available.is_null() &&
        !eval_trigger(const_country_scope(g, country), def->available)) {
        return false;
    }
    if (spirit_slots_free(g, country) < def->slots) return false;
    return true;
}

// Raw grant: skips the availability trigger and any cost, but never exceeds the slot
// capacity. This is what script effects use, so content can hand out a spirit whose
// trigger does not pass (a focus reward) without breaking the slot invariant.
bool spirit_grant(Game& g, CountryId country, uint32_t spirit) {
    const SpiritDef* def = g.content.spirit(spirit);
    if (def == nullptr) return false;
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    if (holds_spirit_index(*c, spirit)) return false;  // already held: no-op
    if (spirit_slots_free(g, country) < def->slots) return false;

    // The key is pushed before the effects run so a recursive `add_national_spirit`
    // for the same spirit sees it as held and cannot loop or apply twice.
    c->spirit_keys.push_back(spirit);
    TimedModifier tm;
    tm.source = def->key;
    tm.mods = def->modifiers;
    tm.days_left = -1;  // spirits are permanent
    c->national_spirits.push_back(std::move(tm));

    apply_effects(country_scope(&g, country), def->effects);
    g.log_event("spirit", c->tag + " adopted national spirit '" + def->key + "'", country);
    return true;
}

bool spirit_add(Game& g, CountryId country, uint32_t spirit) {
    if (!spirit_available(g, country, spirit)) return false;
    return spirit_grant(g, country, spirit);
}

bool spirit_remove(Game& g, CountryId country, uint32_t spirit) {
    Country* c = g.world.country(country);
    if (c == nullptr) return false;
    const SpiritDef* def = g.content.spirit(spirit);

    const auto it = std::find(c->spirit_keys.begin(), c->spirit_keys.end(), spirit);
    const bool held_key = it != c->spirit_keys.end();
    bool held_mod = false;
    if (def != nullptr) {
        held_mod = has_source(*c, def->key);
        if (held_key) c->spirit_keys.erase(it);
        if (held_mod) erase_source(*c, def->key);
    } else if (held_key) {
        c->spirit_keys.erase(it);
    }
    if (!held_key && !held_mod) return false;  // country does not hold it

    const std::string key = def != nullptr ? def->key : ("#" + std::to_string(spirit));
    g.log_event("spirit", c->tag + " removed national spirit '" + key + "'", country);
    return true;
}

bool has_spirit(const World& w, CountryId country, const std::string& key) {
    const Country* c = w.country(country);
    if (c == nullptr) return false;
    return has_source(*c, key);
}

int spirit_slots_free(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr) return 0;
    const int free = c->spirit_slots - spirit_used_slots(g, *c);
    return free > 0 ? free : 0;
}

// ----------------------------------------------------------------- advisors --

bool advisor_available(const Game& g, CountryId country, uint32_t advisor) {
    const AdvisorDef* def = g.content.advisor(advisor);
    if (def == nullptr) return false;
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    if (holds_advisor_index(*c, advisor)) return false;
    if (!def->available.is_null() &&
        !eval_trigger(const_country_scope(g, country), def->available)) {
        return false;
    }
    if (advisor_slots_free(g, country) <= 0) return false;
    if (c->political_power < def->cost_pp) return false;
    return true;
}

// Raw appointment for script effects: no political-power cost and no trigger check,
// but a free advisor slot is still required so the capacity invariant holds.
bool advisor_grant(Game& g, CountryId country, uint32_t advisor) {
    const AdvisorDef* def = g.content.advisor(advisor);
    if (def == nullptr) return false;
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    if (holds_advisor_index(*c, advisor)) return false;  // already in office: no-op
    if (advisor_slots_free(g, country) <= 0) return false;

    c->advisors.push_back(advisor);
    TimedModifier tm;
    tm.source = def->key;
    tm.mods = def->modifiers;
    tm.days_left = -1;  // appointments are permanent until dismissed
    c->national_spirits.push_back(std::move(tm));

    g.log_event("advisor", c->tag + " appointed advisor '" + def->key + "'", country);
    return true;
}

bool advisor_appoint(Game& g, CountryId country, uint32_t advisor) {
    if (!advisor_available(g, country, advisor)) return false;
    const AdvisorDef* def = g.content.advisor(advisor);
    Country* c = g.world.country(country);
    if (c == nullptr || def == nullptr) return false;  // narrowed by availability, defensive
    if (!advisor_grant(g, country, advisor)) return false;

    // Charged only after the appointment succeeded; the availability check guarantees
    // the country can pay, so the subtraction never goes negative.
    c->political_power = std::max(0.0, c->political_power - def->cost_pp);
    return true;
}

void advisor_dismiss(Game& g, CountryId country, uint32_t advisor) {
    Country* c = g.world.country(country);
    if (c == nullptr) return;
    const AdvisorDef* def = g.content.advisor(advisor);

    const auto it = std::find(c->advisors.begin(), c->advisors.end(), advisor);
    const bool held = it != c->advisors.end();
    if (!held) return;  // not in office: nothing to dismiss
    c->advisors.erase(it);
    if (def != nullptr) erase_source(*c, def->key);

    // No refund: the political power was spent when the advisor was appointed.
    const std::string key = def != nullptr ? def->key : ("#" + std::to_string(advisor));
    g.log_event("advisor", c->tag + " dismissed advisor '" + key + "'", country);
}

bool has_advisor(const World& w, CountryId country, const std::string& key) {
    const Country* c = w.country(country);
    if (c == nullptr) return false;
    return has_source(*c, key);
}

int advisor_slots_free(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr) return 0;
    const int free = c->advisor_slots - static_cast<int>(c->advisors.size());
    return free > 0 ? free : 0;
}

// -------------------------------------------------------------- daily phase --

// Spirits and advisors are permanent, so there is no timer work. The phase only
// repairs a save whose content changed under it: an index that no longer resolves, or
// a permanent modifier with no held spirit/advisor behind it, is dropped once.
void phase_spirits(Game& g) {
    World& w = g.world;
    // Daily granularity, the same gate as phase_focuses / phase_events.
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) != 0) return;

    w.countries.for_each([&](CountryId, Country& c) {
        if (!c.alive) return;
        int repaired = 0;

        // 1. Drop held indices whose content no longer exists.
        size_t keep = 0;
        for (size_t i = 0; i < c.spirit_keys.size(); ++i) {
            const uint32_t key = c.spirit_keys[i];
            if (g.content.spirit(key) != nullptr) {
                c.spirit_keys[keep++] = key;
            } else {
                ++repaired;
            }
        }
        c.spirit_keys.resize(keep);

        keep = 0;
        for (size_t i = 0; i < c.advisors.size(); ++i) {
            const uint32_t key = c.advisors[i];
            if (g.content.advisor(key) != nullptr) {
                c.advisors[keep++] = key;
            } else {
                ++repaired;
            }
        }
        c.advisors.resize(keep);

        // 2. Drop permanent modifiers no held spirit or advisor accounts for.
        keep = 0;
        for (size_t i = 0; i < c.national_spirits.size(); ++i) {
            const TimedModifier& tm = c.national_spirits[i];
            bool backed = false;
            const uint32_t spirit = g.content.spirit_id(tm.source);
            if (spirit != INVALID_ID && holds_spirit_index(c, spirit)) {
                backed = true;
            } else {
                const uint32_t advisor = g.content.advisor_id(tm.source);
                if (advisor != INVALID_ID && holds_advisor_index(c, advisor)) backed = true;
            }
            if (backed) {
                c.national_spirits[keep++] = tm;
            } else {
                ++repaired;
            }
        }
        c.national_spirits.resize(keep);

        if (repaired > 0) {
            g.log_event("spirit",
                        c.tag + " repaired national spirits (removed " +
                            std::to_string(repaired) + " stale entr" +
                            (repaired == 1 ? "y" : "ies") + ")",
                        c.id);
        }
    });
}

// --------------------------------------------------------------------- AI ----

// Advisors first: the best affordable appointment by modifier score, one per run
// while a slot is free. Then spirits: the best one whose trigger passes and whose
// modifiers fit the country's posture, one per run while a slot is free. Both choices
// are pushed as commands and only queued after validate_command accepts them.
void ai_spirit_advisor_layer(Game& g, Country& c) {
    if (!c.alive) return;
    const CountryId country = c.id;
    const bool at_war = c.at_war;

    if (advisor_slots_free(g, country) > 0) {
        bool found = false;
        uint32_t best = INVALID_ID;
        double best_score = 0.0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(g.content.advisors.size()); ++i) {
            if (!advisor_available(g, country, i)) continue;
            const double score = modifier_score(g.content.advisors[i].modifiers, at_war);
            // Strict comparison keeps the lowest index on a tie (deterministic).
            if (!found || score > best_score) {
                found = true;
                best = i;
                best_score = score;
            }
        }
        if (found && best_score > 0.0) {
            const AdvisorDef& def = g.content.advisors[best];
            Command cmd;
            cmd.type = CommandType::AppointAdvisor;
            cmd.country = country;
            cmd.text = def.key;
            if (push_if_valid(g, std::move(cmd))) {
                AiReason reason;
                reason.what = "advisor:" + def.key;
                reason.score = best_score;
                reason.factors = {{"modifier_score", best_score},
                                  {"cost_pp", def.cost_pp},
                                  {"political_power", c.political_power},
                                  {"slots_free", static_cast<double>(advisor_slots_free(g, country))},
                                  {"at_war", at_war ? 1.0 : 0.0}};
                record_reason(g, std::move(reason));
            }
        }
    }

    if (spirit_slots_free(g, country) > 0) {
        bool found = false;
        uint32_t best = INVALID_ID;
        double best_score = 0.0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(g.content.spirits.size()); ++i) {
            if (!spirit_available(g, country, i)) continue;
            const double score = modifier_score(g.content.spirits[i].modifiers, at_war);
            if (!found || score > best_score) {
                found = true;
                best = i;
                best_score = score;
            }
        }
        if (found && best_score > 0.0) {
            const SpiritDef& def = g.content.spirits[best];
            Command cmd;
            cmd.type = CommandType::AddNationalSpirit;
            cmd.country = country;
            cmd.text = def.key;
            if (push_if_valid(g, std::move(cmd))) {
                AiReason reason;
                reason.what = "spirit:" + def.key;
                reason.score = best_score;
                reason.factors = {{"modifier_score", best_score},
                                  {"slots_free", static_cast<double>(spirit_slots_free(g, country))},
                                  {"slots_cost", static_cast<double>(def.slots)},
                                  {"at_war", at_war ? 1.0 : 0.0}};
                record_reason(g, std::move(reason));
            }
        }
    }
}

}  // namespace hoi