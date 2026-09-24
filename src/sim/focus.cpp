// National focus trees (spec section 57).
//
// A focus is pure data: prerequisites, exclusions, availability/bypass triggers, a
// duration and an effect block, all evaluated by the script engine (src/sim/script.h).
// This file owns only the engine rule: selection, the daily progress, completion and
// timed-modifier expiry, plus the AI that picks a focus exactly like a human (it
// pushes a SelectFocus command through the normal queue).
//
// Determinism: countries and focuses are walked in ascending index order, ties in
// the AI score break on the lowest focus index, no unordered container is iterated,
// and every derived score is clamped to a finite value before it can be compared.

#include "sim/focus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/politics.h"
#include "sim/script.h"
#include "sim/world.h"

namespace hoi {
namespace {

// --------------------------------------------------------------- AI weights --
//
// Every weight below is a named part of the scoring model. The AI debugger shows
// the same names in AiReason::factors, so `last_reasons` explains a focus choice
// without a debugger.

// Effect blocks, per unit of the payload.
constexpr double kFocusPpPerPoint = 0.05;        // political power point
constexpr double kFocusStabilityPerPoint = 100.0;  // stability unit (0..1)
constexpr double kFocusWarSupportPerPoint = 100.0;
constexpr double kFocusManpowerPerPerson = 0.0005;
constexpr double kFocusFuelPerUnit = 0.02;
constexpr double kFocusTech = 15.0;
constexpr double kFocusClaim = 10.0;
constexpr double kFocusDeclareWar = 15.0;
constexpr double kFocusSetLaw = 5.0;
constexpr double kFocusCompleteFocus = 10.0;
constexpr double kFocusTriggerEvent = 3.0;
constexpr double kFocusOpinion = 2.0;

// Posture adjustments, applied when the country needs the effect more.
constexpr double kFocusWarModifierBoost = 0.5;        // adds to the military factor
constexpr double kFocusStabilityNeedBoost = 1.5;
constexpr double kFocusFactoryNeedBoost = 1.5;
constexpr double kFocusResearchNeedBoost = 1.0;
constexpr double kFocusWarDeclarePacifist = 0.5;
constexpr double kFocusWarDeclareInterventionist = 2.0;

// Posture thresholds.
constexpr double kFocusLowStability = 0.50;
constexpr double kFocusFewFactories = 20.0;
constexpr double kFocusResearchPerYear = 3.0;
constexpr int kFocusFirstYear = 1936;

constexpr size_t kMaxFocusReasons = 32;

// -------------------------------------------------------------- small helpers --

bool is_completed(const Country& c, FocusIndex focus) {
    return std::find(c.completed_focuses.begin(), c.completed_focuses.end(), focus) !=
           c.completed_focuses.end();
}

ScriptScope country_scope(Game* g, CountryId c) {
    ScriptScope scope;
    scope.game = g;
    scope.country = c;
    return scope;
}

// Case/separator-insensitive comparison, matching how content resolves modifier
// names (data/content.cpp). Kept local so focus scoring never depends on the loader.
std::string normalize_name(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')) {
            out.push_back(static_cast<char>(u >= 'A' && u <= 'Z' ? u - 'A' + 'a' : u));
        }
    }
    return out;
}

int modifier_index(const std::string& key) {
    const std::string want = normalize_name(key);
    if (want.empty()) return -1;
    for (int i = 0; i < static_cast<int>(ModifierKind::Count); ++i) {
        if (normalize_name(modifier_kind_name(static_cast<ModifierKind>(i))) == want) return i;
    }
    return -1;
}

// Value of one modifier kind, in score points, per unit of the modifier.
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

// ------------------------------------------------------------- AI posture -----

struct FocusPosture {
    bool at_war = false;
    double stability_need = 0.0;  // 0..1, 1 = very unstable
    double factory_need = 0.0;    // 0..1, 1 = almost no industry
    double research_need = 0.0;   // 0..1, 1 = far behind the calendar
};

FocusPosture focus_posture(const Game& g, CountryId country) {
    FocusPosture p;
    const Country* c = g.world.country(country);
    if (!c) return p;
    p.at_war = c->at_war;

    if (c->stability < kFocusLowStability) {
        p.stability_need = clamp01((kFocusLowStability - c->stability) / kFocusLowStability);
    }

    double factories = 0.0;
    g.world.states.for_each([&](StateId, const State& s) {
        if (s.controller == country) factories += static_cast<double>(s.total_factories());
    });
    if (factories < kFocusFewFactories) {
        p.factory_need = clamp01((kFocusFewFactories - factories) / kFocusFewFactories);
    }

    const int year = g.world.date.year;
    const size_t expected = static_cast<size_t>(std::max(1, year - kFocusFirstYear + 1)) *
                            static_cast<size_t>(kFocusResearchPerYear);
    const size_t done = c->research.completed.size();
    if (done < expected) {
        p.research_need = clamp01(static_cast<double>(expected - done) /
                                  static_cast<double>(expected));
    }
    return p;
}

// Scores the effects of a focus for `c`, appending each contribution to `factors`.
// The return value is always finite; content cannot push a NaN into the comparator.
double score_effects(const FocusDef& def, const FocusPosture& posture,
                     std::vector<std::pair<std::string, double>>* factors) {
    double score = 0.0;
    const Json& e = def.effects;
    if (!e.is_object()) return score;

    auto add = [&](const std::string& name, double value) {
        if (!std::isfinite(value) || value == 0.0) return;
        score += value;
        factors->emplace_back(name, value);
    };

    const double war_mod = posture.at_war ? kFocusWarModifierBoost : 0.0;
    for (const auto& item : e.object_items()) {
        const std::string& key = item.first;
        const Json& val = item.second;
        if (key == "add_political_power") {
            add("add_political_power", val.as_double(0.0) * kFocusPpPerPoint);
        } else if (key == "add_stability") {
            add("add_stability", val.as_double(0.0) * kFocusStabilityPerPoint *
                                     (1.0 + posture.stability_need * kFocusStabilityNeedBoost));
        } else if (key == "add_war_support") {
            add("add_war_support", val.as_double(0.0) * kFocusWarSupportPerPoint *
                                       (1.0 + war_mod));
        } else if (key == "add_manpower") {
            add("add_manpower", val.as_double(0.0) * kFocusManpowerPerPerson);
        } else if (key == "add_fuel") {
            add("add_fuel", val.as_double(0.0) * kFocusFuelPerUnit);
        } else if (key == "add_tech") {
            add("add_tech", kFocusTech * (1.0 + posture.research_need * kFocusResearchNeedBoost));
        } else if (key == "add_modifier") {
            const int idx = modifier_index(val.at("kind").as_string(""));
            if (idx < 0) continue;
            const ModifierKind kind = static_cast<ModifierKind>(idx);
            const double value = val.at("value").as_double(0.0);
            // Absent or negative `days` is permanent; otherwise scale by duration so a
            // permanent modifier always outranks a short one.
            const double days = val.has("days") ? val.at("days").as_double(0.0) : -1.0;
            const double duration = days < 0.0 ? 1.0 : clamp(days / 365.0, 0.1, 1.5);
            double w = modifier_weight(kind) * duration;
            if (posture.factory_need > 0.0 &&
                (kind == ModifierKind::FactoryOutput || kind == ModifierKind::ConstructionSpeed ||
                 kind == ModifierKind::DockyardOutput)) {
                w *= 1.0 + posture.factory_need * kFocusFactoryNeedBoost;
            }
            if (posture.research_need > 0.0 && kind == ModifierKind::ResearchSpeed) {
                w *= 1.0 + posture.research_need * kFocusResearchNeedBoost;
            }
            if (posture.stability_need > 0.0 && kind == ModifierKind::Stability) {
                w *= 1.0 + posture.stability_need * kFocusStabilityNeedBoost;
            }
            const bool military = kind == ModifierKind::DivisionAttack ||
                                  kind == ModifierKind::DivisionDefense ||
                                  kind == ModifierKind::DivisionBreakthrough ||
                                  kind == ModifierKind::DivisionOrganization;
            if (military) w *= 1.0 + war_mod;
            add(std::string("add_modifier:") + modifier_kind_name(kind), value * w);
        } else if (key == "add_claim") {
            add("add_claim", kFocusClaim);
        } else if (key == "declare_war") {
            add("declare_war", kFocusDeclareWar *
                                   (posture.at_war ? kFocusWarDeclarePacifist
                                                   : kFocusWarDeclareInterventionist));
        } else if (key == "set_law") {
            add("set_law", kFocusSetLaw);
        } else if (key == "add_opinion") {
            add("add_opinion", kFocusOpinion);
        } else if (key == "complete_focus") {
            add("complete_focus", kFocusCompleteFocus);
        } else if (key == "trigger_event") {
            add("trigger_event", kFocusTriggerEvent);
        }
        // set_flag / clear_flag / set_variable / add_to_variable carry no score.
    }
    if (!std::isfinite(score)) return 0.0;
    return score;
}

// --------------------------------------------------------- completion core ----

// Marks a focus completed, optionally applying its effects, clears it from the
// selection and logs the event. The index is pushed before effects run so a
// recursive `complete_focus` effect sees it as completed and cannot loop.
void finish_focus(Game& g, Country& c, FocusIndex focus, const FocusDef& def, bool apply,
                  bool bypassed) {
    if (!is_completed(c, focus)) c.completed_focuses.push_back(focus);
    if (apply) {
        const size_t modifiers_before = c.timed_modifiers.size();
        apply_effects(country_scope(&g, c.id), def.effects);
        // Label modifiers this focus granted with the focus key, so the inspector and
        // the client can say where a modifier came from instead of "script".
        for (size_t i = modifiers_before; i < c.timed_modifiers.size(); ++i) {
            if (c.timed_modifiers[i].source == "script") {
                c.timed_modifiers[i].source = def.key;
            }
        }
    }
    if (c.selected_focus == focus) {
        c.selected_focus = INVALID_FOCUS;
        c.focus_progress = 0.0;
    }
    const std::string name = def.name.empty() ? def.key : def.name;
    g.log_event("focus", bypassed ? ("focus '" + name + "' bypassed")
                                  : ("focus '" + name + "' completed"),
                c.id);
}

}  // namespace

// -------------------------------------------------------------- availability --

bool focus_available(const Game& g, CountryId country, FocusIndex focus) {
    const FocusDef* def = g.content.focus(focus);
    if (!def) return false;
    const Country* c = g.world.country(country);
    if (!c || !c->alive) return false;
    if (is_completed(*c, focus)) return false;
    for (const std::string& key : def->prerequisites) {
        const FocusIndex prereq = g.content.focus_id(key);
        if (prereq == INVALID_FOCUS || !is_completed(*c, prereq)) return false;
    }
    for (const std::string& key : def->mutually_exclusive) {
        const FocusIndex other = g.content.focus_id(key);
        if (other != INVALID_FOCUS && is_completed(*c, other)) return false;
    }
    if (def->available.is_null()) return true;
    return eval_trigger(country_scope(const_cast<Game*>(&g), country), def->available);
}

bool focus_bypassable(const Game& g, CountryId country, FocusIndex focus) {
    const FocusDef* def = g.content.focus(focus);
    if (!def || def->bypass.is_null()) return false;
    const Country* c = g.world.country(country);
    if (!c || !c->alive || is_completed(*c, focus)) return false;
    return eval_trigger(country_scope(const_cast<Game*>(&g), country), def->bypass);
}

// ---------------------------------------------------------------- selection ---

bool focus_select(Game& g, CountryId country, FocusIndex focus) {
    Country* c = g.world.country(country);
    if (!c || !c->alive) return false;
    const FocusDef* def = g.content.focus(focus);
    if (!def) return false;
    // Selecting the focus already in progress is a no-op (progress survives).
    if (c->selected_focus == focus) return true;
    if (!focus_available(g, country, focus)) return false;

    c->selected_focus = focus;
    c->focus_progress = 0.0;
    // A bypassed focus completes at once, but grants none of its effects.
    if (focus_bypassable(g, country, focus)) {
        finish_focus(g, *c, focus, *def, /*apply=*/false, /*bypassed=*/true);
    }
    return true;
}

void focus_cancel(Game& g, CountryId country) {
    Country* c = g.world.country(country);
    if (!c) return;
    c->selected_focus = INVALID_FOCUS;
    c->focus_progress = 0.0;
}

bool focus_complete(Game& g, CountryId country, FocusIndex focus) {
    Country* c = g.world.country(country);
    if (!c || !c->alive) return false;
    const FocusDef* def = g.content.focus(focus);
    if (!def) return false;
    if (is_completed(*c, focus)) return false;
    finish_focus(g, *c, focus, *def, /*apply=*/true, /*bypassed=*/false);
    return true;
}

// ------------------------------------------------------------- daily phase ----

void phase_focuses(Game& g) {
    World& w = g.world;
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) != 0) return;

    const double speed = g.content.constants.focus_progress_speed;
    w.countries.for_each([&](CountryId, Country& c) {
        if (!c.alive) return;

        // 1. Expire timed modifiers (permanent ones use days_left < 0). Done before
        // focus progress so a modifier granted on completion starts counting tomorrow.
        for (size_t i = 0; i < c.timed_modifiers.size();) {
            TimedModifier& tm = c.timed_modifiers[i];
            if (tm.days_left > 0) --tm.days_left;
            if (tm.days_left == 0) {
                c.timed_modifiers.erase(c.timed_modifiers.begin() +
                                        static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        // 2. One day of progress for the selected focus, then completion.
        if (c.selected_focus != INVALID_FOCUS) {
            const FocusDef* def = g.content.focus(c.selected_focus);
            if (!def) {
                // Content changed under a save: drop the dangling selection.
                c.selected_focus = INVALID_FOCUS;
                c.focus_progress = 0.0;
            } else {
                c.focus_progress += speed;
                if (c.focus_progress >= def->days) {
                    const FocusIndex done = c.selected_focus;
                    finish_focus(g, c, done, *def, /*apply=*/true, /*bypassed=*/false);
                }
            }
        }
    });
}

// --------------------------------------------------------------------- AI -----

void ai_focus_layer(Game& g, Country& c) {
    if (!c.alive) return;
    const CountryId country = c.id;

    // A valid selection already advances on its own; only act when there is none or
    // the current one can no longer be worked on.
    if (c.selected_focus != INVALID_FOCUS && focus_available(g, country, c.selected_focus)) {
        return;
    }

    const FocusPosture posture = focus_posture(g, country);

    double best_score = 0.0;
    FocusIndex best = INVALID_FOCUS;
    AiLayerState& layer = g.ai.layer(AiLayer::Politics);
    std::vector<std::pair<std::string, double>> factors;

    for (FocusIndex i = 0; i < static_cast<FocusIndex>(g.content.focuses.size()); ++i) {
        if (!focus_available(g, country, i)) continue;
        const FocusDef& def = g.content.focuses[i];
        factors.clear();
        double score = def.ai_weight;
        factors.emplace_back("ai_weight", def.ai_weight);
        score += score_effects(def, posture, &factors);
        if (!std::isfinite(score)) score = 0.0;

        if (layer.last_reasons.size() < kMaxFocusReasons) {
            AiReason reason;
            reason.what = "focus:" + def.key;
            reason.score = score;
            reason.factors = factors;
            layer.last_reasons.push_back(std::move(reason));
        }
        // Strict comparison keeps the lowest focus index on a tie (deterministic).
        if (best == INVALID_FOCUS || score > best_score) {
            best = i;
            best_score = score;
        }
    }

    if (best == INVALID_FOCUS) return;
    const FocusDef& def = g.content.focuses[best];

    Command cmd;
    cmd.type = CommandType::SelectFocus;
    cmd.country = country;
    cmd.issued_tick = g.world.tick;
    cmd.text = def.key;
    // The AI acts only through commands a human could issue; a rejected command is
    // never queued.
    if (validate_command(g, cmd) != CommandResult::Applied) return;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
}

// ----------------------------------------------------------------- tooltips ---

std::string focus_status_text(const Game& g, CountryId country, FocusIndex focus) {
    const FocusDef* def = g.content.focus(focus);
    if (!def) return "Focus <unknown>";

    const std::string name = def->name.empty() ? def->key : def->name;
    const Country* c = g.world.country(country);

    size_t total = 0;
    size_t owned = 0;
    for (const std::string& key : def->prerequisites) {
        ++total;
        const FocusIndex prereq = g.content.focus_id(key);
        if (prereq != INVALID_FOCUS && c && is_completed(*c, prereq)) ++owned;
    }

    char buf[512];
    if (c && is_completed(*c, focus)) {
        std::snprintf(buf, sizeof(buf), "Focus '%s': completed (%zu/%zu prerequisites complete)",
                      name.c_str(), owned, total);
        return std::string(buf);
    }
    const double progress = (c && c->selected_focus == focus) ? c->focus_progress : 0.0;
    const int progress_days = static_cast<int>(progress < 0.0 ? 0.0 : progress);
    const int required_days = static_cast<int>(def->days < 0.0 ? 0.0 : def->days);
    std::snprintf(buf, sizeof(buf),
                  "Focus '%s': %d/%d days (%zu/%zu prerequisites complete)", name.c_str(),
                  progress_days, required_days, owned, total);
    return std::string(buf);
}

}  // namespace hoi