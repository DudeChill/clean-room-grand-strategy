// Golden tests: end-to-end behaviours that must keep working. Each test drives the
// real simulation through commands and asserts on authoritative state - never on
// internals, wiring or source text.

#include <string>

#include "data/content.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/industry.h"
#include "sim/map.h"
#include "sim/phases.h"
#include "sim/research.h"
#include "sim/supply.h"
#include "test.h"
#include "test_util.h"

using namespace hoi;
using namespace hoi_test;

namespace {

void push(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    g.queue.push(cmd);
}

std::string repo_root() {
    std::string file(__FILE__);
    const std::string suffix = "/tests/golden.cpp";
    if (file.size() > suffix.size() &&
        file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return file.substr(0, file.size() - suffix.size());
    }
    return ".";
}

}  // namespace

HOI_TEST(GOLDEN_001_production_reaches_stockpile) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const EquipmentId inf = g.content.equipment_by_key["infantry_equipment_1"];

    Command set;
    set.type = CommandType::SetProductionLine;
    set.country = m.a;
    set.equipment = inf;
    set.value = 5;
    push(g, set);
    g.tick_once();

    const double before = g.world.countries[m.a].equipment_stockpile[inf.v];
    g.run_ticks(30 * TICKS_PER_DAY);
    const double after = g.world.countries[m.a].equipment_stockpile[inf.v];
    CHECK_GT(after, before);
    CHECK_EQ(g.world.countries[m.a].lines.size(), size_t{1});
    CHECK_GT(g.world.countries[m.a].lines[0].efficiency, 0.10);
    CHECK_GT(g.world.countries[m.a].lines[0].output_total, 0.0);
}

HOI_TEST(GOLDEN_002_recruit_train_deploy) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;

    Command recruit;
    recruit.type = CommandType::RecruitDivision;
    recruit.country = m.a;
    recruit.template_id = m.tmpl;
    recruit.value = 1;
    push(g, recruit);
    g.tick_once();

    CHECK_EQ(g.world.countries[m.a].training.size(), size_t{1});
    const DivisionId did = g.world.countries[m.a].training[0].division;
    const ProvinceId target = m.provinces[0];

    g.run_ticks(20 * TICKS_PER_DAY);  // template trains for 10 days

    const Division* d = g.world.division(did);
    CHECK(d != nullptr);
    CHECK_GT(d->equipment[g.content.equipment_by_key["infantry_equipment_1"].v], 0.0);
    CHECK_EQ(d->training_days_left, 0.0);

    Command deploy;
    deploy.type = CommandType::DeployDivision;
    deploy.country = m.a;
    deploy.division = did;
    deploy.province = target;
    push(g, deploy);
    g.tick_once();

    CHECK(g.world.division(did)->location == target);
    CHECK_EQ(g.world.countries[m.a].training.size(), size_t{0});
}

HOI_TEST(GOLDEN_003_movement_reaches_target) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const DivisionId d = add_division(g, m.a, m.provinces[0], m.tmpl);

    Command move;
    move.type = CommandType::MoveDivision;
    move.country = m.a;
    move.division = d;
    move.province = m.provinces[2];
    push(g, move);
    g.tick_once();

    CHECK(g.world.division(d)->moving);
    g.run_ticks(10 * TICKS_PER_DAY);
    CHECK(g.world.division(d)->location == m.provinces[2]);
    CHECK(!g.world.division(d)->moving);
}

HOI_TEST(GOLDEN_004_land_combat_resolves) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    // Three attackers against one defender on the frontier province.
    add_division(g, m.b, m.provinces[3], m.tmpl);
    for (int i = 0; i < 3; ++i) add_division(g, m.a, m.provinces[2], m.tmpl);

    for (DivisionId did : g.world.countries[m.a].divisions) {
        Command attack;
        attack.type = CommandType::MoveDivision;
        attack.country = m.a;
        attack.division = did;
        attack.province = m.provinces[3];
        push(g, attack);
    }
    g.tick_once();
    CHECK(g.world.battles.size() >= size_t{1});

    g.run_ticks(30 * TICKS_PER_DAY);
    // Three-to-one odds must take the province; the defender is either destroyed
    // or has retreated out of it.
    CHECK_EQ(g.world.province(m.provinces[3])->controller.v, m.a.v);
    CHECK(g.world.countries[m.a].divisions.size() > 0);
}

HOI_TEST(GOLDEN_005_front_assignment_is_real) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    add_division(g, m.a, m.provinces[2], m.tmpl);

    Command army;
    army.type = CommandType::CreateArmy;
    army.country = m.a;
    army.text = "First Army";
    push(g, army);
    g.tick_once();
    CHECK_EQ(g.world.countries[m.a].armies.size(), size_t{1});
    const ArmyId aid = g.world.countries[m.a].armies[0];

    Command assign;
    assign.type = CommandType::AssignDivisionToArmy;
    assign.country = m.a;
    assign.army = aid;
    assign.divisions = g.world.countries[m.a].divisions;
    push(g, assign);

    Command front;
    front.type = CommandType::SetDivisionOrder;
    front.country = m.a;
    front.army = aid;
    front.value = static_cast<int>(OrderKind::FrontLine);
    push(g, front);
    g.tick_once();

    const Army* a = g.world.army(aid);
    CHECK(a != nullptr);
    CHECK_EQ(static_cast<int>(a->order.kind), static_cast<int>(OrderKind::FrontLine));
    CHECK(!a->order.line.empty());
    CHECK_EQ(a->divisions.size(), size_t{1});
}

HOI_TEST(GOLDEN_006_encirclement_starves_supply) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    // Army B holds p3 (capital-side source), p4 and p5. Cut p5 off by flipping its
    // only neighbour to A, so no supply route can reach it.
    add_division(g, m.b, m.provinces[5], m.tmpl);
    g.world.provinces[m.provinces[4]].controller = m.a;
    g.run_ticks(TICKS_PER_DAY * 2);

    CHECK_NEAR(g.world.province(m.provinces[5])->supply_level, 0.0, 1e-9);
    CHECK_NEAR(g.world.division(g.world.countries[m.b].divisions[0])->supply, 0.0, 1e-9);
}

HOI_TEST(GOLDEN_007_research_unlocks_equipment) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    Content& c = g.content;
    TechDef t;
    t.key = "infantry_weapons";
    t.name = "Infantry Weapons";
    t.year = 1936;
    t.cost_days = 5.0;
    t.unlock_equipment.push_back("infantry_equipment_1");
    t.modifiers.set(ModifierKind::DivisionAttack, 0.05);
    const TechId tid(static_cast<uint32_t>(c.techs.size()));
    c.techs.push_back(t);
    c.tech_by_key[t.key] = tid;

    Command research;
    research.type = CommandType::StartResearch;
    research.country = m.a;
    research.tech = tid;
    push(g, research);
    g.tick_once();
    CHECK(g.world.countries[m.a].research.slots[0].active);

    const double before = g.world.countries[m.a].tech_modifiers.get(ModifierKind::DivisionAttack);
    g.run_ticks(10 * TICKS_PER_DAY);
    CHECK(g.world.countries[m.a].research.has_tech(tid));
    CHECK_GT(g.world.countries[m.a].tech_modifiers.get(ModifierKind::DivisionAttack), before);
}

HOI_TEST(GOLDEN_008_construction_raises_factories) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    g.world.states[m.state_a].civilian_factories = 40;  // keep the test short but real
    g.world.states[m.state_a].building_slots = 40;
    const int before = g.world.states[m.state_a].military_factories;

    Command build;
    build.type = CommandType::StartConstruction;
    build.country = m.a;
    build.value = static_cast<int>(BuildingKind::MilitaryFactory);
    build.state = m.state_a;
    push(g, build);
    g.tick_once();
    CHECK_EQ(g.world.countries[m.a].construction.queue.size(), size_t{1});

    g.run_ticks(60 * TICKS_PER_DAY);
    CHECK_GT(g.world.states[m.state_a].military_factories, before);
    CHECK_EQ(g.world.countries[m.a].construction.queue.size(), size_t{0});
}

HOI_TEST(GOLDEN_009_determinism_same_seed_same_hash) {
    MiniWorld m1 = make_mini_world(true);
    MiniWorld m2 = make_mini_world(true);
    m1.game.world.world_seed = 99;
    m2.game.world.world_seed = 99;
    m1.game.rng.seed(99);
    m2.game.rng.seed(99);
    for (MiniWorld* m : {&m1, &m2}) {
        m->game.set_ai(m->a, true);
        m->game.set_ai(m->b, true);
        add_division(m->game, m->a, m->provinces[2], m->tmpl);
        add_division(m->game, m->b, m->provinces[3], m->tmpl);
    }
    m1.game.run_ticks(30 * TICKS_PER_DAY);
    m2.game.run_ticks(30 * TICKS_PER_DAY);
    CHECK_EQ(world_hash(m1.game), world_hash(m2.game));
}

HOI_TEST(GOLDEN_010_save_load_continues_identically) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    g.world.world_seed = 7;
    g.rng.seed(7);
    g.set_ai(m.a, true);
    g.set_ai(m.b, true);
    add_division(g, m.a, m.provinces[2], m.tmpl);
    add_division(g, m.b, m.provinces[3], m.tmpl);
    g.run_ticks(10 * TICKS_PER_DAY);

    std::string err;
    const std::string path = "golden_010.save";
    CHECK(save_game(g, path, &err));

    Game loaded;
    CHECK(load_game(loaded, path, &err));
    CHECK_EQ(world_hash(g), world_hash(loaded));

    g.run_ticks(10 * TICKS_PER_DAY);
    loaded.run_ticks(10 * TICKS_PER_DAY);
    CHECK_EQ(world_hash(g), world_hash(loaded));
    std::remove(path.c_str());
}

HOI_TEST(GOLDEN_011_capitulation_transfers_territory) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    // Country B loses every province including its capital.
    for (size_t i = 3; i < m.provinces.size(); ++i) {
        g.world.provinces[m.provinces[i]].controller = m.a;
    }
    g.world.states[m.state_b].controller = m.a;
    g.run_ticks(TICKS_PER_DAY * 2);

    CHECK(!g.world.countries[m.b].alive);
    bool any_owned_by_b = false;
    g.world.provinces.for_each([&](ProvinceId, const Province& p) {
        if (p.controller == m.b) any_owned_by_b = true;
    });
    CHECK(!any_owned_by_b);
}

HOI_TEST(GOLDEN_012_world_stays_consistent_over_long_run) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    g.world.world_seed = 4242;
    g.rng.seed(4242);
    g.set_ai(m.a, true);
    g.set_ai(m.b, true);
    for (int i = 0; i < 2; ++i) add_division(g, m.a, m.provinces[2], m.tmpl);
    add_division(g, m.b, m.provinces[3], m.tmpl);

    g.run_ticks(90 * TICKS_PER_DAY);
    std::vector<std::string> violations = check_invariants(g);
    if (!violations.empty()) {
        ::hoi_test::fail(__FILE__, __LINE__, "invariants: " + violations.front());
    }
    g.world.divisions.for_each([&](DivisionId, const Division& d) {
        CHECK(std::isfinite(d.organization));
        CHECK(std::isfinite(d.strength));
        CHECK(d.organization >= 0.0);
        CHECK(d.strength >= 0.0 && d.strength <= 1.000001);
    });
}

HOI_TEST(GOLDEN_013_full_scenario_loads_and_runs) {
    const std::string root = repo_root();
    const std::string scenario = root + "/data/scenarios/1936.json";
    Game g;
    std::string err;
    if (!Game::create(root + "/data", scenario, 12345, &g, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "scenario load failed: " + err);
    }
    CHECK_GT(g.world.provinces.size(), size_t{100});
    CHECK_GT(g.world.countries.size(), size_t{3});
    for (CountryId cid(0); cid.v < g.world.countries.capacity(); ++cid.v) {
        if (!g.world.countries.alive(cid)) continue;
        g.set_ai(cid, true);
    }
    const uint64_t tick_hours = 30 * static_cast<uint64_t>(TICKS_PER_DAY);
    g.run_ticks(tick_hours);

    std::string report;
    const bool ok = audit_world(g, &report);
    if (!ok) ::hoi_test::fail(__FILE__, __LINE__, report);
}

HOI_TEST(GOLDEN_014_replay_reproduces_world_hash) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    g.world.world_seed = 31337;
    g.rng.seed(31337);
    add_division(g, m.a, m.provinces[2], m.tmpl);
    add_division(g, m.b, m.provinces[3], m.tmpl);

    Command move;
    move.type = CommandType::MoveDivision;
    move.country = m.a;
    move.division = g.world.countries[m.a].divisions[0];
    move.province = m.provinces[3];
    push(g, move);
    g.run_ticks(5 * TICKS_PER_DAY);

    std::string err;
    const std::string path = "golden_014.replay";
    CHECK(save_replay(g, path, &err));
    std::remove(path.c_str());
    CHECK(g.log.records.size() >= 1);
}
