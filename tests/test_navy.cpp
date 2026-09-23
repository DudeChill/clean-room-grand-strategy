// Naval warfare tests: hermetic hand-built worlds, no scenario files. Each test is
// driven through phase_naval (or the command/query layer) so it proves the
// simulation, not a private helper.
//
// The fixtures build their own ship models with the statistics each test needs, so
// the expected numbers follow from the test's own values (the real content has its
// own scale; the engine reads everything from content).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/game.h"
#include "save/save.h"
#include "sim/commands.h"
#include "sim/navy.h"
#include "sim/phases.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

// ---------------------------------------------------------------- builders --

EquipmentId add_ship_model(Content& c, const std::string& key, EquipmentCategory category,
                           double naval_attack, double torpedo_attack, double sub_detection,
                           double detection, double visibility, double armor, double piercing,
                           double air_attack, double ground_attack, double max_strength,
                           double build_cost) {
    EquipmentDef e;
    e.key = key;
    e.name = key;
    e.category = category;
    e.naval_attack = naval_attack;
    e.torpedo_attack = torpedo_attack;
    e.sub_detection = sub_detection;
    e.detection = detection;
    e.visibility = visibility;
    e.armor = armor;
    e.piercing = piercing;
    e.air_attack = air_attack;      // AA guns
    e.ground_attack = ground_attack;  // carrier air group strike power
    e.max_strength = max_strength;
    e.build_cost = build_cost;
    e.fuel_use = 0.05;
    e.manpower = 100.0;
    e.supply_use = 0.5;
    const EquipmentId id(static_cast<uint32_t>(c.equipment.size()));
    e.id = id;
    c.equipment.push_back(e);
    c.equipment_by_key[key] = id;
    return id;
}

// Warship models with a navy's worth of statistics. `gunboat` has no sensors, so it
// can be parked in a zone without ever engaging — useful for control/raiding tests.
Content make_navy_content() {
    Content c = make_test_content();
    add_ship_model(c, "destroyer", EquipmentCategory::Ship, 20, 30, 12, 40, 20, 2, 20, 6, 0, 200,
                   500);
    add_ship_model(c, "cruiser", EquipmentCategory::Ship, 45, 20, 6, 40, 20, 8, 30, 8, 0, 500,
                   1200);
    add_ship_model(c, "battleship", EquipmentCategory::Ship, 220, 0, 2, 6, 20, 45, 120, 16, 0,
                   2000, 6000);
    add_ship_model(c, "carrier", EquipmentCategory::Ship, 10, 0, 4, 40, 20, 12, 15, 12, 60, 1200,
                   4500);
    add_ship_model(c, "submarine", EquipmentCategory::Ship, 4, 45, 0, 30, 10, 2, 10, 0, 0, 180,
                   350);
    add_ship_model(c, "transport", EquipmentCategory::Ship, 0, 0, 0, 4, 20, 1, 5, 0, 0, 300, 300);
    add_ship_model(c, "gunboat", EquipmentCategory::Ship, 20, 0, 0, 0, 0, 8, 20, 4, 0, 500, 400);
    add_ship_model(c, "convoy_1", EquipmentCategory::Convoy, 0, 0, 0, 0, 0, 1, 0, 0, 0, 40, 8);
    return c;
}

ProvinceId add_sea_province(World& w, const std::string& name, RegionId zone) {
    Province p;
    p.name = name;
    p.is_sea = true;
    p.region = zone;
    p.terrain = Terrain::Ocean;
    const ProvinceId id = w.provinces.create(p);
    w.provinces[id].id = id;
    w.regions[zone].provinces.push_back(id);
    return id;
}

ProvinceId add_coastal_port(World& w, const std::string& name, StateId st, RegionId land,
                            CountryId owner, int naval_base, ProvinceId sea) {
    const ProvinceId id = add_province(w, name, st, land);
    Province& p = w.provinces[id];
    p.owner = owner;
    p.controller = owner;
    p.naval_base = naval_base;
    p.coastal = true;
    if (sea.valid()) {
        p.sea_adj.push_back(sea);
        w.provinces[sea].adj.push_back(id);
    }
    w.states[st].provinces.push_back(id);
    w.regions[land].provinces.push_back(id);
    return id;
}

// Two countries, a land region, a chain of sea zones (zone1 - zone2 - zone3) and an
// isolated zone4, one naval base per country, and no fleets: tests either form them
// through form_task_force or build them directly.
struct NavyFixture {
    explicit NavyFixture(bool at_war) {
        game.content = make_navy_content();
        World& w = game.world;
        w.tick = 0;
        w.date = GameDate{1936, 1, 1, 0};

        land_region = w.regions.create(Region{});
        w.regions[land_region].id = land_region;
        w.regions[land_region].name = "land";
        zone1 = make_zone(w, "zone1");
        zone2 = make_zone(w, "zone2");
        zone3 = make_zone(w, "zone3");
        zone4 = make_zone(w, "zone4");

        state_a = w.states.create(State{});
        w.states[state_a].id = state_a;
        w.states[state_a].name = "state_a";
        w.states[state_a].building_slots = 20;
        state_b = w.states.create(State{});
        w.states[state_b].id = state_b;
        w.states[state_b].name = "state_b";
        w.states[state_b].building_slots = 20;

        a = add_country(w, "VLA", game.content.equipment.size());
        b = add_country(w, "NOR", game.content.equipment.size());
        w.countries[a].capital = state_a;
        w.countries[b].capital = state_b;
        w.states[state_a].owner = a;
        w.states[state_a].controller = a;
        w.states[state_b].owner = b;
        w.states[state_b].controller = b;

        sea1 = add_sea_province(w, "sea1", zone1);
        sea2 = add_sea_province(w, "sea2", zone2);
        sea3 = add_sea_province(w, "sea3", zone3);
        sea4 = add_sea_province(w, "sea4", zone4);
        link_sea(w, sea1, sea2);
        link_sea(w, sea2, sea3);

        port_a = add_coastal_port(w, "port_a", state_a, land_region, a, 2, sea1);
        port_b = add_coastal_port(w, "port_b", state_b, land_region, b, 2, sea2);
        inland = add_province(w, "inland", state_a, land_region);
        w.provinces[inland].owner = a;
        w.provinces[inland].controller = a;
        w.states[state_a].provinces.push_back(inland);
        w.regions[land_region].provinces.push_back(inland);
        link_provinces(w, port_a, inland);

        destroyer = game.content.equipment_id("destroyer");
        cruiser = game.content.equipment_id("cruiser");
        battleship = game.content.equipment_id("battleship");
        carrier = game.content.equipment_id("carrier");
        submarine = game.content.equipment_id("submarine");
        transport = game.content.equipment_id("transport");
        gunboat = game.content.equipment_id("gunboat");
        convoy = game.content.equipment_id("convoy_1");

        game.ai_controlled.assign(w.countries.capacity(), 0);
        if (at_war) {
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
            w.wars[wid].id = wid;
            w.countries[a].wars.push_back(wid);
            w.countries[b].wars.push_back(wid);
            w.countries[a].at_war = true;
            w.countries[b].at_war = true;
            Relation& rel = w.relation(a, b);
            rel.at_war = true;
            rel.value = -100.0;
        }
        game.rng.seed(4242);
    }

    static RegionId make_zone(World& w, const std::string& name) {
        Region r;
        r.name = name;
        r.is_sea = true;
        const RegionId id = w.regions.create(r);
        w.regions[id].id = id;
        return id;
    }

    static CountryId add_country(World& w, const std::string& tag, size_t equipment_count) {
        Country c;
        c.tag = tag;
        c.name = tag;
        c.manpower = 500000.0;
        c.equipment_stockpile.assign(equipment_count, 0.0);
        c.law_levels.assign(4, 0);
        c.research.slots.resize(3);
        c.research.slots_unlocked = 3;
        c.starting_factories = 10;
        return w.countries.create(c);
    }

    static void link_sea(World& w, ProvinceId x, ProvinceId y) {
        w.provinces[x].sea_adj.push_back(y);
        w.provinces[y].sea_adj.push_back(x);
        std::sort(w.provinces[x].sea_adj.begin(), w.provinces[x].sea_adj.end());
        std::sort(w.provinces[y].sea_adj.begin(), w.provinces[y].sea_adj.end());
    }

    FleetId ensure_fleet(CountryId country) {
        Country* c = game.world.country(country);
        for (FleetId id : c->fleets) {
            if (game.world.fleet(id) != nullptr) return id;
        }
        Fleet fleet;
        fleet.country = country;
        fleet.name = "Fleet";
        const FleetId id = game.world.fleets.create(fleet);
        game.world.fleets[id].id = id;
        c->fleets.push_back(id);
        return id;
    }

    TaskForceId add_task_force(CountryId country, ProvinceId port, RegionId zone,
                               NavalMission mission, bool at_sea) {
        TaskForce tf;
        tf.country = country;
        tf.fleet = ensure_fleet(country);
        tf.name = "TF";
        tf.port = port;
        tf.sea_region = zone;
        tf.mission = mission;
        tf.at_sea = at_sea;
        const TaskForceId id = game.world.task_forces.create(tf);
        game.world.task_forces[id].id = id;
        game.world.fleets[tf.fleet].task_forces.push_back(id);
        return id;
    }

    ShipId add_ship(CountryId country, TaskForceId tf, EquipmentId equipment, double strength,
                    double organisation = 1.0, double fuel = 1.0) {
        const TaskForce* force = game.world.task_force(tf);
        Ship s;
        s.country = country;
        s.equipment = equipment;
        s.task_force = tf;
        s.fleet = force->fleet;
        s.port = force->port;
        s.strength = strength;
        s.organisation = organisation;
        s.fuel = fuel;
        s.at_sea = force->at_sea;
        s.sea_region = force->at_sea ? force->sea_region : RegionId{};
        const ShipId id = game.world.ships.create(s);
        game.world.ships[id].id = id;
        game.world.ships[id].name = "Ship " + std::to_string(id.v + 1);
        game.world.task_forces[tf].ships.push_back(id);
        return id;
    }

    Game game;
    CountryId a;
    CountryId b;
    RegionId land_region;
    RegionId zone1;
    RegionId zone2;
    RegionId zone3;
    RegionId zone4;
    StateId state_a;
    StateId state_b;
    ProvinceId port_a;
    ProvinceId port_b;
    ProvinceId sea1;
    ProvinceId sea2;
    ProvinceId sea3;
    ProvinceId sea4;
    ProvinceId inland;
    EquipmentId destroyer;
    EquipmentId cruiser;
    EquipmentId battleship;
    EquipmentId carrier;
    EquipmentId submarine;
    EquipmentId transport;
    EquipmentId gunboat;
    EquipmentId convoy;
};

double task_force_total_strength(const Game& g, TaskForceId id) {
    double sum = 0.0;
    const TaskForce* tf = g.world.task_force(id);
    if (tf == nullptr) return 0.0;
    for (ShipId sid : tf->ships) {
        const Ship* s = g.world.ship(sid);
        if (s != nullptr) sum += s->strength;
    }
    return sum;
}

// --------------------------------------------------- forming task forces ----

// A task force forms at a port, draws the ships out of the stockpile, joins the
// country's fleet roster and names every hull; short stock, wrong equipment, an
// unusable port and a full base are all rejected.
HOI_TEST(navy_form_task_force_draws_from_stockpile) {
    NavyFixture f(false);
    Game& g = f.game;
    g.world.country(f.a)->equipment_stockpile[f.destroyer.v] = 6.0;

    const TaskForceId tf = form_task_force(g, f.a, f.port_a, f.destroyer, 4, "Home Fleet");
    CHECK(tf.valid());
    const TaskForce* force = g.world.task_force(tf);
    CHECK(force != nullptr);
    CHECK_EQ(force->ships.size(), 4u);
    CHECK(force->fleet.valid());
    CHECK(g.world.fleet(force->fleet) != nullptr);
    CHECK_EQ(force->port, f.port_a);
    CHECK_EQ(force->sea_region, f.zone1);  // the sea zone off its port
    CHECK(!force->at_sea);

    // Fleet roster and country roster are consistent.
    const Fleet* fleet = g.world.fleet(force->fleet);
    CHECK(std::find(fleet->task_forces.begin(), fleet->task_forces.end(), tf) !=
          fleet->task_forces.end());
    CHECK_EQ(g.world.country(f.a)->fleets.size(), 1u);
    CHECK_EQ(g.world.country(f.a)->fleets.front(), force->fleet);

    // Individual ships with names and valid links.
    std::vector<std::string> names;
    for (ShipId sid : force->ships) {
        const Ship* s = g.world.ship(sid);
        CHECK(s != nullptr);
        CHECK_EQ(s->country, f.a);
        CHECK_EQ(s->task_force, tf);
        CHECK_EQ(s->fleet, force->fleet);
        CHECK_EQ(s->port, f.port_a);
        CHECK(!s->name.empty());
        names.push_back(s->name);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());

    CHECK_NEAR(g.world.country(f.a)->equipment_stockpile[f.destroyer.v], 2.0, 1e-9);

    // Fewer ships in stock than requested: the command fails and draws nothing.
    CHECK(!form_task_force(g, f.a, f.port_a, f.destroyer, 6, "").valid());
    CHECK_NEAR(g.world.country(f.a)->equipment_stockpile[f.destroyer.v], 2.0, 1e-9);

    // Non-ship equipment and an enemy port are rejected.
    CHECK(!form_task_force(g, f.a, f.port_a, g.content.equipment_id("infantry_equipment_1"), 1, "")
               .valid());
    CHECK(!form_task_force(g, f.a, f.port_b, f.destroyer, 1, "").valid());
    CHECK(!form_task_force(g, f.a, f.port_a, f.destroyer, 0, "").valid());

    // A level-2 base hosts 16 ships; 4 are home, so a 20-ship ask takes the last 12.
    g.world.country(f.a)->equipment_stockpile[f.destroyer.v] = 40.0;
    const TaskForceId second = form_task_force(g, f.a, f.port_a, f.destroyer, 20, "");
    CHECK(second.valid());
    CHECK_EQ(g.world.task_force(second)->ships.size(), 12u);
    // Nothing more fits.
    CHECK(!form_task_force(g, f.a, f.port_a, f.destroyer, 1, "").valid());
    CHECK_EQ(naval_base_capacity(g, f.port_a), 16);
}

// ------------------------------------------------------------- queries ------

HOI_TEST(navy_port_capacity_and_sea_geometry) {
    NavyFixture f(false);
    Game& g = f.game;

    CHECK_EQ(naval_base_capacity(g, f.port_a), 16);  // level 2 * 8 per level
    CHECK_EQ(naval_base_capacity(g, f.sea1), 0);
    CHECK_EQ(naval_base_capacity(g, f.inland), 0);
    CHECK_EQ(naval_base_capacity(g, ProvinceId{}), 0);

    CHECK(is_usable_port(g, f.a, f.port_a));
    CHECK(!is_usable_port(g, f.a, f.inland));   // no naval base
    CHECK(!is_usable_port(g, f.a, f.port_b));   // another country's port
    CHECK(!is_usable_port(g, f.b, f.port_a));
    // Military access makes a co-belligerent's port usable.
    g.world.relation(f.a, f.b).military_access = true;
    CHECK(is_usable_port(g, f.a, f.port_b));

    CHECK_EQ(adjacent_sea_region(g, f.port_a), f.zone1);
    CHECK_EQ(adjacent_sea_region(g, f.port_b), f.zone2);
    CHECK(!adjacent_sea_region(g, f.inland).valid());
    CHECK(!adjacent_sea_region(g, ProvinceId{}).valid());

    CHECK_EQ(sea_region_distance(g, f.zone1, f.zone1), 0);
    CHECK_EQ(sea_region_distance(g, f.zone1, f.zone2), 1);
    CHECK_EQ(sea_region_distance(g, f.zone1, f.zone3), 2);
    CHECK_EQ(sea_region_distance(g, f.zone1, f.zone4), -1);  // isolated zone
    CHECK_EQ(sea_region_distance(g, RegionId{}, f.zone2), -1);
    CHECK_EQ(sea_region_distance(g, f.zone1, RegionId{}), -1);
}

HOI_TEST(navy_task_force_stats_sum_ship_statistics) {
    NavyFixture f(false);
    Game& g = f.game;
    const TaskForceId tf = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::Patrol, false);
    f.add_ship(f.a, tf, f.destroyer, 1.0);
    f.add_ship(f.a, tf, f.destroyer, 0.5);
    f.add_ship(f.a, tf, f.battleship, 1.0);

    const TaskForceStats st = task_force_stats(g, tf);
    CHECK_EQ(st.ships, 3);
    CHECK_NEAR(st.naval_attack, 20.0 + 20.0 + 220.0, 1e-9);
    CHECK_NEAR(st.torpedo_attack, 30.0 + 30.0 + 0.0, 1e-9);
    CHECK_NEAR(st.hull, 200.0 + 100.0 + 2000.0, 1e-9);  // strength-weighted
    CHECK_NEAR(st.armour, 2.0 + 2.0 + 45.0, 1e-9);
    CHECK_EQ(task_force_stats(g, TaskForceId{}).ships, 0);
}

// ----------------------------------------------------------- sea combat -----

// Two hostile task forces in one sea zone detect each other and fight: the weaker
// force loses strength and withdraws to its port while the stronger one holds the
// zone.
HOI_TEST(navy_hostile_forces_fight_and_weaker_retreats) {
    NavyFixture f(true);
    Game& g = f.game;

    const TaskForceId strong =
        f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::StrikeForce, true);
    for (int i = 0; i < 3; ++i) f.add_ship(f.a, strong, f.destroyer, 1.0);
    const TaskForceId weak = f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::Patrol, true);
    const ShipId lone = f.add_ship(f.b, weak, f.cruiser, 1.0);
    g.world.tick = 1;  // so a recorded engagement is distinguishable from "never"

    for (int i = 0; i < 500; ++i) {
        phase_naval(g);
        const TaskForce* t = g.world.task_force(weak);
        if (t == nullptr || !t->at_sea) break;
    }

    const TaskForce* weak_tf = g.world.task_force(weak);
    CHECK(weak_tf != nullptr);
    CHECK(!weak_tf->at_sea);                       // withdrew
    CHECK_EQ(weak_tf->port, f.port_b);             // to its own port
    CHECK_EQ(weak_tf->mission, NavalMission::Patrol);  // the order stays set
    CHECK(g.world.task_force(strong)->at_sea);     // the stronger force holds the zone
    const Ship* s = g.world.ship(lone);
    CHECK(s != nullptr);
    CHECK_LT(s->strength, 1.0);
    CHECK_GT(task_force_total_strength(g, strong), task_force_total_strength(g, weak));
    // The engagement was recorded on the task force that stayed in the zone.
    CHECK_GT(g.world.task_force(strong)->last_engagement, 0u);
}

// A ship at strength 0 is removed: it leaves the ship store, its task force roster
// and the fleet roster (the empty task force leaves with it). destroy_ship is
// exercised directly as well, without the RNG.
HOI_TEST(navy_sunk_ship_leaves_every_roster) {
    NavyFixture f(true);
    Game& g = f.game;

    const FleetId fleet_b = f.ensure_fleet(f.b);
    const TaskForceId victim = f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::Patrol, true);
    const ShipId doomed = f.add_ship(f.b, victim, f.destroyer, 0.05);
    const ShipId partner = f.add_ship(f.b, victim, f.destroyer, 1.0);

    const TaskForceId hunter =
        f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::StrikeForce, true);
    for (int i = 0; i < 6; ++i) f.add_ship(f.a, hunter, f.destroyer, 1.0);

    for (int i = 0; i < 800 && g.world.ship(doomed) != nullptr; ++i) phase_naval(g);

    CHECK(!g.world.ship(doomed));  // gone from the world
    CHECK(g.world.ship(partner) != nullptr);  // its sister survived
    // The task force lost one ship but still exists, and the fleet still lists it.
    const TaskForce* tf = g.world.task_force(victim);
    CHECK(tf != nullptr);
    CHECK(std::find(tf->ships.begin(), tf->ships.end(), doomed) == tf->ships.end());
    CHECK(std::find(tf->ships.begin(), tf->ships.end(), partner) != tf->ships.end());
    const Fleet* fleet = g.world.fleet(fleet_b);
    CHECK(std::find(fleet->task_forces.begin(), fleet->task_forces.end(), victim) !=
          fleet->task_forces.end());

    // Direct removal: the second ship takes the task force with it, leaving the
    // fleet roster clean.
    destroy_ship(g, partner);
    CHECK(!g.world.ship(partner));
    CHECK(g.world.task_force(victim) == nullptr);
    const Fleet* fleet_after = g.world.fleet(fleet_b);
    CHECK(std::find(fleet_after->task_forces.begin(), fleet_after->task_forces.end(), victim) ==
          fleet_after->task_forces.end());
    destroy_ship(g, doomed);  // idempotent on an already-destroyed id
}

// --------------------------------------------------------------- repair -----

// Repair in port restores strength and organisation over time, and costs the
// country real fuel and spare parts from the ship's own model stockpile.
HOI_TEST(navy_repair_in_port_restores_strength_and_costs) {
    NavyFixture f(false);
    Game& g = f.game;
    g.world.country(f.a)->fuel = 100.0;
    g.world.country(f.a)->equipment_stockpile[f.destroyer.v] = 10.0;

    const TaskForceId tf = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::None, false);
    const ShipId ship = f.add_ship(f.a, tf, f.destroyer, 0.5, 0.5, 1.0);

    phase_naval(g);

    CHECK_GT(g.world.ship(ship)->strength, 0.5);
    CHECK_GT(g.world.ship(ship)->organisation, 0.5);
    CHECK_LT(g.world.country(f.a)->fuel, 100.0);  // the dockyard work was paid for
    CHECK_LT(g.world.country(f.a)->equipment_stockpile[f.destroyer.v], 10.0);  // spare parts
    CHECK(!g.world.task_force(tf)->at_sea);  // no mission, no sortie

    const double strength_after_one_hour = g.world.ship(ship)->strength;
    for (int i = 0; i < 300; ++i) phase_naval(g);
    CHECK_GT(g.world.ship(ship)->strength, strength_after_one_hour);
    CHECK_NEAR(g.world.ship(ship)->strength, 1.0, 1e-9);  // capped at full strength
    CHECK_GT(g.world.ship(ship)->organisation, 0.99);
}

// -------------------------------------------------------- convoy raiding ----

// Convoy raiding sinks enemy convoys out of their stockpile and cuts the raided
// country's naval control in the zone; escorts reduce the toll.
HOI_TEST(navy_convoy_raiding_sinks_convoys_and_cuts_control) {
    NavyFixture f(true);
    Game& g = f.game;
    g.world.country(f.b)->equipment_stockpile[f.convoy.v] = 100.0;

    const TaskForceId raider =
        f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::ConvoyRaid, true);
    f.add_ship(f.a, raider, f.gunboat, 1.0);
    f.add_ship(f.a, raider, f.gunboat, 1.0);
    const TaskForceId escort =
        f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::ConvoyEscort, true);
    f.add_ship(f.b, escort, f.gunboat, 1.0);

    for (int i = 0; i < 24; ++i) phase_naval(g);

    CHECK_LT(g.world.country(f.b)->equipment_stockpile[f.convoy.v], 100.0);
    CHECK_GT(g.world.country(f.b)->equipment_stockpile[f.convoy.v], 0.0);

    const double raided_share = naval_control_share(g, f.b, f.zone1);
    // The same battle without a raider: the convoy escort keeps full control.
    NavyFixture plain(true);
    plain.game.world.country(plain.b)->equipment_stockpile[plain.convoy.v] = 100.0;
    const TaskForceId escort_only =
        plain.add_task_force(plain.b, plain.port_b, plain.zone1, NavalMission::ConvoyEscort, true);
    plain.add_ship(plain.b, escort_only, plain.gunboat, 1.0);
    for (int i = 0; i < 24; ++i) phase_naval(plain.game);
    CHECK_GT(naval_control_share(plain.game, plain.b, plain.zone1), raided_share);

    // Standing the raider down empties the zone of its presence.
    g.world.task_force(raider)->mission = NavalMission::None;
    phase_naval(g);
    CHECK(!g.world.task_force(raider)->at_sea);
    CHECK_GT(naval_control_share(g, f.b, f.zone1), raided_share);

    // Escort protection: without the escort the same raid takes more convoys.
    NavyFixture unescorted(true);
    unescorted.game.world.country(unescorted.b)->equipment_stockpile[unescorted.convoy.v] = 100.0;
    const TaskForceId lone_raider = unescorted.add_task_force(
        unescorted.a, unescorted.port_a, unescorted.zone1, NavalMission::ConvoyRaid, true);
    unescorted.add_ship(unescorted.a, lone_raider, unescorted.gunboat, 1.0);
    unescorted.add_ship(unescorted.a, lone_raider, unescorted.gunboat, 1.0);
    for (int i = 0; i < 24; ++i) phase_naval(unescorted.game);
    const double lost_escorted = 100.0 - g.world.country(f.b)->equipment_stockpile[f.convoy.v];
    const double lost_unescorted =
        100.0 - unescorted.game.world.country(unescorted.b)->equipment_stockpile[unescorted.convoy.v];
    CHECK_GT(lost_unescorted, lost_escorted);
}

// -------------------------------------------------------- naval control -----

// Control shares come from the ships at sea, are written sorted by country id, sum
// to at most one, and are zero for missions that do not contest a zone.
HOI_TEST(navy_control_shares_are_derived_and_sorted) {
    NavyFixture f(false);
    Game& g = f.game;
    const TaskForceId ta = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::Patrol, true);
    f.add_ship(f.a, ta, f.destroyer, 1.0);
    f.add_ship(f.a, ta, f.destroyer, 1.0);
    const TaskForceId tb = f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::Patrol, true);
    f.add_ship(f.b, tb, f.destroyer, 1.0);

    phase_naval(g);

    const Region* zone = g.world.regions.try_get(f.zone1);
    CHECK(zone != nullptr);
    CHECK_EQ(zone->naval_control.size(), 2u);
    CHECK(zone->naval_control[0].first.v < zone->naval_control[1].first.v);
    double sum = 0.0;
    for (const auto& entry : zone->naval_control) sum += entry.second;
    CHECK(sum <= 1.0 + 1e-9);
    CHECK_GT(naval_control_share(g, f.a, f.zone1), naval_control_share(g, f.b, f.zone1));
    CHECK_NEAR(naval_control_share(g, f.a, f.zone1), 2.0 / 3.0, 1e-9);
    CHECK_EQ(naval_control_share(g, CountryId{}, f.zone1), 0.0);

    // A training force does not contest the zone; a None force does not either.
    for (NavalMission m : {NavalMission::Training, NavalMission::None}) {
        g.world.task_force(tb)->mission = m;
        phase_naval(g);
        CHECK_EQ(naval_control_share(g, f.b, f.zone1), 0.0);
        CHECK_NEAR(naval_control_share(g, f.a, f.zone1), 1.0, 1e-9);
        g.world.task_force(tb)->at_sea = true;  // keep it in the zone for the next round
        g.world.task_force(tb)->mission = NavalMission::Patrol;
    }
}

// Carrier air groups strike through the air model's ground-attack values, and the
// target's anti-air guns cut the damage.
HOI_TEST(navy_carrier_air_group_damage_and_anti_air) {
    NavyFixture f(true);
    Game& g = f.game;
    const TaskForceId carriers =
        f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::StrikeForce, true);
    for (int i = 0; i < 4; ++i) f.add_ship(f.a, carriers, f.carrier, 1.0);

    const TaskForceId target = f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::Patrol, true);
    const ShipId victim = f.add_ship(f.b, target, f.cruiser, 1.0);

    for (int i = 0; i < 200 && g.world.ship(victim)->strength >= 1.0; ++i) phase_naval(g);
    const double plain_loss = 1.0 - g.world.ship(victim)->strength;
    CHECK_GT(plain_loss, 0.0);

    // The same strike against a hull with heavy flak does less damage per hour.
    // Measured on the first hour of contact for both, with the same seed.
    NavyFixture flak(true);
    flak.game.content.equipment[flak.cruiser.v].air_attack = 60.0;
    const TaskForceId flak_carriers =
        flak.add_task_force(flak.a, flak.port_a, flak.zone1, NavalMission::StrikeForce, true);
    for (int i = 0; i < 4; ++i) flak.add_ship(flak.a, flak_carriers, flak.carrier, 1.0);
    const TaskForceId flak_target =
        flak.add_task_force(flak.b, flak.port_b, flak.zone1, NavalMission::Patrol, true);
    const ShipId flak_victim = flak.add_ship(flak.b, flak_target, flak.cruiser, 1.0);
    for (int i = 0; i < 200 && flak.game.world.ship(flak_victim)->strength >= 1.0; ++i) {
        phase_naval(flak.game);
    }
    const double flak_loss = 1.0 - flak.game.world.ship(flak_victim)->strength;
    CHECK_GT(plain_loss, flak_loss);
}

// ------------------------------------------------------------- fuel ---------

// Bunker fuel burns at sea and an empty bunker takes the whole task force back to
// port, where the country's fuel pool pays to refill it.
HOI_TEST(navy_fuel_burn_forces_return_to_port) {
    // With a fuel pool the ship burns out its bunker, returns to port, and is
    // refuelled out of the country's fuel.
    NavyFixture f(true);
    Game& g = f.game;
    g.content.constants.naval_fuel_use_per_hour = 0.5;  // one hour of steaming empties it
    g.world.country(f.a)->fuel = 50.0;
    const TaskForceId tf = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::Patrol, true);
    const ShipId ship = f.add_ship(f.a, tf, f.destroyer, 1.0, 1.0, 0.02);

    phase_naval(g);

    CHECK(!g.world.ship(ship)->at_sea);               // the empty bunker sent it home
    CHECK(!g.world.task_force(tf)->at_sea);
    CHECK_NEAR(g.world.ship(ship)->fuel, 1.0, 1e-9);  // refuelled in port
    CHECK_LT(g.world.country(f.a)->fuel, 50.0);       // and the country paid for it

    // Without a fuel pool the same force stays in port with dry bunkers.
    NavyFixture dry(true);
    dry.game.content.constants.naval_fuel_use_per_hour = 0.5;
    dry.game.world.country(dry.a)->fuel = 0.0;
    const TaskForceId dry_tf =
        dry.add_task_force(dry.a, dry.port_a, dry.zone1, NavalMission::Patrol, true);
    const ShipId dry_ship = dry.add_ship(dry.a, dry_tf, dry.destroyer, 1.0, 1.0, 0.02);

    phase_naval(dry.game);

    CHECK(!dry.game.world.ship(dry_ship)->at_sea);
    CHECK(!(dry.game.world.ship(dry_ship)->fuel > 0.0));
    CHECK(!dry.game.world.task_force(dry_tf)->at_sea);
    CHECK_NEAR(dry.game.world.country(dry.a)->fuel, 0.0, 1e-12);
}

// ------------------------------------------------------------- training -----

// Training missions gain experience without fighting even with a hostile force in
// the same zone.
HOI_TEST(navy_training_gains_experience_without_combat) {
    NavyFixture f(true);
    Game& g = f.game;
    const TaskForceId trainer =
        f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::Training, true);
    const ShipId trainee = f.add_ship(f.a, trainer, f.destroyer, 1.0);
    const TaskForceId hostile =
        f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::StrikeForce, true);
    const ShipId enemy = f.add_ship(f.b, hostile, f.battleship, 1.0);

    for (int i = 0; i < 24; ++i) phase_naval(g);

    CHECK_GT(g.world.ship(trainee)->experience, 0.0);
    CHECK_NEAR(g.world.ship(trainee)->strength, 1.0, 1e-9);   // never fired at
    CHECK_NEAR(g.world.ship(enemy)->strength, 1.0, 1e-9);
    CHECK_EQ(g.world.task_force(hostile)->last_engagement, 0u);
}

// --------------------------------------------------------- displacement -----

// A task force whose port is captured moves to the nearest friendly port; with no
// port left at all, its ships are scuttled.
HOI_TEST(navy_displaced_when_port_falls) {
    NavyFixture f(true);
    Game& g = f.game;
    // A second friendly port one land hop inland, so the displaced force has
    // somewhere to go before that one falls too.
    const ProvinceId spare =
        add_coastal_port(g.world, "spare", f.state_a, f.land_region, f.a, 2, f.sea3);
    link_provinces(g.world, f.inland, spare);

    const TaskForceId tf = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::Patrol, true);
    const ShipId ship = f.add_ship(f.a, tf, f.destroyer, 1.0);
    g.world.provinces[f.port_a].controller = f.b;  // the base falls

    phase_naval(g);

    CHECK(g.world.ship(ship) != nullptr);
    CHECK_EQ(g.world.task_force(tf)->port, spare);
    CHECK(!g.world.task_force(tf)->at_sea);  // displaced forces lie in port
    CHECK_EQ(g.world.ship(ship)->port, spare);

    // With every friendly port gone the ships are scuttled rather than left without
    // a base: the empty task force leaves with them.
    g.world.provinces[spare].controller = f.b;
    phase_naval(g);
    CHECK(g.world.ship(ship) == nullptr);
    CHECK(g.world.task_force(tf) == nullptr);
}

// ------------------------------------------------------------- command ------

// The CreateTaskForce command reaches form_task_force through the command layer and
// reports success/failure as a CommandResult.
HOI_TEST(navy_create_task_force_command) {
    NavyFixture f(false);
    Game& g = f.game;
    g.world.country(f.a)->equipment_stockpile[f.cruiser.v] = 3.0;

    Command cmd;
    cmd.type = CommandType::CreateTaskForce;
    cmd.country = f.a;
    cmd.province = f.port_a;
    cmd.equipment = f.cruiser;
    cmd.value = 2;
    cmd.text = "Strike Force";
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);
    CHECK_EQ(apply_command(g, cmd), CommandResult::Applied);
    CHECK_EQ(g.world.country(f.a)->fleets.size(), 1u);

    // Too few cruisers left for another force of two: validation cannot see the
    // stockpile, application refuses to form the force.
    cmd.value = 2;
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);
    CHECK_EQ(apply_command(g, cmd), CommandResult::InsufficientResources);
    CHECK_EQ(g.world.country(f.a)->fleets.size(), 1u);
}

// ---------------------------------------------------------- determinism -----

namespace {
// Folds only the state the naval phase owns plus the live RNG stream, so a future
// divergence points at the naval phase rather than at the whole world.
uint64_t hash_navy_state(const Game& g) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    auto mix_double = [&mix](double v) {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        mix(bits);
    };
    g.world.ships.for_each([&](ShipId id, const Ship& s) {
        mix(id.v);
        mix(s.country.v);
        mix(s.equipment.v);
        mix(s.task_force.v);
        mix_double(s.strength);
        mix_double(s.organisation);
        mix_double(s.experience);
        mix_double(s.fuel);
        mix(s.at_sea ? 1 : 0);
        mix(s.sea_region.v);
    });
    g.world.task_forces.for_each([&](TaskForceId id, const TaskForce& tf) {
        mix(id.v);
        mix(tf.country.v);
        mix(static_cast<uint64_t>(tf.mission));
        mix(tf.at_sea ? 1 : 0);
        mix(tf.sea_region.v);
        mix_double(tf.detection);
        for (ShipId sid : tf.ships) mix(sid.v);
    });
    g.world.regions.for_each([&](RegionId, const Region& r) {
        for (const auto& entry : r.naval_control) {
            mix(entry.first.v);
            mix_double(entry.second);
        }
    });
    g.world.countries.for_each([&](CountryId, const Country& c) {
        mix_double(c.fuel);
    });
    mix(g.rng.state_hash());
    return h;
}

struct NavyRunHashes {
    uint64_t world = 0;
    uint64_t navy = 0;
};

NavyRunHashes navy_run() {
    NavyFixture f(true);
    Game& g = f.game;
    g.world.world_seed = 99;
    g.world.country(f.a)->fuel = 500.0;
    g.world.country(f.b)->fuel = 500.0;
    g.world.country(f.b)->equipment_stockpile[f.convoy.v] = 200.0;
    const TaskForceId ta = f.add_task_force(f.a, f.port_a, f.zone1, NavalMission::StrikeForce, true);
    for (int i = 0; i < 6; ++i) f.add_ship(f.a, ta, f.destroyer, 1.0);
    f.add_ship(f.a, ta, f.cruiser, 1.0);
    const TaskForceId tb = f.add_task_force(f.b, f.port_b, f.zone1, NavalMission::ConvoyRaid, true);
    for (int i = 0; i < 3; ++i) f.add_ship(f.b, tb, f.submarine, 1.0);
    const TaskForceId tc = f.add_task_force(f.b, f.port_b, f.zone2, NavalMission::Patrol, true);
    f.add_ship(f.b, tc, f.battleship, 1.0);
    f.add_ship(f.b, tc, f.carrier, 1.0);
    g.rng.seed(20240923);
    for (int i = 0; i < 48; ++i) phase_naval(g);
    NavyRunHashes hashes;
    hashes.world = world_hash(g);
    hashes.navy = hash_navy_state(g);
    return hashes;
}
}  // namespace

HOI_TEST(navy_phase_is_deterministic) {
    const NavyRunHashes first = navy_run();
    const NavyRunHashes second = navy_run();
    CHECK_EQ(first.world, second.world);
    CHECK_EQ(first.navy, second.navy);
}

// ----------------------------------------------------------- benchmark ------

// Synthetic scale check: 400 ships in 30 sea zones, two hostile countries, every
// task force at sea. Prints ms/tick and fails if the phase exceeds 1 ms/tick on
// this machine's baseline (the acceptance target).
HOI_TEST(navy_phase_benchmark_400_ships_30_zones) {
    NavyFixture f(true);
    Game& g = f.game;
    World& w = g.world;
    w.countries[f.a].fuel = 1e9;
    g.world.country(f.b)->fuel = 1e9;
    g.world.country(f.b)->equipment_stockpile[f.convoy.v] = 1e6;

    // 30 sea zones in a chain, one sea province each, off the fixture's two ports.
    std::vector<RegionId> zones;
    zones.push_back(f.zone1);
    zones.push_back(f.zone2);
    for (int i = 2; i < 30; ++i) {
        const RegionId zone = NavyFixture::make_zone(w, "bench_zone" + std::to_string(i));
        const ProvinceId sea = add_sea_province(w, "bench_sea" + std::to_string(i), zone);
        NavyFixture::link_sea(w, w.regions[zones.back()].provinces.front(), sea);
        zones.push_back(zone);
    }

    // 40 task forces of 10 destroyers each (200 per side), spread over the zones.
    for (int i = 0; i < 40; ++i) {
        const CountryId country = (i % 2 == 0) ? f.a : f.b;
        const ProvinceId port = (i % 2 == 0) ? f.port_a : f.port_b;
        const NavalMission mission =
            (i % 3 == 0) ? NavalMission::ConvoyRaid : NavalMission::StrikeForce;
        const TaskForceId tf = f.add_task_force(country, port, zones[i % zones.size()], mission,
                                                true);
        for (int j = 0; j < 10; ++j) f.add_ship(country, tf, f.destroyer, 1.0);
    }

    const int ticks = 20;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ticks; ++i) phase_naval(g);
    const auto end = std::chrono::steady_clock::now();
    const double ms_total =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double ms_per_tick = ms_total / ticks;
    std::printf("    [navy] 400 ships / 30 zones: %.4f ms/tick (%d ticks, %.2f ms total)\n",
                ms_per_tick, ticks, ms_total);
    CHECK(ms_per_tick < 1.0);
    CHECK(std::isfinite(ms_per_tick));
}

}  // namespace