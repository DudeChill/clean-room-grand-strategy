// National spirits and political advisors: availability, add/remove, slot
// accounting, effect application, the daily repair pass, the AI politics layer and
// determinism. Hand-built worlds only (no scenario files).

#include <algorithm>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/types.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/phases.h"
#include "sim/spirits.h"
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

// {"political_power": {"gte": v}}
Json pp_gte(double value) {
    Json j = obj();
    j.set("political_power", comparator("gte", value));
    return j;
}

uint32_t add_spirit(Content& c, const std::string& key, int slots = 1,
                    Json available = Json(), Json effects = Json(), Modifiers mods = Modifiers()) {
    SpiritDef s;
    s.index = static_cast<uint32_t>(c.spirits.size());
    s.key = key;
    s.name = key;
    s.slots = slots;
    s.available = std::move(available);
    s.effects = std::move(effects);
    s.modifiers = mods;
    c.spirit_index[s.key] = s.index;
    c.spirits.push_back(s);
    return s.index;
}

uint32_t add_advisor(Content& c, const std::string& key, double cost = 150.0,
                     Json available = Json(), Modifiers mods = Modifiers()) {
    AdvisorDef a;
    a.index = static_cast<uint32_t>(c.advisors.size());
    a.key = key;
    a.name = key;
    a.cost_pp = cost;
    a.available = std::move(available);
    a.modifiers = mods;
    c.advisor_index[a.key] = a.index;
    c.advisors.push_back(a);
    return a.index;
}

Modifiers mod_of(ModifierKind kind, double value) {
    Modifiers m;
    m.set(kind, value);
    return m;
}

// One country, one owned state and one owned capital province.
struct SpiritWorld {
    Game g;
    CountryId c;
    StateId state;
    ProvinceId province;
    RegionId region;
};

SpiritWorld make_world(double political_power = 0.0, int spirit_slots = 6,
                       int advisor_slots = 3) {
    SpiritWorld fw;
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
    cc.spirit_slots = spirit_slots;
    cc.advisor_slots = advisor_slots;
    cc.equipment_stockpile.assign(fw.g.content.equipment.size(), 1000.0);
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

// Runs one simulated day: AI planning (optional), then the command and spirit phases.
void run_day(SpiritWorld& fw, int day, bool with_ai) {
    fw.g.world.tick = static_cast<Tick>(day * TICKS_PER_DAY);
    fw.g.world.date = tick_to_date(fw.g.world.tick, GameDate{1936, 1, 1, 0});
    if (with_ai && fw.g.is_ai(fw.c)) {
        ai_spirit_advisor_layer(fw.g, *fw.g.world.country(fw.c));
    }
    phase_commands(fw.g);
    phase_spirits(fw.g);
}

bool has_flag(const Country& c, const std::string& flag) {
    return std::find(c.country_flags.begin(), c.country_flags.end(), flag) !=
           c.country_flags.end();
}

bool logged_kind(const Game& g, const std::string& kind, const std::string& needle) {
    for (const SimEvent& e : g.events) {
        if (e.kind == kind && e.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// A spirit is available only when it exists, the country is alive, it is not already
// held, its trigger passes and a free slot can pay its cost.
HOI_TEST(spirit_available_gating_trigger_slots_and_duplicates) {
    SpiritWorld fw = make_world(50.0);
    Content& content = fw.g.content;
    const uint32_t plain = add_spirit(content, "plain", 1);
    const uint32_t big = add_spirit(content, "big", 4);
    const uint32_t gated = add_spirit(content, "gated", 1, pp_gte(100.0));

    CHECK(spirit_available(fw.g, fw.c, plain));
    CHECK(spirit_available(fw.g, fw.c, big));
    CHECK(!spirit_available(fw.g, fw.c, gated));  // trigger fails below 100 pp
    CHECK(!spirit_available(fw.g, fw.c, 999u));   // unknown index
    CHECK(!spirit_available(fw.g, fw.c, INVALID_ID));

    fw.g.world.country(fw.c)->political_power = 150.0;
    CHECK(spirit_available(fw.g, fw.c, gated));  // trigger now passes

    // Slots: a country with two free slots cannot take a four-slot spirit.
    Country& c = *fw.g.world.country(fw.c);
    c.spirit_slots = 2;
    CHECK(!spirit_available(fw.g, fw.c, big));
    c.spirit_slots = 6;

    // Already held: no longer available.
    CHECK(spirit_add(fw.g, fw.c, plain));
    CHECK(!spirit_available(fw.g, fw.c, plain));

    // Dead country: nothing is available.
    c.alive = false;
    CHECK(!spirit_available(fw.g, fw.c, big));
}

// Add and remove are a round trip: both the key and the permanent modifier entry
// appear and disappear, and a duplicate add is a no-op.
HOI_TEST(spirit_add_remove_round_trip_and_double_add) {
    SpiritWorld fw = make_world();
    const uint32_t spirit =
        add_spirit(fw.g.content, "industry", 2, Json(), Json(),
                   mod_of(ModifierKind::ConstructionSpeed, 0.1));
    Country& c = *fw.g.world.country(fw.c);

    CHECK(spirit_add(fw.g, fw.c, spirit));
    CHECK_EQ(c.spirit_keys.size(), static_cast<size_t>(1));
    CHECK_EQ(c.spirit_keys[0], spirit);
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits[0].source, std::string("industry"));
    CHECK_EQ(c.national_spirits[0].days_left, -1);  // permanent
    CHECK(has_spirit(fw.g.world, fw.c, "industry"));
    CHECK_NEAR(c.total_modifiers().get(ModifierKind::ConstructionSpeed), 0.1, 1e-9);
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 4);

    // Double add is a no-op: no second key, no second modifier, no double effect.
    CHECK(!spirit_add(fw.g, fw.c, spirit));
    CHECK_EQ(c.spirit_keys.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(1));
    CHECK_NEAR(c.total_modifiers().get(ModifierKind::ConstructionSpeed), 0.1, 1e-9);

    CHECK(spirit_remove(fw.g, fw.c, spirit));
    CHECK(c.spirit_keys.empty());
    CHECK(c.national_spirits.empty());
    CHECK(!has_spirit(fw.g.world, fw.c, "industry"));
    CHECK_NEAR(c.total_modifiers().get(ModifierKind::ConstructionSpeed), 0.0, 1e-9);
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 6);

    CHECK(!spirit_remove(fw.g, fw.c, spirit));  // no longer held
    CHECK(!spirit_remove(fw.g, fw.c, 999u));    // unknown index
}

// The spirit's effect block runs exactly once through the script engine, even when
// the add is repeated.
HOI_TEST(spirit_effects_applied_exactly_once) {
    SpiritWorld fw = make_world(10.0);
    Json effects = obj();
    effects.set("add_political_power", Json(40.0));
    effects.set("set_flag", Json(std::string("spirit_done")));
    const uint32_t spirit = add_spirit(fw.g.content, "fx", 1, Json(), effects);

    CHECK(spirit_add(fw.g, fw.c, spirit));
    Country& c = *fw.g.world.country(fw.c);
    CHECK_NEAR(c.political_power, 50.0, 1e-9);
    CHECK(has_flag(c, "spirit_done"));

    CHECK(!spirit_add(fw.g, fw.c, spirit));  // already held
    CHECK_NEAR(c.political_power, 50.0, 1e-9);  // effects did not run again
    size_t flagged = 0;
    for (const std::string& f : c.country_flags) {
        if (f == "spirit_done") ++flagged;
    }
    CHECK_EQ(flagged, static_cast<size_t>(1));
}

// Slot accounting sums the slots of every held spirit; a spirit that does not fit is
// rejected, and removing one frees its slots again.
HOI_TEST(spirit_slot_accounting) {
    SpiritWorld fw = make_world();
    const uint32_t a = add_spirit(fw.g.content, "a", 1);
    const uint32_t b = add_spirit(fw.g.content, "b", 2);
    const uint32_t d = add_spirit(fw.g.content, "d", 3);
    const uint32_t overflow = add_spirit(fw.g.content, "overflow", 1);

    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 6);
    CHECK(spirit_add(fw.g, fw.c, a));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 5);
    CHECK(spirit_add(fw.g, fw.c, b));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 3);
    CHECK(spirit_add(fw.g, fw.c, d));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 0);

    CHECK(!spirit_available(fw.g, fw.c, overflow));
    CHECK(!spirit_add(fw.g, fw.c, overflow));

    CHECK(spirit_remove(fw.g, fw.c, b));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 2);
    CHECK(spirit_add(fw.g, fw.c, overflow));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 1);
}

// Advisors cost political power, occupy a slot, grant their modifiers and refund
// nothing on dismissal.
HOI_TEST(advisor_availability_cost_and_dismissal) {
    SpiritWorld fw = make_world(100.0);
    const uint32_t cheap =
        add_advisor(fw.g.content, "chief", 150.0, Json(),
                    mod_of(ModifierKind::DivisionAttack, 0.1));
    const uint32_t gated = add_advisor(fw.g.content, "gated", 50.0, pp_gte(200.0));

    CHECK(!advisor_available(fw.g, fw.c, cheap));  // 100 pp < 150 cost
    CHECK(!advisor_available(fw.g, fw.c, gated));  // trigger fails
    CHECK(!advisor_appoint(fw.g, fw.c, cheap));
    CHECK(!advisor_available(fw.g, fw.c, 999u));

    Country& c = *fw.g.world.country(fw.c);
    c.political_power = 250.0;
    CHECK(advisor_available(fw.g, fw.c, cheap));
    CHECK(advisor_available(fw.g, fw.c, gated));

    CHECK(advisor_appoint(fw.g, fw.c, cheap));
    CHECK_NEAR(c.political_power, 100.0, 1e-9);  // cost deducted
    CHECK_EQ(c.advisors.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits[0].source, std::string("chief"));
    CHECK_EQ(c.national_spirits[0].days_left, -1);
    CHECK(has_advisor(fw.g.world, fw.c, "chief"));
    CHECK_NEAR(c.total_modifiers().get(ModifierKind::DivisionAttack), 0.1, 1e-9);
    CHECK_EQ(advisor_slots_free(fw.g, fw.c), 2);

    // Re-appointing the same advisor is rejected (already in office).
    CHECK(!advisor_appoint(fw.g, fw.c, cheap));
    CHECK_NEAR(c.political_power, 100.0, 1e-9);

    advisor_dismiss(fw.g, fw.c, cheap);
    CHECK(c.advisors.empty());
    CHECK(c.national_spirits.empty());
    CHECK(!has_advisor(fw.g.world, fw.c, "chief"));
    CHECK_NEAR(c.political_power, 100.0, 1e-9);  // no refund
    CHECK_NEAR(c.total_modifiers().get(ModifierKind::DivisionAttack), 0.0, 1e-9);
    CHECK_EQ(advisor_slots_free(fw.g, fw.c), 3);

    advisor_dismiss(fw.g, fw.c, cheap);  // not in office: no-op
}

// Advisor slots cap appointments.
HOI_TEST(advisor_slots_gating) {
    SpiritWorld fw = make_world(1000.0, 6, 1);
    const uint32_t first = add_advisor(fw.g.content, "first", 10.0);
    const uint32_t second = add_advisor(fw.g.content, "second", 10.0);

    CHECK(advisor_appoint(fw.g, fw.c, first));
    CHECK_EQ(advisor_slots_free(fw.g, fw.c), 0);
    CHECK(!advisor_available(fw.g, fw.c, second));
    CHECK(!advisor_appoint(fw.g, fw.c, second));

    advisor_dismiss(fw.g, fw.c, first);
    CHECK(advisor_available(fw.g, fw.c, second));
    CHECK(advisor_appoint(fw.g, fw.c, second));
}

// phase_spirits drops a stale permanent modifier and an index whose content no
// longer exists, keeps the entries that are still backed, and logs the repair once.
HOI_TEST(phase_spirits_repairs_stale_entries) {
    SpiritWorld fw = make_world(500.0);
    const uint32_t spirit = add_spirit(fw.g.content, "kept", 1);
    const uint32_t advisor = add_advisor(fw.g.content, "chief", 10.0);

    CHECK(spirit_add(fw.g, fw.c, spirit));
    CHECK(advisor_appoint(fw.g, fw.c, advisor));

    Country& c = *fw.g.world.country(fw.c);
    // Corrupt: an out-of-range index and a permanent modifier nobody accounts for.
    c.spirit_keys.push_back(999u);
    c.advisors.push_back(999u);
    TimedModifier ghost;
    ghost.source = "ghost";
    ghost.days_left = -1;
    c.national_spirits.push_back(ghost);

    const size_t events_before = fw.g.events.size();
    fw.g.world.tick = 0;
    phase_spirits(fw.g);

    CHECK_EQ(c.spirit_keys.size(), static_cast<size_t>(1));
    CHECK_EQ(c.spirit_keys[0], spirit);
    CHECK_EQ(c.advisors.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(2));  // spirit + advisor kept
    CHECK(has_spirit(fw.g.world, fw.c, "kept"));
    CHECK(has_advisor(fw.g.world, fw.c, "chief"));
    CHECK(fw.g.events.size() > events_before);
    CHECK(logged_kind(fw.g, "spirit", "repaired"));

    // A second clean run repairs nothing and logs nothing.
    const size_t events_clean = fw.g.events.size();
    fw.g.world.tick = static_cast<Tick>(TICKS_PER_DAY);
    phase_spirits(fw.g);
    CHECK_EQ(fw.g.events.size(), events_clean);
}

// The AI appoints an affordable advisor and adopts a beneficial spirit through the
// command queue, and records its reasons on the politics layer.
HOI_TEST(ai_appoints_advisor_and_adopts_spirit_within_thirty_days) {
    SpiritWorld fw = make_world(300.0);
    add_advisor(fw.g.content, "chief", 150.0, Json(), mod_of(ModifierKind::DivisionAttack, 0.1));
    add_spirit(fw.g.content, "industry", 1, Json(), Json(),
               mod_of(ModifierKind::ConstructionSpeed, 0.1));

    fw.g.set_ai(fw.c, true);
    for (int day = 0; day < 30; ++day) run_day(fw, day, true);

    Country& c = *fw.g.world.country(fw.c);
    CHECK(has_advisor(fw.g.world, fw.c, "chief"));
    CHECK(has_spirit(fw.g.world, fw.c, "industry"));
    CHECK_NEAR(c.political_power, 150.0, 1e-9);  // exactly one 150 pp appointment
    CHECK_GT(fw.g.ai.commands_issued, 0u);
    CHECK(!fw.g.ai.layer(AiLayer::Politics).last_reasons.empty());
}

// Posture: at war the AI prefers a military spirit, at peace an industrial one.
HOI_TEST(ai_posture_prefers_matching_spirit) {
    auto best_spirit_name = [](bool at_war) -> std::string {
        SpiritWorld fw = make_world(300.0);
        const uint32_t military =
            add_spirit(fw.g.content, "military", 1, Json(), Json(),
                       mod_of(ModifierKind::DivisionAttack, 0.1));
        add_spirit(fw.g.content, "economy", 1, Json(), Json(),
                   mod_of(ModifierKind::ConstructionSpeed, 0.1));
        Country& c = *fw.g.world.country(fw.c);
        c.at_war = at_war;
        fw.g.set_ai(fw.c, true);
        run_day(fw, 0, true);
        CHECK_EQ(c.spirit_keys.size(), static_cast<size_t>(1));
        return c.spirit_keys[0] == military ? std::string("military") : std::string("economy");
    };

    CHECK_EQ(best_spirit_name(true), std::string("military"));
    CHECK_EQ(best_spirit_name(false), std::string("economy"));
}

// Two identical runs produce identical world hashes: the same advisor is appointed,
// the same spirit adopted, and the same modifiers land in the same order.
HOI_TEST(spirits_advisors_determinism_two_runs) {
    auto run = [](uint64_t seed) -> uint64_t {
        SpiritWorld fw = make_world(500.0, 4, 2);
        fw.g.world.world_seed = seed;
        fw.g.rng.seed(seed);
        Content& content = fw.g.content;

        // A chance-gated spirit makes RNG consumption (or its absence) observable.
        Json maybe = obj();
        maybe.set("chance", Json(0.5));
        add_spirit(content, "chancy", 1, maybe, Json(),
                   mod_of(ModifierKind::DivisionOrganization, 0.05));
        add_spirit(content, "industry", 1, pp_gte(0.0), Json(),
                   mod_of(ModifierKind::ConstructionSpeed, 0.1));
        add_spirit(content, "research", 1, Json(), Json(),
                   mod_of(ModifierKind::ResearchSpeed, 0.1));
        add_advisor(content, "chief", 150.0, Json(), mod_of(ModifierKind::DivisionAttack, 0.1));

        fw.g.set_ai(fw.c, true);
        for (int day = 0; day < 40; ++day) run_day(fw, day, true);
        return world_hash(fw.g);
    };

    const uint64_t first = run(777);
    const uint64_t second = run(777);
    CHECK_EQ(first, second);
}

// The raw grant skips the availability trigger but still enforces the slot
// invariant: a grant that does not fit fails and leaves the country untouched.
HOI_TEST(spirit_grant_skips_trigger_but_respects_slots) {
    SpiritWorld fw = make_world(0.0, 2, 3);
    Content& content = fw.g.content;
    const uint32_t gated = add_spirit(content, "gated", 1, pp_gte(100.0));
    const uint32_t big = add_spirit(content, "big", 2, pp_gte(100.0));
    Country& c = *fw.g.world.country(fw.c);

    // The command path is gated by the trigger; rejection leaves no state behind.
    CHECK(!spirit_add(fw.g, fw.c, gated));
    CHECK(c.spirit_keys.empty());
    CHECK(c.national_spirits.empty());
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 2);

    // The grant path hands it out despite the failing trigger.
    CHECK(spirit_grant(fw.g, fw.c, gated));
    CHECK(has_spirit(fw.g.world, fw.c, "gated"));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 1);
    CHECK(!spirit_grant(fw.g, fw.c, gated));  // idempotent

    // Slots are an invariant: a two-slot grant does not fit and changes nothing.
    CHECK(!spirit_grant(fw.g, fw.c, big));
    CHECK_EQ(c.spirit_keys.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(1));
    CHECK_EQ(spirit_slots_free(fw.g, fw.c), 1);
    CHECK(!spirit_available(fw.g, fw.c, big));
    CHECK(!spirit_add(fw.g, fw.c, big));
}

// The advisor grant is free and skips the trigger, but honors advisor capacity.
HOI_TEST(advisor_grant_skips_cost_and_trigger_but_respects_slots) {
    SpiritWorld fw = make_world(0.0, 6, 1);
    const uint32_t gated = add_advisor(fw.g.content, "gated", 150.0, pp_gte(500.0));
    const uint32_t second = add_advisor(fw.g.content, "second", 10.0);
    Country& c = *fw.g.world.country(fw.c);

    // Command path: unaffordable and trigger-gated, so nothing happens.
    CHECK(!advisor_available(fw.g, fw.c, gated));
    CHECK(!advisor_appoint(fw.g, fw.c, gated));
    CHECK(c.advisors.empty());
    CHECK_NEAR(c.political_power, 0.0, 1e-9);

    // Grant path: appointed for free despite the trigger and zero political power.
    CHECK(advisor_grant(fw.g, fw.c, gated));
    CHECK(has_advisor(fw.g.world, fw.c, "gated"));
    CHECK_NEAR(c.political_power, 0.0, 1e-9);
    CHECK(!advisor_grant(fw.g, fw.c, gated));  // idempotent

    // The only advisor slot is taken: a further grant fails and changes nothing.
    CHECK_EQ(advisor_slots_free(fw.g, fw.c), 0);
    CHECK(!advisor_grant(fw.g, fw.c, second));
    CHECK_EQ(c.advisors.size(), static_cast<size_t>(1));
    CHECK_EQ(c.national_spirits.size(), static_cast<size_t>(1));
}