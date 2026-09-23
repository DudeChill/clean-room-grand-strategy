// Naval invasion tests: hermetic hand-built worlds with a real coastline, driven
// through phase_movement + phase_naval_invasion so they prove the state machine
// (gather -> load -> cross -> land), not a private helper.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "game/game.h"
#include "save/save.h"
#include "sim/navy.h"
#include "sim/phases.h"
#include "sim/supply.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

// ------------------------------------------------------------------ builders --

EquipmentId add_ship_model(Content& c, const std::string& key, EquipmentCategory category,
                           double naval_attack, double torpedo_attack, double build_cost,
                           double max_strength, double speed) {
    EquipmentDef e;
    e.key = key;
    e.name = key;
    e.category = category;
    e.naval_attack = naval_attack;
    e.torpedo_attack = torpedo_attack;
    e.detection = 8.0;
    e.visibility = 20.0;
    e.build_cost = build_cost;
    e.max_strength = max_strength;
    e.speed = speed;
    e.manpower = 100.0;
    const EquipmentId id(static_cast<uint32_t>(c.equipment.size()));
    e.id = id;
    c.equipment.push_back(e);
    c.equipment_by_key[key] = id;
    return id;
}

Content make_navy_content() {
    Content c = hoi_test::make_test_content();
    add_ship_model(c, "destroyer_1", EquipmentCategory::Ship, 120.0, 60.0, 500.0, 200.0, 37.0);
    add_ship_model(c, "transport_1", EquipmentCategory::Ship, 0.0, 0.0, 300.0, 300.0, 12.0);
    add_ship_model(c, "convoy_1", EquipmentCategory::Convoy, 0.0, 0.0, 8.0, 40.0, 12.0);
    return c;
}

// A coastal world: VLA (attacker) holds `home` (port) and `mid` (inland), NOR holds
// `coast`. Both coast provinces touch the single sea zone `sea`; the two countries
// are at war.
struct InvasionFixture {
    Game game;
    CountryId a, b;
    StateId state_a, state_b;
    RegionId land_region, sea_region;
    ProvinceId home, mid, coast, sea;
    TemplateId tmpl;
    EquipmentId destroyer, transport, convoy;

    explicit InvasionFixture(bool at_war = true, double convoy_stock = 200.0) {
        game.content = make_navy_content();
        World& w = game.world;
        w.tick = 0;
        w.date = GameDate{1936, 1, 1, 0};

        Region lr;
        lr.name = "land";
        land_region = w.regions.create(lr);
        Region sr;
        sr.name = "sea_zone";
        sr.is_sea = true;
        sea_region = w.regions.create(sr);

        state_a = w.states.create();
        state_b = w.states.create();
        w.states[state_a].name = "state_a";
        w.states[state_b].name = "state_b";
        w.states[state_a].building_slots = 20;
        w.states[state_b].building_slots = 20;

        a = w.countries.create();
        b = w.countries.create();
        Country& ca = w.countries[a];
        ca.tag = "VLA";
        ca.name = "Valtia";
        ca.capital = state_a;
        ca.manpower = 500000.0;
        ca.equipment_stockpile.assign(game.content.equipment.size(), 0.0);
        ca.law_levels.assign(4, 0);
        ca.research.slots.resize(3);
        ca.starting_factories = 10;
        Country& cb = w.countries[b];
        cb.tag = "NOR";
        cb.name = "Norlund";
        cb.capital = state_b;
        cb.manpower = 500000.0;
        cb.equipment_stockpile.assign(game.content.equipment.size(), 0.0);
        cb.law_levels.assign(4, 0);
        cb.research.slots.resize(3);
        cb.starting_factories = 10;

        w.states[state_a].owner = a;
        w.states[state_a].controller = a;
        w.states[state_b].owner = b;
        w.states[state_b].controller = b;

        tmpl = game.content.template_id("infantry_template");
        game.content.templates[tmpl.v].country = a;

        destroyer = game.content.equipment_id("destroyer_1");
        transport = game.content.equipment_id("transport_1");
        convoy = game.content.equipment_id("convoy_1");
        ca.equipment_stockpile[convoy.v] = convoy_stock;

        // Land provinces.
        home = add_province(w, "home", state_a, land_region);
        mid = add_province(w, "mid", state_a, land_region);
        coast = add_province(w, "coast", state_b, land_region);
        for (ProvinceId p : {home, mid}) {
            w.provinces[p].owner = a;
            w.provinces[p].controller = a;
        }
        w.provinces[coast].owner = b;
        w.provinces[coast].controller = b;
        w.provinces[home].is_capital = true;
        w.provinces[home].supply_hub = true;
        w.provinces[home].naval_base = 3;
        w.provinces[home].coastal = true;
        w.provinces[coast].naval_base = 1;
        w.provinces[coast].coastal = true;
        w.states[state_a].provinces = {home, mid};
        w.states[state_b].provinces = {coast};
        link_provinces(w, home, mid);

        // The sea zone, linked from both coasts through sea_adj (loader convention).
        sea = add_province(w, "sea", StateId{}, sea_region);
        w.provinces[sea].is_sea = true;
        w.provinces[home].sea_adj.push_back(sea);
        w.provinces[coast].sea_adj.push_back(sea);
        w.provinces[sea].adj = {home, coast};

        game.ai_controlled.assign(w.countries.capacity(), 0);
        game.rng.seed(20240607);
        if (at_war) declare_war();
    }

    void declare_war() {
        World& w = game.world;
        War war;
        WarParticipant pa;
        pa.country = a;
        WarParticipant pb;
        pb.country = b;
        war.attackers.push_back(pa);
        war.defenders.push_back(pb);
        war.aggressor = a;
        war.start_tick = 0;
        const WarId wid = w.wars.create(war);
        w.countries[a].wars.push_back(wid);
        w.countries[b].wars.push_back(wid);
        w.countries[a].at_war = true;
        w.countries[b].at_war = true;
        Relation& rel = w.relation(a, b);
        rel.at_war = true;
        rel.value = -100.0;
    }

    ArmyId add_army(const std::vector<DivisionId>& divs) {
        World& w = game.world;
        Army army;
        army.country = a;
        army.name = "1st Army";
        const ArmyId id = w.armies.create(army);
        w.armies[id].divisions = divs;
        w.countries[a].armies.push_back(id);
        for (DivisionId d : divs) w.divisions[d].army = id;
        return id;
    }

    DivisionId add_army_division(ProvinceId province) {
        return add_division(game, a, province, tmpl);
    }

    // Enemy raiding task force at sea in the crossing zone.
    TaskForceId add_enemy_task_force(int ships) {
        World& w = game.world;
        TaskForce tf;
        tf.country = b;
        tf.name = "Raiders";
        tf.sea_region = sea_region;
        tf.mission = NavalMission::StrikeForce;
        tf.at_sea = true;
        const TaskForceId id = w.task_forces.create(tf);
        for (int i = 0; i < ships; ++i) {
            Ship s;
            s.country = b;
            s.equipment = destroyer;
            s.task_force = id;
            s.sea_region = sea_region;
            s.at_sea = true;
            s.strength = 1.0;
            s.organisation = 1.0;
            const ShipId sid = w.ships.create(s);
            w.task_forces[id].ships.push_back(sid);
        }
        return id;
    }

    double convoy_stock() const {
        const Country* c = game.world.country(a);
        if (!c || convoy.v >= c->equipment_stockpile.size()) return 0.0;
        return c->equipment_stockpile[convoy.v];
    }

    // One hour of the phases that matter for an invasion.
    void advance(int hours) {
        for (int i = 0; i < hours; ++i) {
            phase_movement(game);
            phase_naval_invasion(game);
            ++game.world.tick;
        }
    }
};

const NavalInvasion* find_invasion(const World& w, ArmyId army) {
    for (const NavalInvasion& inv : w.invasions) {
        if (inv.army == army) return &inv;
    }
    return nullptr;
}

int army_division_count(const World& w, ArmyId army) {
    const Army* a = w.army(army);
    if (!a) return 0;
    int n = 0;
    for (DivisionId d : a->divisions)
        if (w.divisions.alive(d)) ++n;
    return n;
}

void expect_clean_audit(const Game& g) {
    const std::vector<std::string> violations = check_invariants(g);
    if (!violations.empty()) ::hoi_test::fail(__FILE__, __LINE__, "world audit: " + violations.front());
}

}  // namespace

// The full lifecycle: the army gathers at the port, the convoy crosses over hours,
// the divisions land, the undefended coast changes hands into a beachhead source,
// the army returns to a front-line order, and the audit stays clean.
HOI_TEST(invasion_crosses_lands_and_takes_the_coast) {
    InvasionFixture f;
    ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid)});

    CHECK(start_naval_invasion(f.game, army, f.home, f.coast));
    CHECK_EQ(f.game.world.invasions.size(), size_t{1});
    CHECK(f.game.world.army(army)->order.kind == OrderKind::NavalInvasion);
    CHECK(find_invasion(f.game.world, army) != nullptr);
    CHECK(find_invasion(f.game.world, army)->progress == 0.0);

    f.advance(2);
    CHECK(find_invasion(f.game.world, army) != nullptr);
    CHECK(find_invasion(f.game.world, army)->progress == 0.0);  // still gathering

    // Gather + load + 12-hour crossing fits well inside 100 hours.
    f.advance(100);

    CHECK(find_invasion(f.game.world, army) == nullptr);
    CHECK_EQ(f.game.world.invasions.size(), size_t{0});
    CHECK_EQ(army_division_count(f.game.world, army), 2);
    CHECK_EQ(f.game.world.province(f.coast)->controller, f.a);
    CHECK(f.game.world.province(f.coast)->supply_hub);  // beachhead supply source
    CHECK(f.game.world.army(army)->order.kind == OrderKind::FrontLine);
    for (DivisionId d : f.game.world.army(army)->divisions) {
        CHECK_EQ(f.game.world.division(d)->location, f.coast);
        CHECK_EQ(f.game.world.division(d)->order, OrderKind::None);
    }
    // Shipping came home for the divisions that landed.
    CHECK_NEAR(f.convoy_stock(), 200.0, 1e-9);
    expect_clean_audit(f.game);

    // The landed divisions can actually draw supply through the beachhead.
    phase_supply(f.game);
    for (DivisionId d : f.game.world.army(army)->divisions) {
        CHECK_GT(f.game.world.division(d)->supply, 0.0);
    }
    expect_clean_audit(f.game);
}

// An offensive enemy task force in the crossing zone intercepts the convoy: the
// transports are destroyed and the divisions are lost with them.
HOI_TEST(invasion_convoy_intercepted_loses_divisions_and_shipping) {
    InvasionFixture f;
    f.add_enemy_task_force(6);
    ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid),
                              f.add_army_division(f.mid)});
    const double stock_before = f.convoy_stock();

    CHECK(start_naval_invasion(f.game, army, f.home, f.coast));
    f.advance(120);

    CHECK(find_invasion(f.game.world, army) == nullptr);
    CHECK_EQ(army_division_count(f.game.world, army), 0);  // every division lost at sea
    CHECK_EQ(f.game.world.country(f.a)->divisions.size(), size_t{0});
    // No landing happened.
    CHECK_EQ(f.game.world.province(f.coast)->controller, f.b);
    // The lost transports are real: their shipping never comes home.
    CHECK_LT(f.convoy_stock(), stock_before);
    expect_clean_audit(f.game);
}

// Cancelling while the convoy is at sea returns the army to the port alive and
// refunds the shipping.
HOI_TEST(invasion_cancel_mid_crossing_returns_army) {
    InvasionFixture f;
    ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid)});
    const double stock_before = f.convoy_stock();

    CHECK(start_naval_invasion(f.game, army, f.home, f.coast));

    // Run until the convoy has set out but has not yet landed.
    for (int i = 0; i < 200; ++i) {
        f.advance(1);
        const NavalInvasion* inv = find_invasion(f.game.world, army);
        if (inv && inv->progress > 0.0 && inv->progress < 1.0) break;
    }
    const NavalInvasion* inv = find_invasion(f.game.world, army);
    CHECK(inv != nullptr);
    CHECK_GT(inv->progress, 0.0);
    CHECK_LT(inv->progress, 1.0);

    cancel_naval_invasion(f.game, army);

    CHECK(find_invasion(f.game.world, army) == nullptr);
    CHECK_EQ(f.game.world.invasions.size(), size_t{0});
    CHECK_EQ(army_division_count(f.game.world, army), 2);
    CHECK(f.game.world.army(army)->order.kind == OrderKind::None);
    for (DivisionId d : f.game.world.army(army)->divisions) {
        CHECK(f.game.world.division(d)->location == f.home);
    }
    CHECK_NEAR(f.convoy_stock(), stock_before, 1e-9);
    expect_clean_audit(f.game);
}

// Shipping that disappears between launch and the load hour aborts the invasion
// without leaving the army stuck in a dead invasion order.
HOI_TEST(invasion_shipping_lost_at_load_aborts_cleanly) {
    InvasionFixture f;
    ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid)});

    CHECK(start_naval_invasion(f.game, army, f.home, f.coast));
    // Drain the convoy pool after launch, before the army has finished gathering.
    f.game.world.country(f.a)->equipment_stockpile[f.convoy.v] = 5.0;

    f.advance(100);

    CHECK(find_invasion(f.game.world, army) == nullptr);
    CHECK_EQ(f.game.world.invasions.size(), size_t{0});
    CHECK(f.game.world.army(army)->order.kind == OrderKind::None);
    CHECK_EQ(army_division_count(f.game.world, army), 2);
    for (DivisionId d : f.game.world.army(army)->divisions) {
        const Division* div = f.game.world.division(d);
        CHECK(div->order == OrderKind::None);
        CHECK(!div->order_target.valid());
        CHECK(!div->moving);
        CHECK(div->path.empty());
    }
    // The five units taken at the load attempt were handed straight back.
    CHECK_NEAR(f.convoy_stock(), 5.0, 1e-9);
    expect_clean_audit(f.game);
}

// A coast still held by a live garrison is not simply handed over: the divisions
// land, but control stays with the defenders until combat drives them off.
HOI_TEST(invasion_contested_landing_does_not_take_control) {
    InvasionFixture f;
    ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid)});
    const DivisionId defender = add_division(f.game, f.b, f.coast, f.tmpl);
    (void)defender;

    CHECK(start_naval_invasion(f.game, army, f.home, f.coast));
    f.advance(100);

    CHECK(find_invasion(f.game.world, army) == nullptr);
    CHECK_EQ(army_division_count(f.game.world, army), 2);
    CHECK(f.game.world.division(f.game.world.army(army)->divisions.front())->location == f.coast);
    CHECK_EQ(f.game.world.province(f.coast)->controller, f.b);  // garrison held
    expect_clean_audit(f.game);
}

// Two identical runs (including the interception draws) hash identically.
HOI_TEST(invasion_two_runs_hash_identically) {
    auto run = [](uint64_t* hash) {
        InvasionFixture f;
        f.add_enemy_task_force(3);
        ArmyId army = f.add_army({f.add_army_division(f.mid), f.add_army_division(f.mid)});
        CHECK(start_naval_invasion(f.game, army, f.home, f.coast));
        f.advance(60);
        *hash = world_hash(f.game);
    };
    uint64_t h1 = 0, h2 = 0;
    run(&h1);
    run(&h2);
    CHECK_EQ(h1, h2);
}

// Synthetic load: 400 ships in 30 sea zones, with 20 convoys simultaneously
// crossing. The invasion step is O(invasions x task forces in the crossed zone),
// not O(ships^2), so this stays far under the 1 ms/tick budget. The figure is
// printed for the record; the assertion is generous enough for slow CI hosts.
HOI_TEST(invasion_phase_benchmark_400_ships_30_zones) {
    InvasionFixture f;
    World& w = f.game.world;

    std::vector<RegionId> zones;
    for (int z = 0; z < 30; ++z) {
        Region sr;
        sr.name = "zone" + std::to_string(z);
        sr.is_sea = true;
        zones.push_back(w.regions.create(sr));
        const ProvinceId sp = add_province(w, sr.name, StateId{}, zones.back());
        w.provinces[sp].is_sea = true;
    }
    // 400 enemy ships as 30 task forces, all offensive but all outside the crossed
    // zone (so no convoy is lost and the loop runs a steady crossing).
    int placed = 0;
    for (int z = 0; z < 30; ++z) {
        TaskForce tf;
        tf.country = f.b;
        tf.name = "TF" + std::to_string(z);
        tf.sea_region = zones[static_cast<size_t>(z)];
        tf.mission = NavalMission::StrikeForce;
        tf.at_sea = true;
        const TaskForceId tid = w.task_forces.create(tf);
        const int n = (z == 0) ? 400 - 13 * 29 : 13;
        for (int i = 0; i < n; ++i) {
            Ship s;
            s.country = f.b;
            s.equipment = f.destroyer;
            s.task_force = tid;
            s.sea_region = zones[static_cast<size_t>(z)];
            s.at_sea = true;
            s.strength = 1.0;
            const ShipId sid = w.ships.create(s);
            w.task_forces[tid].ships.push_back(sid);
            ++placed;
        }
    }
    CHECK_EQ(placed, 400);

    // 20 armies already loaded and mid-crossing.
    const int convoys = 20;
    for (int i = 0; i < convoys; ++i) {
        ArmyId army = f.add_army({f.add_army_division(f.home), f.add_army_division(f.home)});
        NavalInvasion inv;
        inv.army = army;
        inv.country = f.a;
        inv.origin = f.home;
        inv.target = f.coast;
        inv.sea_region = f.sea_region;
        inv.progress = 0.5;
        inv.started = 0;
        inv.landed = false;
        w.invasions.push_back(inv);
    }
    CHECK_EQ(w.invasions.size(), size_t{convoys});

    const int hours = 300;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < hours; ++i) {
        // Keep every convoy mid-crossing for the duration of the measurement.
        for (NavalInvasion& inv : w.invasions) inv.progress = 0.5;
        phase_naval_invasion(f.game);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms_per_tick =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(hours);
    std::printf("  invasion phase: %.3f ms/tick (%d ships, 30 zones, %d convoys)\n",
                ms_per_tick, placed, convoys);
    CHECK_LT(ms_per_tick, 5.0);
    expect_clean_audit(f.game);
}