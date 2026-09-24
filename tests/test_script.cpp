// Script engine tests: triggers, effects and their determinism.
//
// Hand-built worlds only (tests/test_util.h). Every trigger key is exercised in a
// true and a false case, every effect key is checked by an observable state change,
// and the two determinism properties of the engine - `chance` follows the seeded
// RngStream::Events stream, and a failing trigger never mutates state - are pinned.

#include <algorithm>
#include <string>
#include <vector>

#include "data/content.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/diplomacy.h"
#include "sim/research.h"
#include "sim/script.h"
#include "sim/units.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using hoi_test::MiniWorld;
using hoi_test::make_mini_world;

Json J(const std::string& text) {
    std::string err;
    Json j = Json::parse(text, &err);
    if (!err.empty()) hoi_test::fail(__FILE__, __LINE__, "json parse: " + err);
    return j;
}

ScriptScope scope_of(MiniWorld& m, CountryId c, StateId state = StateId{},
                     CountryId target = CountryId{}) {
    ScriptScope s;
    s.game = &m.game;
    s.country = c;
    s.state = state;
    s.target_country = target;
    return s;
}

bool trig(MiniWorld& m, CountryId c, const std::string& text, StateId state = StateId{}) {
    return eval_trigger(scope_of(m, c, state), J(text));
}

void fx(MiniWorld& m, CountryId c, const std::string& text, StateId state = StateId{}) {
    apply_effects(scope_of(m, c, state), J(text));
}

// --- content builders ---------------------------------------------------------

uint32_t add_focus(Content& c, const std::string& key, const std::string& effects,
                   const std::string& available = "null") {
    FocusDef f;
    f.index = static_cast<uint32_t>(c.focuses.size());
    f.key = key;
    f.name = key;
    f.tree = "shared";
    f.days = 10.0;
    f.available = J(available);
    f.effects = J(effects);
    c.focus_index[key] = f.index;
    c.focuses.push_back(f);
    return f.index;
}

uint32_t add_event(Content& c, const std::string& key, const std::string& immediate = "{}") {
    EventDef e;
    e.index = static_cast<uint32_t>(c.events.size());
    e.key = key;
    e.title = key;
    e.fire_only_once = false;
    e.trigger = Json();
    e.immediate = J(immediate);
    e.options.push_back(EventOptionDef{"ok", Json()});
    c.event_index[key] = e.index;
    c.events.push_back(e);
    return e.index;
}

uint32_t add_decision(Content& c, const std::string& key, const std::string& effects = "{}") {
    DecisionDef d;
    d.index = static_cast<uint32_t>(c.decisions.size());
    d.key = key;
    d.name = key;
    d.effects = J(effects);
    c.decision_index[key] = d.index;
    c.decisions.push_back(d);
    return d.index;
}

TechId add_tech(Content& c, const std::string& key, ModifierKind kind, double value) {
    TechDef t;
    t.id = TechId(static_cast<uint32_t>(c.techs.size()));
    t.key = key;
    t.name = key;
    t.category = "industry";
    t.modifiers.set(kind, value);
    c.tech_by_key[key] = t.id;
    c.techs.push_back(t);
    return t.id;
}

// ------------------------------------------------------------------- triggers --

HOI_TEST(script_trigger_country_numbers) {
    MiniWorld m = make_mini_world();
    Country& a = m.game.world.countries[m.a];
    a.political_power = 100.0;
    a.stability = 0.5;
    a.war_support = 0.6;
    a.manpower = 200000.0;
    a.fuel = 1000.0;
    hoi_test::add_division(m.game, m.a, m.provinces[0], m.tmpl);
    m.game.world.date = GameDate{1936, 1, 1, 0};

    CHECK(trig(m, m.a, R"({"political_power":{"gte":100}})"));
    CHECK(!trig(m, m.a, R"({"political_power":{"gt":100}})"));
    CHECK(trig(m, m.a, R"({"political_power":{"lte":100}})"));
    CHECK(!trig(m, m.a, R"({"political_power":{"lt":100}})"));
    CHECK(trig(m, m.a, R"({"political_power":{"eq":100}})"));
    CHECK(trig(m, m.a, R"({"political_power":100})"));  // bare number == eq

    CHECK(trig(m, m.a, R"({"stability":{"gte":0.5}})"));
    CHECK(!trig(m, m.a, R"({"stability":{"lt":0.5}})"));
    CHECK(trig(m, m.a, R"({"war_support":{"gt":0.5}})"));
    CHECK(!trig(m, m.a, R"({"war_support":{"lte":0.5}})"));
    CHECK(trig(m, m.a, R"({"manpower":{"gte":200000}})"));
    CHECK(!trig(m, m.a, R"({"manpower":{"gte":300000}})"));
    CHECK(trig(m, m.a, R"({"fuel":{"gte":1000}})"));
    CHECK(!trig(m, m.a, R"({"fuel":{"gt":1000}})"));
    CHECK(trig(m, m.a, R"({"factories":{"gte":10}})"));  // 5 civ + 5 mil in state_a
    CHECK(!trig(m, m.a, R"({"factories":{"gt":10}})"));
    CHECK(trig(m, m.a, R"({"num_divisions":{"gte":1}})"));
    CHECK(!trig(m, m.a, R"({"num_divisions":{"gte":2}})"));
    CHECK(trig(m, m.a, R"({"year":{"eq":1936}})"));
    CHECK(!trig(m, m.a, R"({"year":{"gte":1940}})"));

    m.game.world.date = GameDate{1937, 6, 1, 0};
    CHECK(trig(m, m.a, R"({"year":{"eq":1937}})"));

    // A string where a number is expected is a content error, never true.
    CHECK(!trig(m, m.a, R"({"political_power":"a lot"})"));
    CHECK(!trig(m, m.a, R"({"political_power":{"gte":"50"}})"));
}

HOI_TEST(script_trigger_identity_and_state) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    Country& a = g.world.countries[m.a];
    a.ideology = Ideology::Communist;

    g.set_ai(m.a, true);
    CHECK(trig(m, m.a, R"({"is_ai":true})"));
    CHECK(!trig(m, m.a, R"({"is_ai":false})"));
    g.set_ai(m.a, false);
    CHECK(trig(m, m.a, R"({"is_ai":false})"));
    CHECK(!trig(m, m.a, R"({"is_ai":true})"));

    CHECK(!trig(m, m.a, R"({"at_war":true})"));
    CHECK(trig(m, m.a, R"({"at_war":false})"));

    CHECK(trig(m, m.a, R"({"ideology":"communist"})"));
    CHECK(!trig(m, m.a, R"({"ideology":"fascist"})"));

    CHECK(trig(m, m.a, R"({"owns_state":"state_a"})"));
    CHECK(!trig(m, m.a, R"({"owns_state":"state_b"})"));
    CHECK(trig(m, m.a, R"({"controls_state":"state_a"})"));
    CHECK(!trig(m, m.a, R"({"controls_state":"state_b"})"));
    CHECK(trig(m, m.a, R"({"owns_state":"s1"})"));   // map-key form -> StateId(0)
    CHECK(!trig(m, m.a, R"({"owns_state":"s9999"})"));

    // State-scoped triggers.
    CHECK(trig(m, m.a, R"({"state_controller_is_owner":true})", m.state_a));
    CHECK(!trig(m, m.a, R"({"state_controller_is_owner":false})", m.state_a));
    CHECK(!trig(m, m.a, R"({"state_controller_is_owner":true})"));  // no state scope
    CHECK(trig(m, m.a, R"({"state_factories":{"gte":10}})", m.state_a));
    CHECK(!trig(m, m.a, R"({"state_factories":{"gt":10}})", m.state_a));

    CHECK(!trig(m, m.a, R"({"state_has_flag":"free_port"})", m.state_a));
    g.world.states[m.state_a].flags.push_back("free_port");
    CHECK(trig(m, m.a, R"({"state_has_flag":"free_port"})", m.state_a));
    CHECK(!trig(m, m.a, R"({"state_has_flag":"free_port"})", m.state_b));

    // Flags, decisions, focuses, technologies, dates and variables.
    CHECK(!trig(m, m.a, R"({"has_flag":"x"})"));
    a.country_flags.push_back("x");
    CHECK(trig(m, m.a, R"({"has_flag":"x"})"));
    CHECK(trig(m, m.a, R"({"has_country_flag":"x"})"));
    CHECK(!trig(m, m.a, R"({"has_country_flag":"y"})"));

    const uint32_t dec = add_decision(g.content, "dec_a");
    CHECK(!trig(m, m.a, R"({"has_decision":"dec_a"})"));
    a.active_decisions.push_back(dec);
    CHECK(trig(m, m.a, R"({"has_decision":"dec_a"})"));
    CHECK(!trig(m, m.a, R"({"has_decision":"dec_missing"})"));

    const uint32_t focus = add_focus(g.content, "focus_a", R"({"add_political_power":1})");
    CHECK(!trig(m, m.a, R"({"completed_focus":"focus_a"})"));
    a.completed_focuses.push_back(focus);
    CHECK(trig(m, m.a, R"({"completed_focus":"focus_a"})"));
    CHECK(!trig(m, m.a, R"({"completed_focus":"focus_missing"})"));

    const TechId tech = add_tech(g.content, "tech_a", ModifierKind::FactoryOutput, 0.1);
    CHECK(!trig(m, m.a, R"({"has_tech":"tech_a"})"));
    a.research.completed.push_back(tech);
    CHECK(trig(m, m.a, R"({"has_tech":"tech_a"})"));

    g.world.date = GameDate{1936, 6, 1, 0};
    CHECK(trig(m, m.a, R"({"date_after":"1936-01-01"})"));
    CHECK(!trig(m, m.a, R"({"date_after":"1937-01-01"})"));
    CHECK(trig(m, m.a, R"({"date_before":"1936-06-01"})"));
    CHECK(!trig(m, m.a, R"({"date_before":"1936-05-31"})"));
    CHECK(!trig(m, m.a, R"({"date_after":"not-a-date"})"));

    CHECK(!trig(m, m.a, R"({"var":{"name":"x","gte":1}})"));
    g.world.script_vars["x"] = 5.0;
    CHECK(trig(m, m.a, R"({"var":{"name":"x","gte":5}})"));
    CHECK(!trig(m, m.a, R"({"var":{"name":"x","gt":5}})"));
    CHECK(trig(m, m.a, R"({"var":{"name":"y","eq":0}})"));  // missing variable is 0
}

HOI_TEST(script_trigger_diplomacy_opinion_and_war) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    g.world.relation(m.a, m.b).value = 50.0;

    CHECK(trig(m, m.a, R"({"opinion":{"target":"NOR","gte":50}})"));
    CHECK(!trig(m, m.a, R"({"opinion":{"target":"NOR","gt":50}})"));
    CHECK(trig(m, m.a, R"({"opinion":{"target":"NOR","lte":50}})"));
    CHECK(trig(m, m.a, R"({"opinion":{"target":"NOR","eq":50}})"));
    CHECK(!trig(m, m.a, R"({"opinion":{"target":"NOWHERE","gte":0}})"));
    // target_country scope supplies the target when the trigger omits it.
    CHECK(eval_trigger(scope_of(m, m.a, StateId{}, m.b), J(R"({"opinion":{"gte":50}})")));
    CHECK(!trig(m, m.a, R"({"opinion":{"gte":50}})"));  // no target at all

    CHECK(!trig(m, m.a, R"({"at_war_with":"NOR"})"));
    MiniWorld w = make_mini_world(true);
    CHECK(trig(w, w.a, R"({"at_war_with":"NOR"})"));
    CHECK(!trig(w, w.a, R"({"at_war_with":"VLA"})"));
    CHECK(!trig(w, w.a, R"({"at_war_with":"NOPE"})"));
    CHECK(trig(w, w.a, R"({"at_war":true})"));
}

HOI_TEST(script_trigger_boolean_combinators) {
    MiniWorld m = make_mini_world();
    Country& a = m.game.world.countries[m.a];
    a.political_power = 100.0;
    a.stability = 0.9;

    CHECK(trig(m, m.a, R"({"all":[{"political_power":{"gte":100}},{"stability":{"gte":0.5}}]})"));
    CHECK(!trig(m, m.a, R"({"all":[{"political_power":{"gte":100}},{"stability":{"gte":0.99}}]})"));
    CHECK(trig(m, m.a, R"({"any":[{"political_power":{"gte":999}},{"stability":{"gte":0.5}}]})"));
    CHECK(!trig(m, m.a, R"({"any":[{"political_power":{"gte":999}},{"stability":{"gte":0.99}}]})"));
    CHECK(trig(m, m.a, R"({"not":{"political_power":{"gte":999}}})"));
    CHECK(!trig(m, m.a, R"({"not":{"political_power":{"gte":100}}})"));
    // not over an array ANDs first, then negates.
    CHECK(trig(m, m.a, R"({"not":[{"political_power":{"gte":999}},{"stability":{"gte":0.5}}]})"));
    CHECK(trig(m, m.a, R"({})"));  // empty trigger is true
}

HOI_TEST(script_trigger_unknown_key_is_false_and_does_not_mutate) {
    MiniWorld m = make_mini_world();
    const uint64_t before = world_hash(m.game);
    CHECK(!trig(m, m.a, R"({"what_is_this":{"gte":1}})"));
    CHECK(!trig(m, m.a, R"({"political_power":{"gte":0},"bogus_key":true})"));
    CHECK_EQ(world_hash(m.game), before);
}

HOI_TEST(script_chance_is_deterministic_per_seed) {
    MiniWorld m1 = make_mini_world();
    MiniWorld m2 = make_mini_world();
    m1.game.world.world_seed = 4242;
    m2.game.world.world_seed = 4242;
    m1.game.rng.seed(4242);
    m2.game.rng.seed(4242);
    m1.game.set_ai(m1.a, true);
    m2.game.set_ai(m2.a, true);

    CHECK(!eval_random_chance(m1.game, 0.0));
    CHECK(eval_random_chance(m1.game, 1.0));
    CHECK(!eval_random_chance(m1.game, -1.0));
    CHECK(eval_random_chance(m1.game, 2.0));

    for (int i = 0; i < 64; ++i) {
        const bool r1 = eval_random_chance(m1.game, 0.35);
        const bool r2 = eval_random_chance(m2.game, 0.35);
        CHECK_EQ(r1, r2);
    }
    CHECK_EQ(m1.game.rng.state_hash(), m2.game.rng.state_hash());
    CHECK_EQ(world_hash(m1.game), world_hash(m2.game));

    // A different seed takes a different path with overwhelming probability.
    MiniWorld m3 = make_mini_world();
    m3.game.rng.seed(999);
    int same = 0;
    for (int i = 0; i < 64; ++i) {
        if (eval_random_chance(m1.game, 0.5) == eval_random_chance(m3.game, 0.5)) ++same;
    }
    CHECK(same < 64);
}

// -------------------------------------------------------------------- effects --

HOI_TEST(script_effect_currencies_and_clamps) {
    MiniWorld m = make_mini_world();
    Country& a = m.game.world.countries[m.a];
    const double pp = a.political_power;
    const double manpower = a.manpower;

    fx(m, m.a, R"({"add_political_power":50,"add_stability":0.2,"add_war_support":0.25,"add_manpower":1000,"add_fuel":100})");
    CHECK_NEAR(a.political_power, pp + 50.0, 1e-9);
    CHECK_NEAR(a.stability, 0.7, 1e-9);
    CHECK_NEAR(a.war_support, 0.75, 1e-9);
    CHECK_NEAR(a.manpower, manpower + 1000.0, 1e-9);
    CHECK_NEAR(a.fuel, 100.0, 1e-9);

    fx(m, m.a, R"({"add_stability":5.0,"add_war_support":-5.0})");
    CHECK_NEAR(a.stability, 1.0, 1e-9);
    CHECK_NEAR(a.war_support, 0.0, 1e-9);
    fx(m, m.a, R"({"add_political_power":-100000,"add_manpower":-100000000})");
    CHECK_NEAR(a.political_power, 0.0, 1e-9);
    CHECK_NEAR(a.manpower, 0.0, 1e-9);

    // Malformed values are reported and skipped without changing anything.
    const uint64_t before = world_hash(m.game);
    fx(m, m.a, R"({"add_stability":"lots"})");
    CHECK_EQ(world_hash(m.game), before);
}

HOI_TEST(script_effect_opinion_and_war) {
    MiniWorld m = make_mini_world();
    fx(m, m.a, R"({"add_opinion":{"target":"NOR","value":10}})");
    CHECK_NEAR(m.game.world.relation(m.a, m.b).value, 10.0, 1e-9);
    fx(m, m.a, R"({"add_opinion":{"target":"NOR","value":1000}})");
    CHECK_NEAR(m.game.world.relation(m.a, m.b).value, 100.0, 1e-9);
    fx(m, m.a, R"({"add_opinion":{"target":"NOR","value":-1000}})");
    CHECK_NEAR(m.game.world.relation(m.a, m.b).value, -100.0, 1e-9);

    CHECK(!m.game.world.at_war(m.a, m.b));
    fx(m, m.a, R"({"declare_war":{"target":"NOR","annex":true}})");
    CHECK(m.game.world.at_war(m.a, m.b));
    CHECK(m.game.world.countries[m.a].at_war);
    CHECK(!m.game.world.countries[m.a].wars.empty());

    // A war against yourself or an unknown tag changes nothing.
    const size_t wars = m.game.world.wars.size();
    fx(m, m.b, R"({"declare_war":{"target":"NOPE"}})");
    fx(m, m.b, R"({"declare_war":{"target":"NOR"}})");
    CHECK_EQ(m.game.world.wars.size(), wars);
}

HOI_TEST(script_effect_tech_law_and_modifier) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    Country& a = g.world.countries[m.a];
    const TechId tech = add_tech(g.content, "industry_1", ModifierKind::FactoryOutput, 0.1);

    fx(m, m.a, R"({"add_tech":"industry_1"})");
    CHECK(a.research.has_tech(tech));
    CHECK_NEAR(a.tech_modifiers.get(ModifierKind::FactoryOutput), 0.1, 1e-9);
    const size_t completed = a.research.completed.size();
    fx(m, m.a, R"({"add_tech":"industry_1"})");  // re-applying is a no-op
    CHECK_EQ(a.research.completed.size(), completed);
    fx(m, m.a, R"({"add_tech":"no_such_tech"})");
    CHECK_EQ(a.research.completed.size(), completed);

    fx(m, m.a, R"({"set_law":"economy_partial"})");
    CHECK_EQ(a.law_levels[1], 1);

    fx(m, m.a, R"({"add_modifier":{"kind":"DivisionAttack","value":0.05,"days":90,"source":"focus_x"}})");
    fx(m, m.a, R"({"add_modifier":{"kind":"division_attack","value":0.02}})");  // perm, loose name
    CHECK_EQ(a.timed_modifiers.size(), 2u);
    CHECK_NEAR(a.timed_modifiers[0].mods.get(ModifierKind::DivisionAttack), 0.05, 1e-9);
    CHECK_EQ(a.timed_modifiers[0].days_left, 90);
    CHECK_EQ(a.timed_modifiers[0].source, std::string("focus_x"));
    CHECK_NEAR(a.timed_modifiers[1].mods.get(ModifierKind::DivisionAttack), 0.02, 1e-9);
    CHECK_EQ(a.timed_modifiers[1].days_left, -1);
    CHECK_EQ(a.timed_modifiers[1].source, std::string("script"));

    fx(m, m.a, R"({"add_modifier":{"kind":"NotAKind","value":1}})");
    fx(m, m.a, R"({"add_modifier":{"kind":"DivisionAttack","value":"x"}})");
    CHECK_EQ(a.timed_modifiers.size(), 2u);
}

HOI_TEST(script_effect_focus_event_and_flags) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    Country& a = g.world.countries[m.a];
    const uint32_t focus = add_focus(g.content, "focus_a", R"({"add_political_power":25})");
    add_event(g.content, "event_a");

    const double pp = a.political_power;
    fx(m, m.a, R"({"complete_focus":"focus_a"})");
    CHECK(std::find(a.completed_focuses.begin(), a.completed_focuses.end(), focus) !=
          a.completed_focuses.end());
    CHECK_NEAR(a.political_power, pp + 25.0, 1e-9);
    fx(m, m.a, R"({"complete_focus":"focus_a"})");  // already completed: no double effect
    CHECK_NEAR(a.political_power, pp + 25.0, 1e-9);
    fx(m, m.a, R"({"complete_focus":"focus_missing"})");
    CHECK_EQ(a.completed_focuses.size(), 1u);

    fx(m, m.a, R"({"trigger_event":{"key":"event_a"}})");
    CHECK(std::find(a.pending_events.begin(), a.pending_events.end(), 0u) !=
          a.pending_events.end());
    fx(m, m.a, R"({"trigger_event":{"key":"event_a","days":7}})");
    CHECK_EQ(g.world.delayed_events.size(), 1u);
    CHECK_EQ(g.world.delayed_events[0].country, m.a);
    CHECK_EQ(g.world.delayed_events[0].event, 0u);
    CHECK_EQ(g.world.delayed_events[0].due, g.world.tick + 7 * TICKS_PER_DAY);
    fx(m, m.a, R"({"trigger_event":{"key":"event_missing","days":3}})");
    CHECK_EQ(g.world.delayed_events.size(), 1u);

    fx(m, m.a, R"({"set_flag":"flag_a"})");
    CHECK(std::find(a.country_flags.begin(), a.country_flags.end(), "flag_a") !=
          a.country_flags.end());
    fx(m, m.a, R"({"set_flag":"flag_a"})");  // idempotent
    CHECK_EQ(a.country_flags.size(), 1u);
    fx(m, m.a, R"({"clear_flag":"flag_a"})");
    CHECK(a.country_flags.empty());
}

HOI_TEST(script_effect_variables_and_claim) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;

    fx(m, m.a, R"({"set_variable":{"name":"v","value":3},"add_to_variable":{"name":"v","value":2}})");
    CHECK_NEAR(g.world.script_vars["v"], 5.0, 1e-9);
    fx(m, m.a, R"({"set_variable":{"name":"v","value":1}})");
    CHECK_NEAR(g.world.script_vars["v"], 1.0, 1e-9);
    fx(m, m.a, R"({"add_to_variable":{"name":"w","value":4}})");
    CHECK_NEAR(g.world.script_vars["w"], 4.0, 1e-9);

    // add_claim appends the claimant to the state's core owners, once.
    fx(m, m.a, R"({"add_claim":"state_b"})");
    const State& sb = g.world.states[m.state_b];
    CHECK(std::find(sb.core_owners.begin(), sb.core_owners.end(), m.a) != sb.core_owners.end());
    fx(m, m.a, R"({"add_claim":"state_b"})");  // duplicate is not appended
    size_t claims = 0;
    for (CountryId co : sb.core_owners) {
        if (co == m.a) ++claims;
    }
    CHECK_EQ(claims, 1u);
    fx(m, m.a, R"({"add_claim":"s1"})");  // map-key form targets state_a
    const State& sa = g.world.states[m.state_a];
    CHECK(std::find(sa.core_owners.begin(), sa.core_owners.end(), m.a) != sa.core_owners.end());
    fx(m, m.a, R"({"add_claim":"s9999"})");
    CHECK(!g.world.script_vars.count("claim:9998"));
}

HOI_TEST(script_effect_state_flags) {
    MiniWorld m = make_mini_world();
    State& sa = m.game.world.states[m.state_a];

    apply_effects(scope_of(m, m.a, m.state_a), J(R"({"set_state_flag":"port_a"})"));
    CHECK(std::find(sa.flags.begin(), sa.flags.end(), "port_a") != sa.flags.end());
    CHECK(trig(m, m.a, R"({"state_has_flag":"port_a"})", m.state_a));

    fx(m, m.a, R"({"set_state_flag":"port_a"})");  // idempotent
    CHECK_EQ(sa.flags.size(), 1u);

    // The effect needs a state scope; without one nothing is written.
    const uint64_t before = world_hash(m.game);
    fx(m, m.a, R"({"set_state_flag":"leak"})");
    CHECK_EQ(world_hash(m.game), before);

    apply_effects(scope_of(m, m.a, m.state_a), J(R"({"clear_state_flag":"port_a"})"));
    CHECK(sa.flags.empty());
    CHECK(!trig(m, m.a, R"({"state_has_flag":"port_a"})", m.state_a));
}

HOI_TEST(script_effect_unknown_key_is_ignored_without_mutation) {
    MiniWorld m = make_mini_world();
    const uint64_t before = world_hash(m.game);
    // Only unknown keys: nothing at all may change.
    fx(m, m.a, R"({"not_an_effect":123})");
    CHECK_EQ(world_hash(m.game), before);
    // An unknown key beside a valid one: the valid effect applies, the bad one is
    // ignored rather than aborting the block.
    fx(m, m.a, R"({"add_political_power":5,"also_not_an_effect":true})");
    CHECK_NEAR(m.game.world.countries[m.a].political_power, 500.0 + 5.0, 1e-9);
}

HOI_TEST(script_failing_trigger_never_mutates_state) {
    MiniWorld m = make_mini_world();
    Country& a = m.game.world.countries[m.a];
    const uint64_t before = world_hash(m.game);
    // Every clause here fails, including a nested `any` and an unknown key.
    CHECK(!trig(m, m.a,
                R"({"political_power":{"gte":0},"stability":{"gte":0.5},"all":[{"year":{"gte":1936}}],"any":[{"fuel":{"gte":99999}},{"at_war":true}],"bogus":1})"));
    CHECK_EQ(world_hash(m.game), before);
    CHECK_NEAR(a.stability, 0.5, 1e-9);
}

HOI_TEST(script_two_identical_runs_share_world_hash) {
    auto run = [](MiniWorld& m) {
        Game& g = m.game;
        g.world.world_seed = 77;
        g.rng.seed(77);
        g.set_ai(m.a, true);
        for (int i = 0; i < 40; ++i) {
            ScriptScope s = scope_of(m, m.a);
            const bool rain = eval_trigger(s, J(R"({"chance":0.4})"));
            apply_effects(s, J(rain ? R"({"add_political_power":1,"add_to_variable":{"name":"n","value":1}})"
                                   : R"({"add_stability":0.01,"set_flag":"sunny"})"));
            const bool war_ready = eval_trigger(s, J(R"({"all":[{"political_power":{"gte":0}},{"var":{"name":"n","gte":0}}],"any":[{"chance":0.1},{"fuel":{"gte":0}}]})"));
            apply_effects(s, J(war_ready ? R"({"set_variable":{"name":"ready","value":1}})"
                                         : R"({"add_fuel":2})"));
        }
    };
    MiniWorld m1 = make_mini_world();
    MiniWorld m2 = make_mini_world();
    run(m1);
    run(m2);
    CHECK_EQ(world_hash(m1.game), world_hash(m2.game));
    CHECK_EQ(m1.game.rng.state_hash(), m2.game.rng.state_hash());
    CHECK_NEAR(m1.game.world.script_vars["n"], m2.game.world.script_vars["n"], 0.0);
}

HOI_TEST(script_error_description_is_actionable) {
    const std::string msg = describe_script_error("bogus_key", "unknown trigger key");
    CHECK(msg.find("bogus_key") != std::string::npos);
    CHECK(msg.find("unknown trigger key") != std::string::npos);
}

}  // namespace