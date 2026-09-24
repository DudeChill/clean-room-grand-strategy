// Events and decisions (spec sections 58, 59).
//
// An event is data: an optional automatic trigger, an `immediate` effect block, and
// one or more options whose effects the receiving country chooses (a human by
// command, the AI by option weight). A decision is data too: visibility and
// availability triggers, a political-power cost, an effect block, an optional
// remove-effect and two timers (how long it stays taken, how long before it can be
// taken again).
//
// Everything the script layer can express lives in the JSON blocks; this file owns
// only the lifecycle - scheduling, the pending list, the per-day trigger sweep and
// the timers - and never interprets content by hand. All effects and triggers go
// through src/sim/script.h, so content never needs engine changes.
//
// Determinism: countries are walked in ascending id (Store::for_each), events and
// decisions in ascending content index, due delayed events in (due, country, event)
// order. No unordered container is iterated and no state is ordered by pointer, so
// two runs over the same world produce the same pending list, the same log and the
// same RNG consumption (chance triggers draw from RNG_EVENTS in a fixed order).

#include "sim/events.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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

// The AI reason log is capped the same way the other layers cap theirs (ai.cpp):
// once full, a new reason replaces the weakest one so the debugger shows the
// decisions that actually drove the country, not the first ones made.
constexpr size_t kMaxReasons = 16;

// Political power the AI keeps in reserve before it is "comfortable" spending on a
// decision. Below this it still answers events (which cost nothing) but defers
// decisions until the treasury recovers.
constexpr double kDecisionPowerReserve = 75.0;

// Situation weights shared by the event and decision scorers. They are small,
// documented nudges on top of the content's `ai_weight`: a country at war answers
// events and takes decisions a little more readily, one whose government is shaky
// does too, and one that is poor is slightly more conservative.
constexpr double kWarBoost = 1.15;
constexpr double kInstabilityBoost = 1.10;
constexpr double kStableDamp = 0.95;
constexpr double kPoorPowerDamp = 0.90;
constexpr double kLowStability = 0.40;
constexpr double kHighStability = 0.75;

struct Situation {
    double war = 1.0;
    double stability = 1.0;
    double power = 1.0;
};

Situation situation_of(const Country& c) {
    Situation s;
    s.war = c.at_war ? kWarBoost : 1.0;
    s.stability = c.stability < kLowStability
                      ? kInstabilityBoost
                      : (c.stability > kHighStability ? kStableDamp : 1.0);
    s.power = c.political_power < kDecisionPowerReserve ? kPoorPowerDamp : 1.0;
    return s;
}

bool contains(const std::vector<uint32_t>& ids, uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool is_pending(const Country& c, uint32_t event) {
    return contains(c.pending_events, event);
}

bool is_active(const Country& c, uint32_t decision) {
    return contains(c.active_decisions, decision);
}

// Cooldowns are stored per decision index and grown on demand: a decision that has
// never been taken has no entry and is therefore not on cooldown.
double cooldown_of(const Country& c, uint32_t decision) {
    return decision < c.decision_cooldown.size() ? c.decision_cooldown[decision] : 0.0;
}

void start_cooldown(Country& c, uint32_t decision, int days) {
    if (c.decision_cooldown.size() <= decision) c.decision_cooldown.resize(decision + 1, 0.0);
    c.decision_cooldown[decision] = static_cast<double>(days > 0 ? days : 0);
}

ScriptScope country_scope(Game& g, CountryId country) {
    ScriptScope scope;
    scope.game = &g;
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

// Every AI choice is a Command, checked by the command system before it is queued -
// the same path a human player's order takes, and the same guarantee the other AI
// layers give (ai.cpp): a command the AI knows is illegal is never queued.
bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

const std::string& event_label(const EventDef& def) {
    return def.title.empty() ? def.key : def.title;
}

bool due_before(const DelayedEvent& a, const DelayedEvent& b) {
    if (a.due != b.due) return a.due < b.due;
    if (a.country != b.country) return a.country.v < b.country.v;
    return a.event < b.event;
}

// Fires every delayed event whose due tick has arrived, oldest first. The pending
// (not-yet-due) entries are kept in the same order they were scheduled.
void fire_due_delayed(Game& g) {
    World& w = g.world;
    if (w.delayed_events.empty()) return;

    std::vector<DelayedEvent> due;
    std::vector<DelayedEvent> keep;
    due.reserve(w.delayed_events.size());
    keep.reserve(w.delayed_events.size());
    for (const DelayedEvent& de : w.delayed_events) {
        if (de.due <= w.tick) {
            due.push_back(de);
        } else {
            keep.push_back(de);
        }
    }
    if (due.empty()) return;

    // Firing may schedule further events into w.delayed_events (which now holds the
    // not-yet-due entries), so the due list is detached before it is walked.
    w.delayed_events.swap(keep);
    std::sort(due.begin(), due.end(), due_before);
    for (const DelayedEvent& de : due) {
        fire_event(g, de.country, de.event);
    }
}

// Automatic firing: for every alive country, the lowest-index event whose trigger
// passes fires, and the country fires at most one automatic event that day. The
// cap keeps the pending list and the log readable when several events' triggers
// overlap; a lower index is the tie-break, so content order is priority order.
void fire_automatic(Game& g) {
    g.world.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive) return;
        for (uint32_t i = 0; i < g.content.events.size(); ++i) {
            const EventDef& def = g.content.events[i];
            if (def.trigger.is_null()) continue;  // not an automatic event
            if (def.fire_only_once && contains(c.fired_events, i)) continue;
            if (is_pending(c, i)) continue;  // already awaiting a choice
            if (!eval_trigger(country_scope(g, id), def.trigger)) continue;
            fire_event(g, id, i);
            break;  // one automatic event per country per day
        }
    });
}

// Decision timers. Cooldowns tick first, then expiring decisions apply their
// remove-effect and start their cooldown, so a cooldown set today is not consumed
// by today's tick.
void tick_decisions(Game& g) {
    g.world.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive) return;
        for (double& cd : c.decision_cooldown) {
            if (cd > 0.0) cd = std::max(0.0, cd - 1.0);
        }
        if (c.decision_days_left.size() < c.active_decisions.size()) {
            c.decision_days_left.resize(c.active_decisions.size(), -1.0);
        }
        size_t keep = 0;
        for (size_t i = 0; i < c.active_decisions.size(); ++i) {
            const uint32_t idx = c.active_decisions[i];
            double left = c.decision_days_left[i];
            const DecisionDef* def = g.content.decision(idx);
            bool removed = false;
            if (def == nullptr) {
                removed = true;  // stale entry (content changed under a live save)
            } else if (def->days_remove > 0 && left >= 0.0) {
                left -= 1.0;
                if (left <= 0.0) {
                    apply_effects(country_scope(g, id), def->remove_effect);
                    start_cooldown(c, idx, def->days_cooldown);
                    g.log_event("decision",
                                c.tag + " decision '" + def->key + "' expired", id);
                    removed = true;
                }
            }
            if (!removed) {
                c.active_decisions[keep] = idx;
                c.decision_days_left[keep] = left;
                ++keep;
            }
        }
        c.active_decisions.resize(keep);
        c.decision_days_left.resize(keep);
    });
}

}  // namespace

// ------------------------------------------------------------------- events --

void fire_event(Game& g, CountryId country, EventIndex event) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return;
    const EventDef* def = g.content.event(event);
    if (def == nullptr) return;
    if (def->fire_only_once && contains(c->fired_events, event)) return;
    if (is_pending(*c, event)) return;  // never two copies of the same event pending
    if (def->fire_only_once) c->fired_events.push_back(event);

    const ScriptScope scope = country_scope(g, country);
    apply_effects(scope, def->immediate);

    const std::string& label = event_label(*def);
    // An event with no options is a notification: its immediate effects are the
    // whole content and it has nothing to answer, so it never enters the pending
    // list (a pending choice that can never be made would block re-firing forever).
    if (!def->options.empty()) c->pending_events.push_back(event);
    g.log_event("event", c->tag + " received event: " + label, country);

    // A major event pauses the game for a human player. The engine only records
    // that fact; the client decides how to present it (usually a blocking popup).
    if (def->major && g.player_country.valid() && country == g.player_country) {
        g.log_event("event", "major event '" + label + "' awaits the player's decision",
                    country);
    }
}

void fire_event_delayed(Game& g, CountryId country, EventIndex event, int days) {
    if (g.content.event(event) == nullptr) return;
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return;
    DelayedEvent de;
    de.country = country;
    de.event = event;
    const Tick delay = static_cast<Tick>(days > 0 ? days : 0) *
                       static_cast<Tick>(TICKS_PER_DAY);
    de.due = g.world.tick + delay;
    g.world.delayed_events.push_back(de);
}

bool fire_event_by_key(Game& g, CountryId country, const std::string& key, int days) {
    const uint32_t event = g.content.event_id(key);
    if (event == 0xFFFFFFFFu) return false;
    if (days <= 0) {
        fire_event(g, country, event);
    } else {
        fire_event_delayed(g, country, event, days);
    }
    return true;
}

bool choose_event_option(Game& g, CountryId country, EventIndex event, int option) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const EventDef* def = g.content.event(event);
    if (def == nullptr) return false;
    if (option < 0 || static_cast<size_t>(option) >= def->options.size()) return false;

    // The same event can only be pending once, so the first match is the instance.
    auto it = std::find(c->pending_events.begin(), c->pending_events.end(), event);
    if (it == c->pending_events.end()) return false;

    const EventOptionDef& opt = def->options[static_cast<size_t>(option)];
    apply_effects(country_scope(g, country), opt.effects);
    c->pending_events.erase(it);
    const std::string name = opt.name.empty() ? std::to_string(option) : opt.name;
    g.log_event("event", c->tag + " chose '" + name + "' for event " + def->key, country);
    return true;
}

// ---------------------------------------------------------------- decisions --

bool decision_visible(const Game& g, CountryId country, DecisionIndex decision) {
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const DecisionDef* def = g.content.decision(decision);
    if (def == nullptr) return false;
    return eval_trigger(const_country_scope(g, country), def->visible);
}

bool decision_available(const Game& g, CountryId country, DecisionIndex decision) {
    if (!decision_visible(g, country, decision)) return false;
    const Country* c = g.world.country(country);
    const DecisionDef* def = g.content.decision(decision);
    if (c == nullptr || def == nullptr) return false;
    if (is_active(*c, decision)) return false;
    if (cooldown_of(*c, decision) > 0.0) return false;
    if (c->political_power < def->cost_pp) return false;
    return eval_trigger(const_country_scope(g, country), def->available);
}

bool decision_take(Game& g, CountryId country, DecisionIndex decision) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const DecisionDef* def = g.content.decision(decision);
    if (def == nullptr) return false;
    if (!decision_available(g, country, decision)) return false;

    c->political_power = std::max(0.0, c->political_power - def->cost_pp);
    apply_effects(country_scope(g, country), def->effects);
    c->active_decisions.push_back(decision);
    c->decision_days_left.push_back(static_cast<double>(def->days_remove));
    g.log_event("decision", c->tag + " took decision '" + def->key + "'", country);
    return true;
}

void decision_cancel(Game& g, CountryId country, DecisionIndex decision) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return;
    const DecisionDef* def = g.content.decision(decision);
    auto it = std::find(c->active_decisions.begin(), c->active_decisions.end(), decision);
    if (it == c->active_decisions.end()) return;  // not taken: nothing to cancel

    const size_t pos = static_cast<size_t>(it - c->active_decisions.begin());
    c->active_decisions.erase(it);
    if (pos < c->decision_days_left.size()) {
        c->decision_days_left.erase(c->decision_days_left.begin() +
                                    static_cast<std::ptrdiff_t>(pos));
    }
    // Cancelling is the country's own choice, not the timer running out: the
    // remove-effect still applies, but no cooldown starts (only expiry sets one).
    if (def != nullptr) apply_effects(country_scope(g, country), def->remove_effect);
    const std::string key = def != nullptr ? def->key : "#" + std::to_string(decision);
    g.log_event("decision", c->tag + " cancelled decision '" + key + "'", country);
}

// -------------------------------------------------------------------- phase --

void phase_events(Game& g) {
    World& w = g.world;
    // Daily granularity: the phase is a no-op outside the first hour of a day
    // (same gate as phase_weather).
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) != 0) return;

    fire_due_delayed(g);
    fire_automatic(g);
    tick_decisions(g);
}

// ----------------------------------------------------------------------- AI --

// Answers every pending event: the option with the highest content weight, adjusted
// by the situation, wins; ties go to the lowest option index. One ChooseEventOption
// command per pending event, exactly as a human would issue them.
void ai_event_layer(Game& g, Country& c) {
    if (!c.alive) return;
    const Situation sit = situation_of(c);
    // Copy the pending list: the commands are applied next tick, so nothing here
    // may mutate it, but a defensive copy also keeps iteration stable.
    const std::vector<uint32_t> pending = c.pending_events;
    for (uint32_t idx : pending) {
        const EventDef* def = g.content.event(idx);
        if (def == nullptr || def->options.empty()) continue;
        size_t best = 0;
        double best_score = -1.0;
        for (size_t i = 0; i < def->options.size(); ++i) {
            const double base = def->options[i].ai_weight;
            const double score = base * sit.war * sit.stability * sit.power;
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        Command cmd;
        cmd.type = CommandType::ChooseEventOption;
        cmd.country = c.id;
        cmd.text = def->key;
        cmd.value = static_cast<int32_t>(best);
        if (!push_if_valid(g, std::move(cmd))) continue;
        AiReason reason;
        reason.what = "event:" + def->key;
        reason.score = best_score;
        reason.factors = {{"ai_weight", def->options[best].ai_weight},
                          {"war", sit.war},
                          {"stability", sit.stability},
                          {"political_power", sit.power}};
        record_reason(g, std::move(reason));
    }
}

// Takes the best-scoring available decision, but only while political power is
// comfortable (cost plus a reserve), so the AI does not spend its last points the
// day a focus or law needs them.
void ai_decision_layer(Game& g, Country& c) {
    if (!c.alive) return;
    const Situation sit = situation_of(c);
    size_t best = 0;
    double best_score = -1.0;
    bool found = false;
    for (uint32_t i = 0; i < g.content.decisions.size(); ++i) {
        if (!decision_available(g, c.id, i)) continue;
        const DecisionDef& def = g.content.decisions[i];
        const double score = def.ai_weight * sit.war * sit.stability * sit.power;
        if (!found || score > best_score) {
            found = true;
            best_score = score;
            best = i;
        }
    }
    if (!found) return;

    const DecisionDef& def = g.content.decisions[best];
    if (c.political_power < def.cost_pp + kDecisionPowerReserve) return;

    Command cmd;
    cmd.type = CommandType::TakeDecision;
    cmd.country = c.id;
    cmd.text = def.key;
    if (!push_if_valid(g, std::move(cmd))) return;
    AiReason reason;
    reason.what = "decision:" + def.key;
    reason.score = best_score;
    reason.factors = {{"ai_weight", def.ai_weight},
                      {"cost_pp", def.cost_pp},
                      {"political_power", c.political_power},
                      {"war", sit.war},
                      {"stability", sit.stability}};
    record_reason(g, std::move(reason));
}

}  // namespace hoi