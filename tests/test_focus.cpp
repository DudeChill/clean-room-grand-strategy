// Focus trees: availability, selection, daily progress, bypass, timed modifiers,
// the AI focus layer and determinism. Hand-built worlds only (no scenario files).

#include <algorithm>
#include <string>
#include <vector>

#include "core/json.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/focus.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using hoi_test::make_test_content;

Json obj() { return Json::object(); }

Json comparator(const char* op, double value) {
    Json j = obj();
    j.set(op, Json(value));
    return j;
}

uint32_t add_focus(Content& c, const std::string& key, const std::string& name, double days,
                   std::vector<std::string> prerequisites = {},
                   std::vector<std::string> mutually_exclusive = {}, Json available = Json(),
                   Json bypass = Json(), Json effects = Json(), double ai_weight = 1.0) {
    FocusDef f;
    f.index = static_cast<uint32_t>(c.focuses.size());
    f.key = key;
    f.name = name;
    f.tree = "VLA";
    f.days = days;
    f.prerequisites = std::move(prerequisites);
    f.mutually_exclusive = std::move(mutually_exclusive);
    f.available = std::move(available);
    f.bypass = std::move(bypass);
    f.effects = std::move(effects);
    f.ai_weight = ai_weight;
    c.focus_index[f.key] = f.index;
    c.focuses.push_back(f);
    return static_cast<uint32_t>(c.focuses.size() - 1);
}

TechId add_tech(Content& c, const std::string& key) {
    TechDef t;
    t.id = TechId(static_cast<uint32_t>(c.techs.size()));
    t.key = key;
    t.name = key;
    t.category = "industry";
    c.tech_by_key[t.key] = t.id;
    c.techs.push_back(t);
    return t.id;
}

// One country, one state, one capital province, owned and controlled by the country.
struct FocusWorld {
    Game g;
    CountryId c;
    StateId state;
    ProvinceId province;
    RegionId region;
};

FocusWorld make_world(double political_power = 0.0) {
    FocusWorld fw;
    fw.g.content = make_test_content();
    World& w = fw.g.world;
    w.tick = 0;
    w.date = GameDate{1936, 1, 1, 0};

    Region r;
    r.name = "region";
    fw.region = w.regions.create(r);
    w.regions[fw.region].id = fw.region;

    State s;
    s.name = "home";
    s.building_slots = 20;
    fw.state = w.states.create(s);
    w.states[fw.state].id = fw.state;

    Province p;
    p.name = "capital";
    p.state = fw.state;
    p.region = fw.region;
    p.terrain = Terrain::Plains;
    p.population = 100000.0;
    p.is_capital = true;
    fw.province = w.provinces.create(p);
    w.provinces[fw.province].id = fw.province;
    w.states[fw.state].provinces.push_back(fw.province);

    Country cc;
    cc.tag = "VLA";
    cc.name = "Valtia";
    cc.capital = fw.state;
    cc.political_power = political_power;
    cc.law_levels.assign(4, 0);
    cc.research.slots.resize(3);
    cc.research.slots_unlocked = 3;
    cc.starting_factories = 20;
    fw.c = w.countries.create(cc);
    w.countries[fw.c].id = fw.c;

    State& st = w.states[fw.state];
    st.owner = fw.c;
    st.controller = fw.c;
    st.core_owners.push_back(fw.c);
    st.civilian_factories = 10;
    st.military_factories = 10;
    Province& prov = w.provinces[fw.province];
    prov.owner = fw.c;
    prov.controller = fw.c;

    fw.g.ai_controlled.assign(w.countries.capacity(), 0);
    return fw;
}

// Runs `days` full focus days, each optionally planning with the focus AI first.
void run_days(FocusWorld& fw, int days, bool with_ai) {
    for (int day = 0; day < days; ++day) {
        fw.g.world.tick = static_cast<Tick>(day * TICKS_PER_DAY);
        fw.g.world.date = tick_to_date(fw.g.world.tick, GameDate{1936, 1, 1, 0});
        if (with_ai && fw.g.is_ai(fw.c)) {
            ai_focus_layer(fw.g, *fw.g.world.country(fw.c));
        }
        phase_commands(fw.g);
        phase_focuses(fw.g);
    }
}

bool has_completed(const Country& c, uint32_t focus) {
    return std::find(c.completed_focuses.begin(), c.completed_focuses.end(), focus) !=
           c.completed_focuses.end();
}

bool has_flag(const Country& c, const std::string& flag) {
    return std::find(c.country_flags.begin(), c.country_flags.end(), flag) != c.country_flags.end();
}

}  // namespace

// Prerequisites gate a focus, mutual exclusions block it, and the `available`
// trigger must pass. Completion and a missing index both make it unavailable.
HOI_TEST(focus_available_prerequisites_exclusions_and_triggers) {
    FocusWorld fw = make_world(0.0);
    Content& content = fw.g.content;
    const uint32_t first = add_focus(content, "first", "First", 10.0);
    const uint32_t second = add_focus(content, "second", "Second", 10.0, {"first"});
    Json plenty = obj();
    plenty.set("political_power", comparator("gte", 100.0));
    const uint32_t rich = add_focus(content, "rich", "Rich", 10.0, {}, {}, plenty);
    const uint32_t excl_a = add_focus(content, "excl_a", "Alpha", 10.0, {}, {"excl_b"});
    const uint32_t excl_b = add_focus(content, "excl_b", "Beta", 10.0, {}, {"excl_a"});

    CHECK(focus_available(fw.g, fw.c, first));
    CHECK(!focus_available(fw.g, fw.c, second));
    CHECK(!focus_available(fw.g, fw.c, rich));
    CHECK(focus_available(fw.g, fw.c, excl_a));
    CHECK(focus_available(fw.g, fw.c, excl_b));
    CHECK(!focus_available(fw.g, fw.c, INVALID_FOCUS));
    CHECK(!focus_available(fw.g, fw.c, 999u));

    // Completing the prerequisite opens the next one.
    CHECK(focus_complete(fw.g, fw.c, first));
    CHECK(focus_available(fw.g, fw.c, second));
    CHECK(!focus_available(fw.g, fw.c, first));  // already completed

    // The availability trigger reads live country state.
    fw.g.world.country(fw.c)->political_power = 150.0;
    CHECK(focus_available(fw.g, fw.c, rich));

    // Completing one side of an exclusion blocks the other.
    CHECK(focus_complete(fw.g, fw.c, excl_a));
    CHECK(!focus_available(fw.g, fw.c, excl_b));
}

// Selecting starts progress; re-selecting the same focus is a no-op that keeps the
// progress; selecting a different one replaces it and loses the progress; cancel
// clears both.
HOI_TEST(focus_select_replace_and_cancel) {
    FocusWorld fw = make_world();
    Content& content = fw.g.content;
    const uint32_t a = add_focus(content, "a", "Alpha", 70.0);
    const uint32_t b = add_focus(content, "b", "Beta", 70.0);

    CHECK(focus_select(fw.g, fw.c, a));
    Country& c = *fw.g.world.country(fw.c);
    CHECK_EQ(c.selected_focus, a);
    CHECK_NEAR(c.focus_progress, 0.0, 1e-9);

    c.focus_progress = 34.0;
    CHECK(focus_select(fw.g, fw.c, a));  // same focus: no-op
    CHECK_EQ(c.selected_focus, a);
    CHECK_NEAR(c.focus_progress, 34.0, 1e-9);

    CHECK(focus_select(fw.g, fw.c, b));  // replace
    CHECK_EQ(c.selected_focus, b);
    CHECK_NEAR(c.focus_progress, 0.0, 1e-9);

    focus_cancel(fw.g, fw.c);
    CHECK_EQ(c.selected_focus, static_cast<uint32_t>(INVALID_FOCUS));
    CHECK_NEAR(c.focus_progress, 0.0, 1e-9);

    // An unavailable focus is rejected without changing state.
    CHECK(focus_complete(fw.g, fw.c, a));
    c.selected_focus = b;
    CHECK(!focus_select(fw.g, fw.c, a));
    CHECK_EQ(c.selected_focus, b);

    // The status line reports progress and prerequisite counts.
    const std::string text = focus_status_text(fw.g, fw.c, b);
    CHECK(text.find("Beta") != std::string::npos);
    CHECK(text.find("0/70 days") != std::string::npos);
}

// Reaching the focus's duration applies its effect block through the script engine:
// political power, a timed modifier, a technology and a flag are all observable.
HOI_TEST(focus_completion_applies_effects) {
    FocusWorld fw = make_world(10.0);
    Content& content = fw.g.content;
    const TechId tech = add_tech(content, "industry_1");

    Json effects = obj();
    effects.set("add_political_power", Json(50.0));
    Json mod = obj();
    mod.set("kind", Json(std::string("PoliticalPowerGain")));
    mod.set("value", Json(0.1));
    mod.set("days", Json(30.0));
    mod.set("source", Json(std::string("fx")));
    effects.set("add_modifier", mod);
    effects.set("add_tech", Json(std::string("industry_1")));
    effects.set("set_flag", Json(std::string("fx_done")));

    const uint32_t fx = add_focus(content, "fx", "Industrial Push", 2.0, {}, {}, Json(), Json(),
                                  effects);
    CHECK(focus_select(fw.g, fw.c, fx));

    run_days(fw, 2, false);
    Country& c = *fw.g.world.country(fw.c);

    CHECK(has_completed(c, fx));
    CHECK_EQ(c.selected_focus, static_cast<uint32_t>(INVALID_FOCUS));
    CHECK_NEAR(c.focus_progress, 0.0, 1e-9);
    CHECK_NEAR(c.political_power, 60.0, 1e-9);
    CHECK(c.research.has_tech(tech));
    CHECK(has_flag(c, "fx_done"));
    CHECK_EQ(c.timed_modifiers.size(), static_cast<size_t>(1));
    CHECK_EQ(c.timed_modifiers[0].source, std::string("fx"));
    CHECK_NEAR(c.timed_modifiers[0].mods.get(ModifierKind::PoliticalPowerGain), 0.1, 1e-9);
    CHECK_EQ(c.timed_modifiers[0].days_left, 30);  // granted today, counts from tomorrow
}

// A focus whose bypass trigger passes completes the moment it is selected, logs the
// bypass, and grants none of its effects.
HOI_TEST(focus_bypass_completes_without_effects) {
    FocusWorld fw = make_world(0.0);
    Content& content = fw.g.content;
    Json always = obj();
    always.set("political_power", comparator("gte", 0.0));
    Json effects = obj();
    effects.set("add_political_power", Json(50.0));
    const uint32_t fx = add_focus(content, "skippable", "Skippable", 70.0, {}, {}, Json(), always,
                                  effects);

    CHECK(focus_bypassable(fw.g, fw.c, fx));
    CHECK(focus_select(fw.g, fw.c, fx));
    Country& c = *fw.g.world.country(fw.c);
    CHECK(has_completed(c, fx));
    CHECK_EQ(c.selected_focus, static_cast<uint32_t>(INVALID_FOCUS));
    CHECK_NEAR(c.political_power, 0.0, 1e-9);  // effects skipped

    bool logged = false;
    for (const SimEvent& e : fw.g.events) {
        if (e.kind == "focus" && e.text.find("bypassed") != std::string::npos) logged = true;
    }
    CHECK(logged);
}

// Timed modifiers lose a day per day and are removed at zero; permanent ones last.
HOI_TEST(focus_phase_expires_timed_modifiers) {
    FocusWorld fw = make_world();
    Country& c = *fw.g.world.country(fw.c);
    TimedModifier temp;
    temp.source = "temp";
    temp.days_left = 2;
    c.timed_modifiers.push_back(temp);
    TimedModifier perm;
    perm.source = "perm";
    perm.days_left = -1;
    c.timed_modifiers.push_back(perm);

    run_days(fw, 1, false);
    CHECK_EQ(c.timed_modifiers.size(), static_cast<size_t>(2));
    CHECK_EQ(c.timed_modifiers[0].days_left, 1);

    run_days(fw, 1, false);
    CHECK_EQ(c.timed_modifiers.size(), static_cast<size_t>(1));
    CHECK_EQ(c.timed_modifiers[0].source, std::string("perm"));
    CHECK_EQ(c.timed_modifiers[0].days_left, -1);

    run_days(fw, 5, false);
    CHECK_EQ(c.timed_modifiers.size(), static_cast<size_t>(1));  // permanent survives
}

// The AI pushes a SelectFocus command for a valid focus, and works the tree over the
// following days; reasons are recorded in the politics AI layer.
HOI_TEST(focus_ai_selects_and_advances_within_thirty_days) {
    FocusWorld fw = make_world(0.0);
    Content& content = fw.g.content;
    Json good_effects = obj();
    good_effects.set("add_political_power", Json(200.0));
    const uint32_t good =
        add_focus(content, "good", "Good", 5.0, {}, {}, Json(), Json(), good_effects);
    add_focus(content, "plain", "Plain", 5.0);
    Json later_effects = obj();
    later_effects.set("add_political_power", Json(100.0));
    const uint32_t later =
        add_focus(content, "later", "Later", 5.0, {"good"}, {}, Json(), Json(), later_effects);

    fw.g.set_ai(fw.c, true);
    const CountryId cid = fw.c;
    Country& c = *fw.g.world.country(cid);
    bool saw_good_selected = false;
    for (int day = 0; day < 30; ++day) {
        fw.g.world.tick = static_cast<Tick>(day * TICKS_PER_DAY);
        fw.g.world.date = tick_to_date(fw.g.world.tick, GameDate{1936, 1, 1, 0});
        ai_focus_layer(fw.g, c);
        if (c.selected_focus == good) saw_good_selected = true;
        phase_commands(fw.g);
        phase_focuses(fw.g);
    }

    CHECK(saw_good_selected);          // a focus was selected well within 30 days
    CHECK(has_completed(c, good));     // the best-scoring focus was worked first
    CHECK(has_completed(c, later));    // then the prerequisite-gated one
    CHECK_NEAR(c.political_power, 300.0, 1e-9);

    // Reasons were recorded while considering focuses.
    CHECK(!fw.g.ai.layer(AiLayer::Politics).last_reasons.empty());
}

// Two identical runs produce identical world hashes: the AI picks the same focus,
// completion consumes the same RNG draws, and the timed modifiers line up.
HOI_TEST(focus_determinism_two_runs) {
    auto run = [](uint64_t seed) -> uint64_t {
        FocusWorld fw = make_world(0.0);
        fw.g.world.world_seed = seed;
        fw.g.rng.seed(seed);
        Content& content = fw.g.content;

        Json maybe = obj();
        maybe.set("chance", Json(0.5));
        Json chancy_effects = obj();
        chancy_effects.set("add_political_power", Json(200.0));
        chancy_effects.set("set_flag", Json(std::string("chosen")));
        add_focus(content, "chancy", "Chancy", 3.0, {}, {}, maybe, maybe, chancy_effects);
        Json industry = obj();
        industry.set("add_political_power", Json(80.0));
        add_focus(content, "industry", "Industry", 4.0, {}, {}, Json(), Json(), industry);

        fw.g.set_ai(fw.c, true);
        run_days(fw, 40, true);
        return world_hash(fw.g);
    };

    const uint64_t first = run(12345);
    const uint64_t second = run(12345);
    CHECK_EQ(first, second);
}