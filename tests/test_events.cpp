// Events and decisions: hand-built worlds only (no scenario files), so every check
// points at the lifecycle rule under test - automatic firing, fire_only_once,
// delayed scheduling, option effects, decision visibility/cost/cooldown/timers, the
// AI acting through commands, and determinism.

#include <string>
#include <vector>

#include "core/json.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/events.h"
#include "sim/world.h"
#include "test.h"
#include "test_util.h"

using namespace hoi;
using namespace hoi_test;

namespace {

Json obj() { return Json::object(); }

// Effect block with one numeric key: {"add_political_power": v}.
Json effect_num(const char* key, double v) {
    Json j = Json::object();
    j.set(key, Json(v));
    return j;
}

Json compare(const char* op, double v) {
    Json j = Json::object();
    j.set(op, Json(v));
    return j;
}

// Trigger of the shape {"<key>": {"<op>": v}}.
Json trig_cmp(const char* key, const char* op, double v) {
    Json j = Json::object();
    j.set(key, compare(op, v));
    return j;
}

// Trigger of the shape {"<key>": v} (scalar values such as `chance`).
Json trig_num(const char* key, double v) {
    Json j = Json::object();
    j.set(key, Json(v));
    return j;
}

EventOptionDef option(const std::string& name, double ai_weight, Json effects) {
    EventOptionDef o;
    o.name = name;
    o.ai_weight = ai_weight;
    o.effects = std::move(effects);
    return o;
}

uint32_t add_event(Content& c, const std::string& key, bool only_once, Json trigger,
                   Json immediate, std::vector<EventOptionDef> options) {
    EventDef def;
    def.index = static_cast<uint32_t>(c.events.size());
    def.key = key;
    def.title = key;
    def.fire_only_once = only_once;
    def.trigger = std::move(trigger);
    def.immediate = std::move(immediate);
    def.options = std::move(options);
    c.event_index[key] = def.index;
    c.events.push_back(std::move(def));
    return c.events.back().index;
}

uint32_t add_decision(Content& c, const std::string& key, double cost_pp, int days_remove,
                      int days_cooldown, Json visible, Json available, Json effects,
                      Json remove_effect, double ai_weight = 1.0) {
    DecisionDef def;
    def.index = static_cast<uint32_t>(c.decisions.size());
    def.key = key;
    def.name = key;
    def.cost_pp = cost_pp;
    def.days_remove = days_remove;
    def.days_cooldown = days_cooldown;
    def.visible = std::move(visible);
    def.available = std::move(available);
    def.effects = std::move(effects);
    def.remove_effect = std::move(remove_effect);
    def.ai_weight = ai_weight;
    c.decision_index[key] = def.index;
    c.decisions.push_back(std::move(def));
    return c.decisions.back().index;
}

bool has_pending(const Country& c, uint32_t event) {
    for (uint32_t e : c.pending_events)
        if (e == event) return true;
    return false;
}

bool has_active(const Country& c, uint32_t decision) {
    for (uint32_t d : c.active_decisions)
        if (d == decision) return true;
    return false;
}

// Cooldowns are stored per decision index and grown on demand; a decision that was
// never cooldown-stamped has no entry and reads as zero.
double cooldown_days(const Country& c, uint32_t decision) {
    return decision < c.decision_cooldown.size() ? c.decision_cooldown[decision] : 0.0;
}

size_t count_log_kind(const Game& g, const std::string& kind) {
    size_t n = 0;
    for (const SimEvent& e : g.events)
        if (e.kind == kind) ++n;
    return n;
}

std::string log_texts(const Game& g) {
    std::string out;
    for (const SimEvent& e : g.events) out += e.kind + "|" + e.text + "\n";
    return out;
}

// Advances the world clock to the given day (tick = day * 24), keeping the phase's
// daily gate satisfied.
void set_day(Game& g, uint64_t day) { g.world.tick = day * static_cast<uint64_t>(TICKS_PER_DAY); }

}  // namespace

// An automatic event fires for every country whose trigger passes, applies its
// immediate effects on arrival, and lands in the pending list for a choice.
HOI_TEST(events_automatic_firing_and_immediate_effects) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t ev =
        add_event(g.content, "auto_event", /*only_once=*/true,
                  trig_cmp("political_power", "gte", 100.0),
                  effect_num("add_political_power", 10.0), {option("ok", 1.0, obj())});

    Country* a = g.world.country(m.a);
    Country* b = g.world.country(m.b);
    b->political_power = 0.0;  // the second country does not qualify
    const double pp_a = a->political_power;

    phase_events(g);

    CHECK(has_pending(*a, ev));
    CHECK(!has_pending(*b, ev));
    CHECK_NEAR(a->political_power, pp_a + 10.0, 1e-9);
    CHECK_EQ(count_log_kind(g, "event"), 1u);

    // The event is already pending: a second daily sweep does not duplicate it and
    // does not apply the immediate effects again.
    phase_events(g);
    CHECK_EQ(a->pending_events.size(), 1u);
    CHECK_NEAR(a->political_power, pp_a + 10.0, 1e-9);
}

// A fire_only_once event never returns once it has been received; a repeatable one
// fires again on the next day.
HOI_TEST(events_fire_only_once_and_repeatable) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t once =
        add_event(g.content, "once_event", /*only_once=*/true,
                  trig_cmp("political_power", "gte", 0.0), obj(), {option("a", 1.0, obj())});
    const uint32_t repeat =
        add_event(g.content, "repeat_event", /*only_once=*/false,
                  trig_cmp("political_power", "gte", 0.0), obj(), {option("a", 1.0, obj())});

    Country* a = g.world.country(m.a);
    Country* b = g.world.country(m.b);
    a->political_power = 500.0;
    b->political_power = 0.0;  // keep the second country out of the way
    CHECK(!has_pending(*a, once) && !has_pending(*a, repeat));

    // Day 0: the lowest-index automatic event only.
    phase_events(g);
    CHECK(has_pending(*a, once));
    CHECK(!has_pending(*a, repeat));
    CHECK(choose_event_option(g, m.a, once, 0));
    CHECK(!has_pending(*a, once));

    // Day 1: the spent once-only event is skipped, so the repeatable one fires.
    set_day(g, 1);
    phase_events(g);
    CHECK(!has_pending(*a, once));
    CHECK(has_pending(*a, repeat));
    CHECK(choose_event_option(g, m.a, repeat, 0));

    // Day 2: the repeatable event fires again; the once-only one stays spent.
    set_day(g, 2);
    phase_events(g);
    CHECK(!has_pending(*a, once));
    CHECK(has_pending(*a, repeat));
    for (uint32_t e : a->fired_events) CHECK(e != repeat);  // only scalar events are tracked
}

// A delayed event waits its full delay, fires on the day it is due, and a second
// country's identically-due event is fired in ascending country order.
HOI_TEST(events_delayed_firing) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t ev =
        add_event(g.content, "delayed_event", /*only_once=*/true, Json(),
                  effect_num("add_political_power", 5.0), {option("a", 1.0, obj())});

    fire_event_delayed(g, m.a, ev, 7);
    CHECK_EQ(g.world.delayed_events.size(), 1u);
    CHECK_EQ(g.world.delayed_events[0].due, 7u * static_cast<uint64_t>(TICKS_PER_DAY));

    Country* a = g.world.country(m.a);
    const double pp = a->political_power;

    phase_events(g);  // day 0: not due yet
    CHECK(!has_pending(*a, ev));
    CHECK_NEAR(a->political_power, pp, 1e-9);

    set_day(g, 6);
    phase_events(g);
    CHECK(!has_pending(*a, ev));

    set_day(g, 7);
    phase_events(g);
    CHECK(has_pending(*a, ev));
    CHECK_NEAR(a->political_power, pp + 5.0, 1e-9);
    CHECK(g.world.delayed_events.empty());

    // Ordering: same due tick, different countries -> ascending country id, and the
    // order is visible in the log.
    MiniWorld m2 = make_mini_world();
    Game& g2 = m2.game;
    const uint32_t ea =
        add_event(g2.content, "order_a", true, Json(), obj(), {option("a", 1.0, obj())});
    const uint32_t eb =
        add_event(g2.content, "order_b", true, Json(), obj(), {option("a", 1.0, obj())});
    fire_event_delayed(g2, m2.b, eb, 1);
    fire_event_delayed(g2, m2.a, ea, 1);
    set_day(g2, 1);
    phase_events(g2);
    size_t index_a = g2.events.size();
    size_t index_b = g2.events.size();
    for (size_t i = 0; i < g2.events.size(); ++i) {
        if (g2.events[i].country == m2.a) index_a = i;
        if (g2.events[i].country == m2.b) index_b = i;
    }
    CHECK_LT(index_a, index_b);
}

// Choosing an option applies that option's effects and resolves the event; an
// out-of-range index or a non-pending event changes nothing.
HOI_TEST(events_choose_option_applies_effects_and_validates) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t ev = add_event(
        g.content, "choice_event", /*only_once=*/true, Json(), obj(),
        {option("calm", 1.0, effect_num("add_stability", 0.10)),
         option("bold", 1.0, effect_num("add_war_support", 0.20))});
    Country* a = g.world.country(m.a);
    const double stab = a->stability;
    const double ws = a->war_support;

    fire_event(g, m.a, ev);
    CHECK(has_pending(*a, ev));
    CHECK(!choose_event_option(g, m.a, ev, 99));  // out of range
    CHECK(!choose_event_option(g, m.a, ev, -1));
    CHECK_NEAR(a->stability, stab, 1e-9);
    CHECK(has_pending(*a, ev));

    CHECK(choose_event_option(g, m.a, ev, 1));
    CHECK(!has_pending(*a, ev));
    CHECK_NEAR(a->war_support, ws + 0.20, 1e-9);
    CHECK_NEAR(a->stability, stab, 1e-9);

    // No longer pending: choosing again is refused and applies nothing.
    const double after = a->war_support;
    CHECK(!choose_event_option(g, m.a, ev, 0));
    CHECK_NEAR(a->war_support, after, 1e-9);
    // A different country never had it pending.
    CHECK(!choose_event_option(g, m.b, ev, 0));
}

// Decisions: visibility is a pure trigger, availability adds cost, active state and
// cooldown; taking deducts political power, applies effects and starts the timer;
// the timer applies the remove-effect and starts the cooldown.
HOI_TEST(decisions_visibility_cost_timer_and_cooldown) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t d = add_decision(
        g.content, "rearm", /*cost_pp=*/100.0, /*days_remove=*/3, /*days_cooldown=*/5,
        trig_cmp("political_power", "gte", 10.0), trig_cmp("stability", "gte", 0.30),
        effect_num("add_war_support", 0.10), effect_num("add_stability", 0.05));
    // Visible but never available: its `available` trigger cannot pass.
    const uint32_t locked = add_decision(
        g.content, "locked", 0.0, 0, 0, trig_cmp("political_power", "gte", 10.0),
        trig_cmp("stability", "gte", 0.99), obj(), obj());
    // Visible but too expensive.
    const uint32_t pricey = add_decision(
        g.content, "pricey", 1000.0, 0, 0, trig_cmp("political_power", "gte", 10.0),
        trig_cmp("stability", "gte", 0.30), obj(), obj());

    Country* a = g.world.country(m.a);
    a->political_power = 500.0;
    a->stability = 0.5;

    CHECK(decision_visible(g, m.a, d));
    CHECK(decision_available(g, m.a, d));
    CHECK(decision_visible(g, m.a, locked));  // visible ignores availability
    CHECK(!decision_available(g, m.a, locked));
    CHECK(decision_visible(g, m.a, pricey));
    CHECK(!decision_available(g, m.a, pricey));

    // Too poor to take it, whatever the triggers say.
    a->political_power = 50.0;
    CHECK(decision_visible(g, m.a, d));
    CHECK(!decision_available(g, m.a, d));
    CHECK(!decision_take(g, m.a, d));
    a->political_power = 500.0;

    const double ws = a->war_support;
    CHECK(decision_take(g, m.a, d));
    CHECK_NEAR(a->political_power, 400.0, 1e-9);
    CHECK_NEAR(a->war_support, ws + 0.10, 1e-9);
    CHECK(has_active(*a, d));
    CHECK_EQ(a->active_decisions.size(), a->decision_days_left.size());
    CHECK_NEAR(a->decision_days_left[0], 3.0, 1e-9);
    CHECK(!decision_available(g, m.a, d));  // already active
    CHECK(!decision_take(g, m.a, d));       // a second take is refused

    const double stab = a->stability;
    set_day(g, 1);
    phase_events(g);
    CHECK_NEAR(a->decision_days_left[0], 2.0, 1e-9);
    set_day(g, 2);
    phase_events(g);
    CHECK_NEAR(a->decision_days_left[0], 1.0, 1e-9);
    set_day(g, 3);
    phase_events(g);
    CHECK(!has_active(*a, d));
    CHECK_NEAR(a->stability, stab + 0.05, 1e-9);  // remove-effect applied
    CHECK_NEAR(cooldown_days(*a, d), 5.0, 1e-9);
    CHECK(!decision_available(g, m.a, d));  // on cooldown

    for (int day = 4; day <= 8; ++day) {
        set_day(g, static_cast<uint64_t>(day));
        phase_events(g);
    }
    CHECK_NEAR(cooldown_days(*a, d), 0.0, 1e-9);
    CHECK(decision_available(g, m.a, d));  // cooldown elapsed: takeable again
}

// A permanent decision stays taken until cancelled, and cancelling applies the
// remove-effect without starting a cooldown.
HOI_TEST(decisions_permanent_and_cancel) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t d = add_decision(
        g.content, "standing_policy", 50.0, /*days_remove=*/0, /*days_cooldown=*/4,
        trig_cmp("political_power", "gte", 0.0), trig_cmp("stability", "gte", 0.0), obj(),
        effect_num("add_stability", 0.02));
    Country* a = g.world.country(m.a);
    a->political_power = 500.0;
    CHECK(decision_take(g, m.a, d));

    // Many days pass: a permanent decision is never removed by the timer.
    for (int day = 1; day <= 10; ++day) {
        set_day(g, static_cast<uint64_t>(day));
        phase_events(g);
    }
    CHECK(has_active(*a, d));

    const double stab = a->stability;
    decision_cancel(g, m.a, d);
    CHECK(!has_active(*a, d));
    CHECK_NEAR(a->stability, stab + 0.02, 1e-9);
    CHECK_NEAR(cooldown_days(*a, d), 0.0, 1e-9);  // cancel starts no cooldown
    CHECK(decision_available(g, m.a, d));

    // Cancelling something that is not taken changes nothing.
    decision_cancel(g, m.a, d);
    CHECK_NEAR(a->stability, stab + 0.02, 1e-9);
}

// The AI answers pending events by pushing ChooseEventOption, records an AiReason
// in the politics layer, and the command path applies the chosen option.
HOI_TEST(events_ai_chooses_option_through_command) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t ev = add_event(
        g.content, "ai_choice", /*only_once=*/true, Json(), obj(),
        {option("cheap", 1.0, effect_num("add_stability", 0.01)),
         option("desired", 5.0, effect_num("add_manpower", 1234.0))});
    Country* a = g.world.country(m.a);
    const double manpower = a->manpower;
    const double stability = a->stability;

    fire_event(g, m.a, ev);
    g.queue.clear();
    g.ai.layer(AiLayer::Politics).last_reasons.clear();

    ai_event_layer(g, *a);
    CHECK_EQ(g.queue.pending.size(), 1u);
    CHECK(g.queue.pending[0].type == CommandType::ChooseEventOption);
    CHECK_EQ(g.queue.pending[0].text, std::string("ai_choice"));
    CHECK_EQ(g.queue.pending[0].value, 1);  // the higher-weight option
    CHECK_EQ(g.ai.layer(AiLayer::Politics).last_reasons.size(), 1u);
    CHECK(g.ai.layer(AiLayer::Politics).last_reasons[0].what == "event:ai_choice");
    CHECK_GT(g.ai.layer(AiLayer::Politics).last_reasons[0].score, 0.0);

    phase_commands(g);
    CHECK(!has_pending(*a, ev));
    CHECK_NEAR(a->manpower, manpower + 1234.0, 1e-9);
    CHECK_NEAR(a->stability, stability, 1e-9);  // the rejected option changed nothing
    CHECK_EQ(g.queue.pending.size(), 0u);
}

// The AI takes the best available decision through TakeDecision once political
// power is comfortable, and skips it while it is poor.
HOI_TEST(decisions_ai_takes_best_through_command) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t small = add_decision(
        g.content, "small_step", 50.0, 0, 0, trig_cmp("political_power", "gte", 0.0),
        trig_cmp("stability", "gte", 0.0), effect_num("add_stability", 0.01), obj(), 1.0);
    const uint32_t big = add_decision(
        g.content, "grand_plan", 100.0, 0, 0, trig_cmp("political_power", "gte", 0.0),
        trig_cmp("stability", "gte", 0.0), effect_num("add_stability", 0.02), obj(), 4.0);
    Country* a = g.world.country(m.a);
    g.ai.layer(AiLayer::Politics).last_reasons.clear();

    // Poor: the AI answers nothing.
    a->political_power = 60.0;
    ai_decision_layer(g, *a);
    CHECK(g.queue.pending.empty());

    // Comfortable: the highest-weight decision is pushed as a command and applies.
    a->political_power = 500.0;
    ai_decision_layer(g, *a);
    CHECK_EQ(g.queue.pending.size(), 1u);
    CHECK(g.queue.pending[0].type == CommandType::TakeDecision);
    CHECK_EQ(g.queue.pending[0].text, std::string("grand_plan"));
    CHECK_EQ(g.ai.layer(AiLayer::Politics).last_reasons.size(), 1u);
    CHECK(g.ai.layer(AiLayer::Politics).last_reasons[0].what == "decision:grand_plan");

    phase_commands(g);
    CHECK(has_active(*a, big));
    CHECK(!has_active(*a, small));
    CHECK_NEAR(a->political_power, 400.0, 1e-9);
}

// Determinism: identical worlds, identical seed, identical outcome - pending lists,
// RNG consumption and the log all match, including a `chance` trigger.
HOI_TEST(events_deterministic_across_identical_runs) {
    auto run = [](std::string* log_out, uint64_t* rng_hash, std::vector<uint32_t>* pending) {
        MiniWorld m = make_mini_world();
        Game& g = m.game;
        add_event(g.content, "risky", true, trig_num("chance", 0.5), obj(),
                  {option("a", 1.0, obj())});
        add_event(g.content, "risky_two", true, trig_num("chance", 0.8), obj(),
                  {option("a", 1.0, obj())});
        g.world.country(m.a)->political_power = 500.0;
        g.world.country(m.b)->political_power = 500.0;
        g.rng.seed(987654321ull);
        set_day(g, 1);
        phase_events(g);
        *log_out = log_texts(g);
        *rng_hash = g.rng.state_hash();
        *pending = g.world.country(m.a)->pending_events;
    };

    std::string log_a, log_b;
    uint64_t hash_a = 0, hash_b = 0;
    std::vector<uint32_t> pending_a, pending_b;
    run(&log_a, &hash_a, &pending_a);
    run(&log_b, &hash_b, &pending_b);

    CHECK(log_a == log_b);
    CHECK_EQ(hash_a, hash_b);
    CHECK(pending_a == pending_b);
    CHECK(hash_a != 0u);
}