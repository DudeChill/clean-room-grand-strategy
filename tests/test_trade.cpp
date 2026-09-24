// Trade tests (spec section 46): resources as a real flow between countries.
//
// Every world here is hand-built through the shared helpers in test_util.h, so the
// expected numbers are arithmetic rather than fixtures from data files. The tests
// pin the rules the phase cannot silently guarantee away:
//
//  * a land route delivers resources and raises the importer's industry output;
//  * a sea route consumes convoys and stops when the pool is empty;
//  * enemy naval control in the route's zone cuts delivery, not the route;
//  * war between the parties closes the route;
//  * an exhausted exporter surplus scales the route down to what is feasible;
//  * the AI opens a route through real commands within 30 days;
//  * two identical runs hash identically.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/industry.h"
#include "sim/phases.h"
#include "sim/trade.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

constexpr int STEEL = static_cast<int>(Resource::Steel);

// Infantry equipment the importer's line builds: two steel per factory per day.
EquipmentId add_trade_equipment(Content& c, const char* key, EquipmentCategory category,
                                double steel, double build_cost) {
    EquipmentDef d;
    d.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    d.key = key;
    d.name = key;
    d.category = category;
    d.build_cost = build_cost;
    d.resources[STEEL] = steel;
    d.max_strength = 10.0;
    d.manpower = 100.0;
    c.equipment_by_key[key] = d.id;
    c.equipment.push_back(d);
    return d.id;
}

// A hermetic trade world. `sea_only` decides the geometry:
//   false -> the exporter's and importer's capitals are adjacent (land route);
//   true  -> the capitals are not connected, but both countries hold a port on the
//            same sea zone (sea route).
struct TradeWorld {
    Game g;
    RegionId land = RegionId{};
    RegionId sea = RegionId{};
    CountryId exp = CountryId{};
    CountryId imp = CountryId{};
    CountryId enemy = CountryId{};
    StateId exp_state = StateId{};
    StateId imp_state = StateId{};
    ProvinceId exp_capital = ProvinceId{};
    ProvinceId imp_capital = ProvinceId{};
    ProvinceId exp_port = ProvinceId{};
    ProvinceId imp_port = ProvinceId{};
    ProvinceId sea_zone = ProvinceId{};
    EquipmentId rifle = EquipmentId{};
    EquipmentId convoy = EquipmentId{};

    explicit TradeWorld(bool sea_only) { build(sea_only); }

    CountryId add_country(const char* tag) {
        const CountryId id = g.world.countries.create();
        Country* c = g.world.country(id);
        c->tag = tag;
        c->name = tag;
        c->alive = true;
        c->law_levels.assign(4, 0);
        c->equipment_stockpile.assign(g.content.equipment.size(), 0.0);
        return id;
    }

    StateId add_state(CountryId owner, int civilian, int military) {
        State s;
        s.name = "state";
        s.owner = owner;
        s.controller = owner;
        s.region = land;
        s.building_slots = 20;
        s.civilian_factories = civilian;
        s.military_factories = military;
        return g.world.states.create(s);
    }

    ProvinceId add_land(StateId state, CountryId owner, double steel, bool capital) {
        const ProvinceId id = add_province(g.world, "p", state, land, Terrain::Plains);
        Province& p = g.world.provinces[id];
        p.owner = owner;
        p.controller = owner;
        p.resource_yield[STEEL] = steel;
        p.is_capital = capital;
        return id;
    }

    void build(bool sea_only) {
        Content& ct = g.content;
        rifle = add_trade_equipment(ct, "rifle", EquipmentCategory::Infantry, 2.0, 4.0);
        convoy = add_trade_equipment(ct, "convoy_1", EquipmentCategory::Convoy, 0.0, 10.0);

        World& w = g.world;
        Region lr;
        lr.name = "land";
        land = w.regions.create(lr);
        Region sr;
        sr.name = "sea";
        sr.is_sea = true;
        sea = w.regions.create(sr);

        exp = add_country("EXP");
        imp = add_country("IMP");
        enemy = add_country("ENM");

        exp_state = add_state(exp, 10, 0);
        imp_state = add_state(imp, 10, 5);  // the civilian pool the importer pays with
        add_state(enemy, 0, 0);

        w.country(exp)->capital = exp_state;
        w.country(imp)->capital = imp_state;

        // The exporter sits on a large steel field; the importer has none.
        exp_capital = add_land(exp_state, exp, 20.0, true);
        imp_capital = add_land(imp_state, imp, 0.0, true);

        if (!sea_only) {
            link_provinces(w, exp_capital, imp_capital);
            return;
        }

        sea_zone = add_province(w, "sea_zone", StateId{}, sea, Terrain::Ocean);
        w.provinces[sea_zone].is_sea = true;
        exp_port = add_land(exp_state, exp, 0.0, false);
        imp_port = add_land(imp_state, imp, 0.0, false);
        w.provinces[exp_port].naval_base = 1;
        w.provinces[imp_port].naval_base = 1;
        link_provinces(w, exp_port, sea_zone, true);
        link_provinces(w, imp_port, sea_zone, true);
    }

    // A staffed production line: `factories` on rifles, so `factories * 2` steel is
    // demanded per day and the line starves without imports.
    void add_importer_line(int factories) {
        ProductionLine line;
        line.equipment = rifle;
        line.factories = factories;
        line.efficiency = 1.0;
        line.efficiency_cap = 1.0;
        g.world.country(imp)->lines.push_back(line);
    }

    void set_convoy_stock(double amount) {
        g.world.country(imp)->equipment_stockpile[convoy.v] = amount;
    }

    double convoy_stock() const { return g.world.country(imp)->equipment_stockpile[convoy.v]; }

    double stockpile(EquipmentId e) const { return g.world.country(imp)->equipment_stockpile[e.v]; }

    TradeRoute* route(CountryId importer, CountryId exporter, Resource resource) {
        for (TradeRoute& r : g.world.trade_routes) {
            if (r.importer == importer && r.exporter == exporter && r.resource == resource) {
                return &r;
            }
        }
        return nullptr;
    }

    // One trade day: the phase only acts at hour 0, exactly as tick_once drives it.
    void trade_day() {
        g.world.date.hour = 0;
        phase_trade(g);
    }
};

// A war between two countries, so World::at_war and the naval-share test both see
// a real conflict rather than a bare Relation flag.
WarId declare_war(Game& g, CountryId a, CountryId b) {
    War war;
    war.aggressor = a;
    war.start_tick = g.world.tick;
    war.active = true;
    WarParticipant pa;
    pa.country = a;
    WarParticipant pb;
    pb.country = b;
    war.attackers.push_back(pa);
    war.defenders.push_back(pb);
    const WarId id = g.world.wars.create(war);
    g.world.country(a)->wars.push_back(id);
    g.world.country(b)->wars.push_back(id);
    g.world.country(a)->at_war = true;
    g.world.country(b)->at_war = true;
    return id;
}

bool has_trade_event(const Game& g, const char* needle) {
    for (const SimEvent& e : g.events) {
        if (e.kind == "trade" && e.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

// --------------------------------------------------------------- balance -----

HOI_TEST(trade_resource_balance_is_production_minus_requirement) {
    TradeWorld f(false);
    f.add_importer_line(5);  // 5 factories * 2 steel/day = 10 steel/day demanded
    CHECK_NEAR(resource_balance(f.g, f.imp, Resource::Steel), -10.0, 1e-9);
    CHECK_NEAR(resource_balance(f.g, f.exp, Resource::Steel), 20.0 * 1.25, 1e-9);
    // The exporter needs a positive surplus of something for a route to exist.
    CHECK(trade_route_possible(f.g, f.imp, f.exp, nullptr, nullptr));
}

// ------------------------------------------------------------ land route -----

HOI_TEST(trade_land_route_delivers_and_raises_industry_output) {
    TradeWorld traded(false);
    TradeWorld control(false);
    traded.add_importer_line(5);
    control.add_importer_line(5);

    CHECK(trade_start(traded.g, traded.imp, traded.exp, Resource::Steel, 10.0));
    CHECK_EQ(traded.g.world.trade_routes.size(), static_cast<size_t>(1));
    CHECK(!traded.g.world.trade_routes[0].sea_route);
    CHECK(has_trade_event(traded.g, "opened"));

    traded.g.run_ticks(48);   // two full days
    control.g.run_ticks(48);

    // The route reported a full delivery, in the books and on the countries.
    TradeRoute* r = traded.route(traded.imp, traded.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(r->active);
    CHECK_NEAR(r->delivered, 10.0, 1e-6);
    CHECK_NEAR(traded.g.world.country(traded.imp)->resources_imported[STEEL], 10.0, 1e-6);
    CHECK_NEAR(traded.g.world.country(traded.exp)->resources_exported[STEEL], 10.0, 1e-6);

    // The importer's line is no longer starved, so it out-produces the control world
    // with no route at all.
    CHECK_GT(traded.stockpile(traded.rifle), control.stockpile(control.rifle));
    CHECK_NEAR(traded.g.world.country(traded.imp)->lines[0].resource_shortage, 0.0, 1e-9);
}

HOI_TEST(trade_war_between_parties_closes_the_route) {
    TradeWorld f(false);
    f.add_importer_line(5);
    CHECK(trade_start(f.g, f.imp, f.exp, Resource::Steel, 10.0));
    declare_war(f.g, f.imp, f.exp);

    f.trade_day();

    TradeRoute* r = f.route(f.imp, f.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(!r->active);
    CHECK_NEAR(r->amount, 0.0, 1e-9);
    CHECK_NEAR(r->delivered, 0.0, 1e-9);
    CHECK_NEAR(f.g.world.country(f.imp)->resources_imported[STEEL], 0.0, 1e-9);
    CHECK(has_trade_event(f.g, "war"));
}

HOI_TEST(trade_exhausted_surplus_scales_the_route_down) {
    TradeWorld f(false);
    f.add_importer_line(5);
    CHECK(trade_start(f.g, f.imp, f.exp, Resource::Steel, 10.0));
    CHECK_NEAR(f.route(f.imp, f.exp, Resource::Steel)->amount, 10.0, 1e-9);

    // The exporter's field shrinks: a yield of 4 with infrastructure 5 produces
    // 4 * 1.25 = 5 steel per day, so only 5 is left to ship.
    f.g.world.provinces[f.exp_capital].resource_yield[STEEL] = 4.0;
    f.trade_day();

    TradeRoute* r = f.route(f.imp, f.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(r->active);
    CHECK_NEAR(r->amount, 5.0, 1e-9);
    CHECK_NEAR(r->delivered, 5.0, 1e-9);
    CHECK_NEAR(f.g.world.country(f.imp)->resources_imported[STEEL], 5.0, 1e-9);
}

HOI_TEST(trade_cancel_removes_the_route_and_keeps_order) {
    TradeWorld f(false);
    f.add_importer_line(5);
    CHECK(trade_start(f.g, f.imp, f.exp, Resource::Steel, 10.0));
    CHECK(trade_cancel(f.g, f.imp, f.exp, Resource::Steel));
    CHECK_EQ(f.g.world.trade_routes.size(), static_cast<size_t>(0));
    CHECK(!trade_cancel(f.g, f.imp, f.exp, Resource::Steel));
    CHECK(has_trade_event(f.g, "closed"));
}

// ------------------------------------------------------------- sea routes ----

HOI_TEST(trade_sea_route_consumes_convoys_and_stops_when_exhausted) {
    TradeWorld f(true);
    f.add_importer_line(5);
    // 0.5 convoys at 0.05 convoy units per resource unit ship exactly 10 units for
    // one day, then the pool is empty.
    f.set_convoy_stock(0.5);
    CHECK(trade_start(f.g, f.imp, f.exp, Resource::Steel, 10.0));

    TradeRoute* r = f.route(f.imp, f.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(r->sea_route);

    f.trade_day();
    CHECK_NEAR(r->delivered, 10.0, 1e-6);
    CHECK_NEAR(r->convoy_use, 0.5, 1e-9);
    CHECK_NEAR(f.convoy_stock(), 0.0, 1e-9);
    CHECK_NEAR(f.g.world.country(f.imp)->resources_imported[STEEL], 10.0, 1e-6);
    CHECK(r->active);

    f.trade_day();
    CHECK(!r->active);
    CHECK_NEAR(r->amount, 0.0, 1e-9);
    CHECK_NEAR(f.g.world.country(f.imp)->resources_imported[STEEL], 0.0, 1e-9);
}

HOI_TEST(trade_blockade_cuts_delivery_but_not_the_route) {
    TradeWorld f(true);
    f.add_importer_line(5);
    f.set_convoy_stock(100.0);
    declare_war(f.g, f.imp, f.enemy);
    CHECK(trade_start(f.g, f.imp, f.exp, Resource::Steel, 10.0));

    // Enemy naval control of half the route's sea zone.
    f.g.world.regions[f.sea].naval_control.push_back({f.enemy, 0.5});
    f.trade_day();

    TradeRoute* r = f.route(f.imp, f.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(r->active);
    CHECK_NEAR(r->delivered, 5.0, 1e-6);
    CHECK_NEAR(f.g.world.country(f.imp)->resources_imported[STEEL], 5.0, 1e-6);
    CHECK_NEAR(r->amount, 10.0, 1e-9);  // the standing request is untouched
    CHECK(has_trade_event(f.g, "blockade"));
}

// ------------------------------------------------------------------ AI -------

HOI_TEST(trade_ai_opens_a_route_to_cover_a_deficit_within_30_days) {
    TradeWorld f(false);
    f.add_importer_line(5);
    // Only the importer is AI-controlled; the exporter is a human player.
    f.g.ai_controlled.assign(f.g.world.countries.capacity(), 0);
    f.g.set_ai(f.imp, true);
    f.g.player_country = f.exp;

    for (int day = 0; day < 30; ++day) {
        f.g.world.tick = static_cast<Tick>(day) * TICKS_PER_DAY;
        f.g.world.date.hour = 0;
        phase_ai(f.g);
        phase_commands(f.g);
        phase_trade(f.g);
        phase_industry(f.g);
    }

    TradeRoute* r = f.route(f.imp, f.exp, Resource::Steel);
    CHECK(r != nullptr);
    CHECK(r->active);
    CHECK_GT(r->amount, 0.0);
    CHECK_GT(f.g.world.country(f.imp)->resources_imported[STEEL], 0.0);

    // The AI acted through the command log, not by editing world state.
    bool start_seen = false;
    for (const CommandRecord& rec : f.g.log.records) {
        if (rec.command.type == CommandType::StartTrade && rec.command.country == f.imp) {
            start_seen = true;
        }
    }
    CHECK(start_seen);
}

// ------------------------------------------------------------ determinism ----

HOI_TEST(trade_runs_are_deterministic) {
    const auto run = []() {
        TradeWorld f(false);
        f.add_importer_line(5);
        f.g.ai_controlled.assign(f.g.world.countries.capacity(), 0);
        f.g.set_ai(f.imp, true);
        f.g.player_country = f.exp;
        f.g.run_ticks(30 * TICKS_PER_DAY);
        return world_hash(f.g);
    };
    CHECK_EQ(run(), run());
}

}  // namespace