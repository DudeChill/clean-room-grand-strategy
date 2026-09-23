// Economy tests: resources, production lines, construction, reinforcement and
// research. Every world here is hand-built so the expected numbers are arithmetic
// rather than fixtures from data files.

#include <algorithm>
#include <string>
#include <vector>

#include "game/game.h"
#include "sim/industry.h"
#include "sim/research.h"
#include "test.h"

namespace {

using namespace hoi;

// A hand-built game: content tables are filled by the helpers below, entities are
// created in a fixed order so every world created by the same recipe is identical.
struct Fixture {
    Game g;
    RegionId region_;

    // ------------------------------------------------------------- content ----
    EquipmentId equipment(const char* key, const char* archetype, double cost, double steel,
                          EquipmentCategory category = EquipmentCategory::Infantry,
                          double max_strength = 10.0, double manpower = 100.0) {
        EquipmentDef d;
        d.id = EquipmentId(static_cast<uint32_t>(g.content.equipment.size()));
        d.key = key;
        d.name = key;
        d.archetype = archetype;
        d.category = category;
        d.build_cost = cost;
        d.resources[static_cast<int>(Resource::Steel)] = steel;
        d.max_strength = max_strength;
        d.manpower = manpower;
        g.content.equipment.push_back(d);
        g.content.equipment_by_key[key] = d.id;
        return d.id;
    }

    TemplateId division_template(const char* key, EquipmentId line_equipment, int count,
                                 double manpower, double hit_points) {
        DivisionTemplate t;
        t.id = TemplateId(static_cast<uint32_t>(g.content.templates.size()));
        t.key = key;
        t.name = key;
        BattalionSlot b;
        b.equipment = line_equipment;
        b.count = count;
        t.battalions.push_back(b);
        t.manpower = manpower;
        t.max_strength = hit_points;
        g.content.templates.push_back(t);
        g.content.template_by_key[key] = t.id;
        return t.id;
    }

    TechId add_tech(const char* key, int year, double cost_days,
                    std::vector<TechId> prerequisites = {}, std::vector<std::string> unlock_equipment = {},
                    std::vector<std::string> unlock_buildings = {},
                    ModifierKind modifier = ModifierKind::Count, double modifier_value = 0.0) {
        TechDef t;
        t.id = TechId(static_cast<uint32_t>(g.content.techs.size()));
        t.key = key;
        t.name = key;
        t.year = year;
        t.cost_days = cost_days;
        t.prerequisites = std::move(prerequisites);
        t.unlock_equipment = std::move(unlock_equipment);
        t.unlock_buildings = std::move(unlock_buildings);
        if (modifier != ModifierKind::Count) t.modifiers.set(modifier, modifier_value);
        g.content.techs.push_back(t);
        g.content.tech_by_key[key] = t.id;
        return t.id;
    }

    void add_building(const char* key, BuildingKind kind, double base_cost, int max_level) {
        BuildingDef b;
        b.key = key;
        b.name = key;
        b.kind = kind;
        b.base_cost = base_cost;
        b.max_level = max_level;
        g.content.buildings.push_back(b);
    }

    // Tests derive their expectations from this value, so they also set it to a
    // value that is deliberately NOT the shipped default: that way a phase that
    // hardcodes the cap instead of reading the constant fails the test.
    void set_max_factories_per_project(double value) {
        g.content.constants.max_factories_per_project = value;
    }

    // -------------------------------------------------------------- world ----
    CountryId add_country(const char* tag) {
        const CountryId id = g.world.countries.create();
        Country* c = g.world.country(id);
        c->id = id;
        c->tag = tag;
        c->name = tag;
        c->alive = true;
        return id;
    }

    // Land provinces must reference a state and a region for the world auditor, so
    // the fixture keeps one region per world and attaches every province to it.
    RegionId region() {
        if (!region_.valid()) {
            region_ = g.world.regions.create();
            Region& r = g.world.regions[region_];
            r.id = region_;
            r.name = "region";
        }
        return region_;
    }

    StateId add_state(CountryId owner, int building_slots) {
        const StateId id = g.world.states.create();
        State* s = g.world.state(id);
        s->id = id;
        s->name = "state";
        s->owner = owner;
        s->controller = owner;
        s->building_slots = building_slots;
        return id;
    }

    ProvinceId add_province(StateId state, CountryId owner, CountryId controller, int infrastructure,
                            double steel, double oil) {
        const ProvinceId id = g.world.provinces.create();
        Province* p = g.world.province(id);
        p->id = id;
        p->name = "province";
        p->state = state;
        p->region = region();
        p->owner = owner;
        p->controller = controller;
        p->infrastructure = infrastructure;
        p->resource_yield[static_cast<int>(Resource::Steel)] = steel;
        p->resource_yield[static_cast<int>(Resource::Oil)] = oil;
        if (State* s = g.world.state(state)) s->provinces.push_back(id);
        g.world.regions[region()].provinces.push_back(id);
        return id;
    }

    Country* country(CountryId id) { return g.world.country(id); }
    State* state(StateId id) { return g.world.state(id); }
    Province* province(ProvinceId id) { return g.world.province(id); }

    // --------------------------------------------------------- production ----
    State* first_controlled_state(CountryId id) {
        State* found = nullptr;
        g.world.states.for_each([&](StateId, State& s) {
            if (!found && s.controller == id) found = &s;
        });
        return found;
    }

    // A production line is staffed from factories the country controls, so the helper
    // tops up the country's military factories to cover the assignment (the world
    // auditor requires the assignment to stay within them). Tests that deliberately
    // build an inconsistent world pass `staff = false`.
    size_t add_line(CountryId id, EquipmentId eq, int factories, double efficiency, double cap,
                    bool staff = true) {
        if (staff && factories > 0) {
            int mil = 0;
            int assigned = 0;
            count_factories(g.world, id, nullptr, &mil, nullptr);
            for (const ProductionLine& existing : g.world.country(id)->lines) {
                assigned += existing.factories;
            }
            const int missing = assigned + factories - mil;
            if (missing > 0) {
                if (State* pool = first_controlled_state(id)) pool->military_factories += missing;
            }
        }
        Country* c = g.world.country(id);
        ProductionLine line;
        line.equipment = eq;
        line.factories = factories;
        line.efficiency = efficiency;
        line.efficiency_cap = cap;
        c->lines.push_back(line);
        return c->lines.size() - 1;
    }

    ProductionLine& line(CountryId id, size_t index) { return g.world.country(id)->lines[index]; }

    void set_stockpile(CountryId id, EquipmentId eq, double amount) {
        std::vector<double>& stock = g.world.country(id)->equipment_stockpile;
        if (stock.size() <= eq.v) stock.resize(eq.v + 1, 0.0);
        stock[eq.v] = amount;
    }

    double stockpile(CountryId id, EquipmentId eq) const {
        const std::vector<double>& stock = g.world.country(id)->equipment_stockpile;
        return eq.v < stock.size() ? stock[eq.v] : 0.0;
    }

    DivisionId add_division(CountryId id, TemplateId tpl, ProvinceId location, double strength) {
        const DivisionId did = g.world.divisions.create();
        Division* d = g.world.division(did);
        d->id = did;
        d->country = id;
        d->template_id = tpl;
        d->location = location;
        d->strength = strength;
        g.world.country(id)->divisions.push_back(did);
        return did;
    }

    // Advances the industry phase `hours` times, mirroring the real clock so that
    // the day boundary and production-line switch stamps behave as in game.
    void advance_industry(int hours) {
        for (int i = 0; i < hours; ++i) {
            ++g.world.tick;
            g.world.date.hour = static_cast<uint8_t>(g.world.tick % 24);
            phase_industry(g);
        }
    }

    void advance_research(int hours) {
        for (int i = 0; i < hours; ++i) {
            ++g.world.tick;
            g.world.date.hour = static_cast<uint8_t>(g.world.tick % 24);
            phase_research(g);
        }
    }
};

constexpr int STEEL = static_cast<int>(Resource::Steel);
constexpr int OIL = static_cast<int>(Resource::Oil);

}  // namespace

// ------------------------------------------------------------------ 5.1 -----

HOI_TEST(resource_production_sums_controlled_provinces) {
    Fixture f;
    const CountryId a = f.add_country("AAA");
    const CountryId b = f.add_country("BBB");
    const StateId a_state = f.add_state(a, 10);
    const StateId b_state = f.add_state(b, 10);

    // Scaled by (1 + 0.05 * infrastructure).
    f.add_province(a_state, a, a, 3, 4.0, 0.0);
    f.add_province(a_state, a, a, 8, 2.0, 3.0);
    // Occupied by another country: pays nothing.
    f.add_province(b_state, b, b, 5, 100.0, 0.0);
    // Controlled by us, but its state answers to someone else: excluded.
    f.add_province(b_state, a, a, 5, 100.0, 0.0);

    double produced[RESOURCE_COUNT];
    compute_resource_production(f.g.world, f.g.content, a, produced);
    CHECK_NEAR(produced[STEEL], 4.0 * 1.15 + 2.0 * 1.40, 1e-9);
    CHECK_NEAR(produced[OIL], 3.0 * 1.40, 1e-9);

    compute_resource_production(f.g.world, f.g.content, b, produced);
    CHECK_NEAR(produced[STEEL], 100.0 * 1.25, 1e-9);

    // The phase publishes the same numbers on the country.
    phase_industry(f.g);
    CHECK_NEAR(f.country(a)->resources_produced[STEEL], 7.4, 1e-9);
}

HOI_TEST(resource_shortage_reduces_output_monotonically) {
    const auto output_for_steel_production = [](double daily_steel) {
        Fixture f;
        const CountryId c = f.add_country("AAA");
        const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 2.0);
        const StateId st = f.add_state(c, 10);
        f.add_province(st, c, c, 0, daily_steel, 0.0);
        f.add_line(c, rifle, 1, 1.0, 1.0);
        phase_industry(f.g);
        return f.line(c, 0).output_today;
    };

    const double none = output_for_steel_production(0.0);
    const double some = output_for_steel_production(1.0);
    const double ample = output_for_steel_production(10.0);

    // One factory at full efficiency builds 4.5 IC-days per day: 0.1875 per hour,
    // i.e. 0.046875 rifles per hour at a cost of 4.
    CHECK_NEAR(ample, 0.046875, 1e-12);
    // One factory on this model draws 2 steel per day, so 1 steel per day covers
    // half of it and halves the output.
    CHECK_NEAR(some, 0.046875 * 0.5, 1e-12);
    // Nothing at all leaves the documented shortage floor of output.
    CHECK_NEAR(none, 0.046875 * 0.10, 1e-12);
    CHECK_GT(some, none);
    CHECK_GT(ample, some);

    // And the reported shortage is the complement of the factor actually used.
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 2.0);
    const StateId st = f.add_state(c, 10);
    f.add_province(st, c, c, 0, 0.0, 0.0);
    f.add_line(c, rifle, 1, 1.0, 1.0);
    phase_industry(f.g);
    CHECK_NEAR(f.line(c, 0).resource_shortage, 0.9, 1e-12);
}

HOI_TEST(efficiency_grows_toward_cap_and_never_exceeds_it) {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId st = f.add_state(c, 10);
    f.add_province(st, c, c, 0, 1000.0, 0.0);
    f.add_line(c, rifle, 1, 0.10, 0.50);

    double previous_efficiency = 0.0;
    double previous_cap = 0.0;
    for (int hour = 0; hour < 24 * 30; ++hour) {
        f.advance_industry(1);
        const ProductionLine& line = f.line(c, 0);
        CHECK(line.efficiency >= previous_efficiency);
        CHECK(line.efficiency_cap >= previous_cap);
        CHECK(line.efficiency <= line.efficiency_cap);
        CHECK(line.efficiency_cap <= 1.0);
        CHECK(line.efficiency <= 1.0);
        previous_efficiency = line.efficiency;
        previous_cap = line.efficiency_cap;
    }
    // 30 days of producing: the ceiling grows by efficiency_cap_growth_per_day/24
    // each hour, and efficiency converges onto it.
    CHECK_NEAR(previous_cap, 0.50 + 30.0 * 0.001667, 1e-3);
    CHECK_NEAR(previous_efficiency, previous_cap, 0.02);
    CHECK_GT(previous_efficiency, 0.50);

    // With no factories there is no output, so efficiency never moves.
    Fixture idle;
    const CountryId ic = idle.add_country("AAA");
    const EquipmentId idle_rifle = idle.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId ist = idle.add_state(ic, 10);
    idle.add_province(ist, ic, ic, 0, 1000.0, 0.0);
    idle.add_line(ic, idle_rifle, 0, 0.10, 0.50);
    idle.advance_industry(24 * 10);
    CHECK_NEAR(idle.line(ic, 0).efficiency, 0.10, 1e-12);
    CHECK_NEAR(idle.line(ic, 0).efficiency_cap, 0.50, 1e-12);

    // With factories but no resources the line still runs at the documented
    // shortage floor, so it keeps growing while it produces anything at all.
    Fixture starved;
    const CountryId sc = starved.add_country("AAA");
    const EquipmentId srifle = starved.equipment("rifle", "infantry_rifle", 4.0, 100.0);
    const StateId sst = starved.add_state(sc, 10);
    starved.add_province(sst, sc, sc, 0, 0.0, 0.0);
    starved.add_line(sc, srifle, 1, 0.10, 0.50);
    starved.advance_industry(24);
    CHECK_GT(starved.line(sc, 0).resource_shortage, 0.0);
    CHECK_GT(starved.line(sc, 0).efficiency, 0.10);

    // Daily counters reset when a new day starts and accumulate inside the day.
    Fixture counters;
    const CountryId cc = counters.add_country("AAA");
    const EquipmentId crifle = counters.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId cst = counters.add_state(cc, 10);
    counters.add_province(cst, cc, cc, 0, 1000.0, 0.0);
    counters.add_line(cc, crifle, 1, 1.0, 1.0);
    counters.g.world.tick = 1;
    counters.g.world.date.hour = 1;
    phase_industry(counters.g);
    const double per_hour = counters.line(cc, 0).output_today;
    CHECK_NEAR(per_hour, 0.046875, 1e-12);
    counters.g.world.tick = 2;
    counters.g.world.date.hour = 2;
    phase_industry(counters.g);
    CHECK_NEAR(counters.line(cc, 0).output_today, 2.0 * per_hour, 1e-12);
    counters.g.world.tick = 24;
    counters.g.world.date.hour = 0;
    phase_industry(counters.g);
    CHECK_NEAR(counters.line(cc, 0).output_today, per_hour, 1e-12);
    CHECK_NEAR(counters.line(cc, 0).output_total, 3.0 * per_hour, 1e-12);
}

HOI_TEST(equipment_switch_applies_efficiency_retention) {
    // Same archetype: efficiency and ceiling are retained at 70%.
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle_1 = f.equipment("rifle_1", "infantry_rifle", 4.0, 1.0);
    const EquipmentId rifle_2 = f.equipment("rifle_2", "infantry_rifle", 5.0, 1.0);
    const StateId st = f.add_state(c, 10);
    f.add_province(st, c, c, 0, 1000.0, 0.0);
    const size_t index = f.add_line(c, rifle_2, 1, 1.0, 1.0);
    f.line(c, index).previous.push_back(rifle_1);  // pending switch away from rifle_1
    phase_industry(f.g);
    CHECK_NEAR(f.line(c, index).efficiency, 0.70, 1e-12);
    // The ceiling retains the same 70% and then grows by the usual hourly rate
    // because the line is producing.
    CHECK_NEAR(f.line(c, index).efficiency_cap, 0.70 + 0.001667 / 24.0, 1e-12);
    CHECK(f.line(c, index).previous.empty());  // the switch has been consumed
    // A later tick must not apply the retention a second time.
    f.advance_industry(1);
    CHECK_NEAR(f.line(c, index).efficiency, 0.70, 1e-5);

    // Different archetype: back to the starting point of a brand new line.
    Fixture g;
    const CountryId gc = g.add_country("AAA");
    const EquipmentId armor = g.equipment("armor_1", "armor", 20.0, 3.0, EquipmentCategory::Armor);
    const EquipmentId gear = g.equipment("rifle_2", "infantry_rifle", 5.0, 1.0);
    const StateId gst = g.add_state(gc, 10);
    g.add_province(gst, gc, gc, 0, 1000.0, 0.0);
    const size_t gindex = g.add_line(gc, gear, 1, 0.90, 1.0);
    g.line(gc, gindex).previous.push_back(armor);  // pending switch from armour to rifles
    phase_industry(g.g);
    CHECK_NEAR(g.line(gc, gindex).efficiency, 0.10 + (0.50 - 0.10) * 0.20 / 24.0, 1e-12);
    CHECK_NEAR(g.line(gc, gindex).efficiency_cap, 0.50 + 0.001667 / 24.0, 1e-12);
}

// A country that lost the states holding its military factories must not keep
// producing from factories it does not have: the excess assignment is released
// from the end of the line list, and the world is consistent afterwards.
HOI_TEST(production_lines_release_factories_the_country_no_longer_controls) {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId st = f.add_state(c, 10);
    f.state(st)->military_factories = 1;  // one factory is all that is still controlled
    f.add_province(st, c, c, 0, 1000.0, 0.0);
    const size_t first = f.add_line(c, rifle, 1, 1.0, 1.0, false);
    const size_t second = f.add_line(c, rifle, 3, 1.0, 1.0, false);

    // The world starts inconsistent: four factories assigned, one controlled.
    CHECK(check_invariants(f.g).empty() == false);

    f.advance_industry(1);
    CHECK_EQ(f.line(c, first).factories, 1);   // the earliest line keeps its assignment
    CHECK_EQ(f.line(c, second).factories, 0);  // the last line is released first
    CHECK(f.line(c, second).equipment.valid() == false);
    CHECK_EQ(f.line(c, second).previous.size(), size_t{1});
    CHECK_EQ(f.line(c, second).previous.front(), rifle);
    // Retirement is not an equipment switch: efficiency is untouched.
    CHECK_NEAR(f.line(c, second).efficiency, 1.0, 1e-12);
    CHECK(check_invariants(f.g).empty());

    int mil = 0;
    int assigned = 0;
    count_factories(f.g.world, c, nullptr, &mil, nullptr);
    for (const ProductionLine& line : f.country(c)->lines) assigned += line.factories;
    CHECK_EQ(assigned, mil);
    CHECK_EQ(assigned, 1);
    CHECK_EQ(f.g.events.size(), size_t{1});
    CHECK_EQ(f.g.events.front().kind, std::string("production"));
    CHECK(f.g.events.front().text.find("AAA") != std::string::npos);

    // Nothing re-releases on later ticks, and the retired line stays retired.
    f.advance_industry(48);
    CHECK_EQ(f.line(c, first).factories, 1);
    CHECK_EQ(f.line(c, second).factories, 0);
    CHECK_EQ(f.g.events.size(), size_t{1});
    CHECK(check_invariants(f.g).empty());

    // A country whose whole industry is gone releases every assignment.
    Fixture wiped;
    const CountryId wc = wiped.add_country("BBB");
    const EquipmentId wrifle = wiped.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId wst = wiped.add_state(wc, 10);
    wiped.state(wst)->military_factories = 1;
    wiped.add_province(wst, wc, wc, 0, 1000.0, 0.0);
    wiped.add_line(wc, wrifle, 1, 1.0, 1.0, false);
    wiped.add_line(wc, wrifle, 2, 1.0, 1.0, false);
    wiped.state(wst)->military_factories = 0;  // conquered before the tick runs
    wiped.advance_industry(1);
    CHECK_EQ(wiped.line(wc, 0).factories, 0);
    CHECK_EQ(wiped.line(wc, 1).factories, 0);
    CHECK(wiped.line(wc, 0).equipment.valid() == false);
    CHECK(wiped.line(wc, 1).equipment.valid() == false);
    CHECK(check_invariants(wiped.g).empty());
    // A trickle of losses that never retires a line is not reported at all: the log
    // must not gain one line per lost factory per hour during a long retreat.
    Fixture trickle;
    const CountryId tc = trickle.add_country("CCC");
    const EquipmentId trickle_rifle = trickle.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const StateId trickle_state = trickle.add_state(tc, 10);
    trickle.add_province(trickle_state, tc, tc, 0, 1000.0, 0.0);
    trickle.add_line(tc, trickle_rifle, 2, 1.0, 1.0, false);
    trickle.add_line(tc, trickle_rifle, 3, 1.0, 1.0, false);
    trickle.state(trickle_state)->military_factories = 4;  // one assignment over
    trickle.advance_industry(1);
    CHECK_EQ(trickle.line(tc, 0).factories, 2);
    CHECK_EQ(trickle.line(tc, 1).factories, 2);  // trimmed, not retired
    CHECK(trickle.g.events.empty());
    CHECK(check_invariants(trickle.g).empty());

    trickle.state(trickle_state)->military_factories = 3;
    trickle.advance_industry(1);
    CHECK_EQ(trickle.line(tc, 1).factories, 1);
    CHECK(trickle.g.events.empty());

    // Only the release that retires the line is reported, once for the whole trickle.
    trickle.state(trickle_state)->military_factories = 2;
    trickle.advance_industry(1);
    CHECK_EQ(trickle.line(tc, 0).factories, 2);
    CHECK_EQ(trickle.line(tc, 1).factories, 0);
    CHECK_EQ(trickle.g.events.size(), size_t{1});
    CHECK_EQ(trickle.g.events.front().kind, std::string("production"));
    CHECK_NEAR(trickle.line(tc, 1).efficiency, 1.0, 1e-12);
    CHECK(check_invariants(trickle.g).empty());
}

HOI_TEST(release_rule_respects_factory_pools) {
    // Pools are independent: losing the dockyards retires the ship lines and leaves
    // the military lines (and their factories) alone.
    Fixture navy;
    const CountryId nc = navy.add_country("DDD");
    const EquipmentId nrifle = navy.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const EquipmentId destroyer = navy.equipment("destroyer", "destroyer", 500.0, 20.0,
                                                 EquipmentCategory::Ship);
    const StateId nst = navy.add_state(nc, 20);
    navy.add_province(nst, nc, nc, 0, 1000.0, 0.0);
    navy.state(nst)->military_factories = 2;
    navy.state(nst)->dockyards = 2;
    const size_t rifle_line = navy.add_line(nc, nrifle, 2, 1.0, 1.0, false);
    const size_t ship_line = navy.add_line(nc, destroyer, 2, 1.0, 1.0, false);
    CHECK(check_invariants(navy.g).empty());

    // A ship line is paid for out of dockyards, so its two factories yield dockyard
    // IC (2.5 per factory per day) rather than military IC (4.5).
    CHECK_NEAR(line_hourly_output(navy.g, *navy.country(nc), navy.line(nc, ship_line)),
               2.0 * navy.g.content.constants.ic_per_dockyard / 24.0, 1e-12);

    navy.state(nst)->dockyards = 0;  // the yards are lost, the army industry is not
    navy.advance_industry(1);
    CHECK_EQ(navy.line(nc, rifle_line).factories, 2);
    CHECK_EQ(navy.line(nc, ship_line).factories, 0);
    CHECK(navy.line(nc, ship_line).equipment.valid() == false);
    CHECK_EQ(navy.g.events.size(), size_t{1});
    CHECK_EQ(navy.g.events.front().kind, std::string("production"));
    CHECK(check_invariants(navy.g).empty());

    // The mirror case: losing the military factories retires the rifle line and
    // leaves the ship line staffed.
    Fixture navy2;
    const CountryId n2c = navy2.add_country("EEE");
    const EquipmentId n2rifle = navy2.equipment("rifle", "infantry_rifle", 4.0, 1.0);
    const EquipmentId n2ship = navy2.equipment("destroyer", "destroyer", 500.0, 20.0,
                                               EquipmentCategory::Ship);
    const StateId n2st = navy2.add_state(n2c, 20);
    navy2.add_province(n2st, n2c, n2c, 0, 1000.0, 0.0);
    navy2.state(n2st)->military_factories = 1;
    navy2.state(n2st)->dockyards = 1;
    navy2.add_line(n2c, n2rifle, 1, 1.0, 1.0, false);
    const size_t n2ship_line = navy2.add_line(n2c, n2ship, 1, 1.0, 1.0, false);
    navy2.state(n2st)->military_factories = 0;
    navy2.advance_industry(1);
    CHECK_EQ(navy2.line(n2c, 0).factories, 0);
    CHECK(navy2.line(n2c, 0).equipment.valid() == false);
    CHECK_EQ(navy2.line(n2c, n2ship_line).factories, 1);
    CHECK(check_invariants(navy2.g).empty());
}

// A switch driven through the command path must reach industry's retention rule
// exactly once (the pending-switch queue in `ProductionLine::previous`).
HOI_TEST(production_line_switch_through_command_applies_retention_once) {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle_1 = f.equipment("rifle_1", "infantry_rifle", 4.0, 1.0);
    const EquipmentId rifle_2 = f.equipment("rifle_2", "infantry_rifle", 5.0, 1.0);
    const StateId st = f.add_state(c, 20);
    f.state(st)->military_factories = 1;
    f.add_province(st, c, c, 0, 1000.0, 0.0);

    Command cmd;
    cmd.type = CommandType::SetProductionLine;
    cmd.country = c;
    cmd.equipment = rifle_1;
    cmd.value = 1;
    CHECK_EQ(validate_command(f.g, cmd), CommandResult::Applied);
    CHECK_EQ(apply_command(f.g, cmd), CommandResult::Applied);
    CHECK_EQ(f.country(c)->lines.size(), size_t{1});
    f.line(c, 0).efficiency = 0.90;
    f.line(c, 0).efficiency_cap = 0.90;

    // Retire the model, then build its same-archetype successor.
    cmd.value = 0;
    CHECK_EQ(apply_command(f.g, cmd), CommandResult::Applied);
    CHECK_EQ(f.line(c, 0).previous.size(), size_t{1});
    cmd.equipment = rifle_2;
    cmd.value = 1;
    CHECK_EQ(apply_command(f.g, cmd), CommandResult::Applied);
    CHECK_EQ(f.line(c, 0).equipment, rifle_2);

    // One industry hour consumes the pending switch and retains 70% once: a
    // double application would land at 0.49.
    f.advance_industry(1);
    CHECK(f.line(c, 0).previous.empty());
    CHECK_NEAR(f.line(c, 0).efficiency, 0.90 * 0.70, 1e-9);
    CHECK_NEAR(f.line(c, 0).efficiency_cap, 0.90 * 0.70 + 0.001667 / 24.0, 1e-9);
}

// ------------------------------------------------------------------ 5.2 -----

HOI_TEST(construction_completes_factory_project_and_raises_state_factories) {
    Fixture f;
    f.set_max_factories_per_project(7.0);
    const CountryId c = f.add_country("AAA");
    const StateId industry_state = f.add_state(c, 100);
    const StateId target_state = f.add_state(c, 5);
    f.state(industry_state)->civilian_factories = 50;
    f.country(c)->consumer_goods_ratio = 0.0;

    // Consumer goods cut the civilian capacity available for construction.
    CHECK_NEAR(construction_output(f.g, *f.country(c)), 50.0 * 5.0 / 24.0, 1e-12);
    CHECK_NEAR(consumer_goods_factories(f.g, *f.country(c)), 0.0, 1e-12);
    f.country(c)->consumer_goods_ratio = 0.5;
    CHECK_NEAR(consumer_goods_factories(f.g, *f.country(c)), 25.0, 1e-12);
    CHECK_NEAR(construction_output(f.g, *f.country(c)), 50.0 * 5.0 / 24.0 * 0.5, 1e-12);
    f.country(c)->consumer_goods_ratio = 0.0;

    ConstructionProject project;
    project.kind = BuildingKind::CivilianFactory;
    project.state = target_state;
    project.target_level = 1;
    f.country(c)->construction.queue.push_back(project);

    // A project holds at most `max_factories_per_project` factories, so 50 civilian
    // factories build at the capped rate; the cap is read from the content the test
    // builds, never hardcoded, so tuning it cannot stale this test.
    const SimConstants& k = f.g.content.constants;
    const double cap = k.max_factories_per_project;
    const double per_project_rate = cap * k.ic_per_civilian_factory / 24.0;
    f.advance_industry(10);
    CHECK_EQ(f.state(target_state)->civilian_factories, 0);
    CHECK_NEAR(f.country(c)->construction.queue.front().progress, 10.0 * per_project_rate, 1e-9);

    // A level-0 factory costs the plain base cost (linear scaling adds nothing at
    // level 0), so it should take base_cost / rate hours.
    const double expected_hours = k.construction_cost_factory / per_project_rate;
    int hours = 10;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    CHECK_NEAR(static_cast<double>(hours), expected_hours, 1.0);
    CHECK_EQ(f.state(target_state)->civilian_factories, 1);
    CHECK_EQ(f.state(industry_state)->civilian_factories, 50);
    CHECK(f.country(c)->construction.queue.empty());

    // Building slots are a hard limit: a full state blocks, and the blocked project
    // stays queued without eating capacity.
    Fixture blocked;
    const CountryId bc = blocked.add_country("AAA");
    const StateId full = blocked.add_state(bc, 1);
    blocked.state(full)->civilian_factories = 1;
    ConstructionProject blocked_project;
    blocked_project.kind = BuildingKind::CivilianFactory;
    blocked_project.state = full;
    blocked_project.target_level = 2;
    blocked.country(bc)->construction.queue.push_back(blocked_project);
    blocked.advance_industry(48);
    CHECK_EQ(blocked.country(bc)->construction.queue.size(), size_t{1});
    CHECK_EQ(blocked.state(full)->civilian_factories, 1);
}

HOI_TEST(construction_capacity_splits_across_the_queue) {
    Fixture f;
    f.set_max_factories_per_project(7.0);
    const CountryId c = f.add_country("AAA");
    const StateId pool_state = f.add_state(c, 50);
    const StateId site_a = f.add_state(c, 5);
    const StateId site_b = f.add_state(c, 5);
    const SimConstants& k = f.g.content.constants;
    // Everything below is derived from the content the test builds, so re-tuning the
    // cap or the factory output cannot stale this test.
    const double cap = k.max_factories_per_project;
    const double rate_per_factory = k.ic_per_civilian_factory / 24.0;
    const double pool = cap + 3.0;
    const double second_share = std::min(3.0, cap);  // what is left of the pool for #2
    f.state(pool_state)->civilian_factories = static_cast<int>(pool);
    f.country(c)->consumer_goods_ratio = 0.0;

    // Stamped costs so the projects complete inside the test's short window; the
    // production rules under test (who gets how many factories) are unaffected.
    const double stamped_cost = 10.0;
    ConstructionProject first;
    first.kind = BuildingKind::CivilianFactory;
    first.state = site_a;
    first.target_level = 1;
    first.cost = stamped_cost;
    ConstructionProject second;
    second.kind = BuildingKind::CivilianFactory;
    second.state = site_b;
    second.target_level = 1;
    second.cost = stamped_cost;
    f.country(c)->construction.queue.push_back(first);
    f.country(c)->construction.queue.push_back(second);

    f.advance_industry(1);
    CHECK_EQ(f.country(c)->construction.queue.size(), size_t{2});
    // The pool splits as cap + 3 in queue order, and both projects progress in the
    // same tick.
    CHECK_NEAR(f.country(c)->construction.queue[0].progress, cap * rate_per_factory, 1e-12);
    CHECK_NEAR(f.country(c)->construction.queue[1].progress, second_share * rate_per_factory, 1e-12);

    // Hours until the head project is paid for at its capped rate.
    const int head_hours = static_cast<int>(std::ceil(stamped_cost / (cap * rate_per_factory) - 1e-9));
    f.advance_industry(head_hours - 2);
    CHECK_EQ(f.country(c)->construction.queue.size(), size_t{2});
    CHECK_NEAR(f.country(c)->construction.queue[0].progress, (head_hours - 1) * cap * rate_per_factory,
               1e-9);
    CHECK_NEAR(f.country(c)->construction.queue[1].progress,
               (head_hours - 1) * second_share * rate_per_factory, 1e-9);

    f.advance_industry(1);
    CHECK_EQ(f.state(site_a)->civilian_factories, 1);
    CHECK_EQ(f.country(c)->construction.queue.size(), size_t{1});
    const double carried = f.country(c)->construction.queue[0].progress;
    CHECK_NEAR(carried, head_hours * second_share * rate_per_factory, 1e-9);

    f.advance_industry(1);
    // The factories the finished project held return to the pool, lifting the
    // remaining project to the full cap instead of stalling it.
    CHECK_NEAR(f.country(c)->construction.queue[0].progress - carried, cap * rate_per_factory, 1e-9);
    int extra = 0;
    while (!f.country(c)->construction.queue.empty() && extra < 1000) {
        f.advance_industry(1);
        ++extra;
    }
    CHECK_EQ(f.state(site_b)->civilian_factories, 1);
    CHECK(extra <= head_hours);

    // Capacity the queue cannot absorb is idle: one project with pool > cap
    // factories available still builds at the capped rate.
    Fixture idle;
    idle.set_max_factories_per_project(7.0);
    const CountryId ic = idle.add_country("AAA");
    const StateId idle_pool = idle.add_state(ic, 50);
    const StateId idle_site = idle.add_state(ic, 5);
    idle.state(idle_pool)->civilian_factories = static_cast<int>(pool);
    idle.country(ic)->consumer_goods_ratio = 0.0;
    ConstructionProject single;
    single.kind = BuildingKind::CivilianFactory;
    single.state = idle_site;
    single.target_level = 1;
    idle.country(ic)->construction.queue.push_back(single);
    idle.advance_industry(1);
    CHECK_NEAR(idle.country(ic)->construction.queue.front().progress, cap * rate_per_factory, 1e-12);
}

HOI_TEST(synthetic_refinery_completion_adds_oil_and_rubber) {
    Fixture f;
    f.set_max_factories_per_project(7.0);
    const CountryId c = f.add_country("AAA");
    const StateId industry_state = f.add_state(c, 100);
    const StateId refinery_state = f.add_state(c, 4);
    f.state(industry_state)->civilian_factories = 100;
    f.country(c)->consumer_goods_ratio = 0.0;
    // A province with no oil or rubber of its own: everything must come from the
    // refinery.
    f.add_province(refinery_state, c, c, 0, 0.0, 0.0);

    double produced[RESOURCE_COUNT];
    compute_resource_production(f.g.world, f.g.content, c, produced);
    CHECK_NEAR(produced[OIL], 0.0, 1e-12);
    CHECK_NEAR(produced[static_cast<int>(Resource::Rubber)], 0.0, 1e-12);

    ConstructionProject project;
    project.kind = BuildingKind::SyntheticRefinery;
    project.state = refinery_state;
    project.target_level = 1;
    f.country(c)->construction.queue.push_back(project);

    int hours = 0;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    CHECK(f.country(c)->construction.queue.empty());
    // A level-0 refinery costs the plain synthetic base cost and runs at the
    // content's per-project factory cap; the expected hours follow from the content
    // constants rather than from a literal.
    const SimConstants& rk = f.g.content.constants;
    const double expected_hours =
        rk.construction_cost_synthetic / (rk.max_factories_per_project * rk.ic_per_civilian_factory /
                                          24.0);
    CHECK_NEAR(static_cast<double>(hours), expected_hours, 1.0);
    CHECK_EQ(f.state(refinery_state)->synthetic_refineries, 1);

    compute_resource_production(f.g.world, f.g.content, c, produced);
    CHECK_NEAR(produced[OIL], 2.0, 1e-12);
    CHECK_NEAR(produced[static_cast<int>(Resource::Rubber)], 1.0, 1e-12);
    // The country's published totals are computed at the start of each hour, so the
    // refinery completed during the previous hour shows up one tick later.
    f.advance_industry(1);
    CHECK_NEAR(f.country(c)->resources_produced[OIL], 2.0, 1e-12);

    // A second refinery stacks, and a project that arrives without a cost gets the
    // command layer's formula: base * (1 + (1.25 - 1) * existing_level). At level 1
    // that is 8000 * 1.25 = 10000, and at level 2 it is 12000 (exponential scaling
    // would give 12500, so the level-2 case pins the linear rule).
    ConstructionProject second;
    second.kind = BuildingKind::SyntheticRefinery;
    second.state = refinery_state;
    second.target_level = 2;
    f.country(c)->construction.queue.push_back(second);
    f.advance_industry(1);
    CHECK_NEAR(f.country(c)->construction.queue.front().cost, 8000.0 * 1.25, 1e-9);
    hours = 0;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    CHECK_EQ(f.state(refinery_state)->synthetic_refineries, 2);

    ConstructionProject third;
    third.kind = BuildingKind::SyntheticRefinery;
    third.state = refinery_state;
    third.target_level = 3;
    f.country(c)->construction.queue.push_back(third);
    f.advance_industry(1);
    CHECK_NEAR(f.country(c)->construction.queue.front().cost, 8000.0 * 1.5, 1e-9);
    hours = 0;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    CHECK_EQ(f.state(refinery_state)->synthetic_refineries, 3);
    compute_resource_production(f.g.world, f.g.content, c, produced);
    CHECK_NEAR(produced[OIL], 6.0, 1e-12);
}

HOI_TEST(anti_air_project_completes_and_raises_the_province_level) {
    Fixture f;
    f.set_max_factories_per_project(7.0);
    const CountryId c = f.add_country("AAA");
    const StateId st = f.add_state(c, 10);
    const ProvinceId p = f.add_province(st, c, c, 3, 0.0, 0.0);
    f.state(st)->civilian_factories = 50;
    f.country(c)->consumer_goods_ratio = 0.0;
    f.add_building("anti_air", BuildingKind::AntiAir, 2000.0, 5);

    ConstructionProject project;
    project.kind = BuildingKind::AntiAir;
    project.province = p;
    project.state = st;
    project.target_level = 1;
    f.country(c)->construction.queue.push_back(project);

    int hours = 0;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    // Anti-air is a province building: completing it raises the level and nothing
    // else, and the project leaves the queue.
    CHECK(f.country(c)->construction.queue.empty());
    CHECK_EQ(f.province(p)->anti_air, 1);
    CHECK_EQ(f.province(p)->fort_level, 0);
    CHECK_EQ(f.province(p)->infrastructure, 3);
    const SimConstants& k = f.g.content.constants;
    const double rate = k.max_factories_per_project * k.ic_per_civilian_factory / 24.0;
    CHECK_NEAR(static_cast<double>(hours), 2000.0 / rate, 1.0);

    // The next level scales from the level already standing, and the content's
    // maximum level still applies.
    ConstructionProject second;
    second.kind = BuildingKind::AntiAir;
    second.province = p;
    second.state = st;
    second.target_level = 2;
    f.country(c)->construction.queue.push_back(second);
    f.advance_industry(1);
    CHECK_NEAR(f.country(c)->construction.queue.front().cost,
               2000.0 * (1.0 + (k.construction_level_scaling - 1.0)), 1e-9);
    hours = 0;
    while (!f.country(c)->construction.queue.empty() && hours < 40000) {
        f.advance_industry(1);
        ++hours;
    }
    CHECK_EQ(f.province(p)->anti_air, 2);

    f.province(p)->anti_air = 5;  // at the data's maximum for anti-air
    ConstructionProject third;
    third.kind = BuildingKind::AntiAir;
    third.province = p;
    third.state = st;
    third.target_level = 6;
    f.country(c)->construction.queue.push_back(third);
    f.advance_industry(1);
    CHECK(f.country(c)->construction.queue.empty());
    CHECK_EQ(f.province(p)->anti_air, 5);
}

// ------------------------------------------------------------------ 5.3 -----

HOI_TEST(research_completes_tech_and_applies_modifiers_and_unlocks) {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle_1 = f.equipment("rifle_1", "infantry_rifle", 4.0, 1.0);
    const EquipmentId rifle_2 = f.equipment("rifle_2", "infantry_rifle", 5.0, 1.0);
    f.add_building("civilian_factory", BuildingKind::CivilianFactory, 10800.0, 20);
    f.add_building("radar_station", BuildingKind::Radar, 2400.0, 5);
    const TechId tools = f.add_tech("improved_tools", 1936, 10.0, {}, {"rifle_2"}, {"radar_station"},
                                    ModifierKind::FactoryOutput, 0.15);
    const TechId late = f.add_tech("late_tech", 1940, 10.0);

    CHECK(equipment_unlocked(f.g, c, rifle_2) == false);
    CHECK(equipment_unlocked(f.g, c, rifle_1) == true);
    CHECK(building_unlocked(f.g, c, "radar_station") == false);
    CHECK(building_unlocked(f.g, c, "civilian_factory") == true);

    // 10 days at speed 1, and 4 years ahead of time costs the documented penalty.
    CHECK_NEAR(tech_cost_days(f.g, c, tools), 10.0, 1e-12);
    CHECK_NEAR(tech_cost_days(f.g, c, late), 10.0 * (1.0 + 0.5 * 4.0), 1e-12);
    CHECK(tech_available(f.g, c, tools));
    CHECK(tech_available(f.g, c, late));  // the year is a cost, not a gate

    ResearchSlot slot;
    slot.tech = tools;
    slot.active = true;
    f.country(c)->research.slots.push_back(slot);

    f.advance_research(239);
    CHECK_EQ(f.country(c)->research.completed.size(), size_t{0});
    f.advance_research(2);  // a 10-day technology at 1 day per 24 ticks
    CHECK_EQ(f.country(c)->research.completed.size(), size_t{1});
    CHECK(f.country(c)->research.has_tech(tools));
    CHECK_NEAR(f.country(c)->tech_modifiers.get(ModifierKind::FactoryOutput), 0.15, 1e-12);
    CHECK(f.country(c)->research.slots.front().active == false);
    CHECK(tech_available(f.g, c, tools) == false);  // already known
    CHECK(equipment_unlocked(f.g, c, rifle_2));
    CHECK(building_unlocked(f.g, c, "radar_station"));
    CHECK_EQ(f.g.events.size(), size_t{1});
    CHECK_EQ(f.g.events.front().kind, std::string("research"));

    // Applying twice must not double the modifiers.
    apply_tech_effects(f.g, *f.country(c), tools);
    CHECK_NEAR(f.country(c)->tech_modifiers.get(ModifierKind::FactoryOutput), 0.15, 1e-12);

    // A research-speed modifier shortens the calendar time exactly once: +50%
    // speed costs 10 / 1.5 = 6.67 days, and applying the bonus in both the cost and
    // the progress rate would (wrongly) finish it in 160/1.5 ticks instead.
    Fixture fast;
    const CountryId fc = fast.add_country("AAA");
    const TechId fast_tech = fast.add_tech("improved_tools", 1936, 10.0);
    fast.country(fc)->tech_modifiers.set(ModifierKind::ResearchSpeed, 0.5);
    ResearchSlot fast_slot;
    fast_slot.tech = fast_tech;
    fast_slot.active = true;
    fast.country(fc)->research.slots.push_back(fast_slot);
    CHECK_NEAR(tech_cost_days(fast.g, fc, fast_tech), 10.0 / 1.5, 1e-12);
    fast.advance_research(155);
    CHECK_EQ(fast.country(fc)->research.completed.size(), size_t{0});
    fast.advance_research(10);
    CHECK_EQ(fast.country(fc)->research.completed.size(), size_t{1});

    // A prerequisite chain gates availability: the dependent tech is only
    // available once its prerequisite is completed.
    const TechId follower = f.add_tech("improved_tools_2", 1937, 10.0, {tools});
    CHECK(tech_available(f.g, c, follower));
    const CountryId other = f.add_country("BBB");
    CHECK(tech_available(f.g, other, follower) == false);
    CHECK(tech_available(f.g, other, tools));
}

// -------------------------------------------------------- ordering ----------

namespace {

// Identical world with two lines competing for one resource: the first line in
// `Country::lines` order is served first.
Fixture build_competing_world() {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 2.0);
    const StateId st = f.add_state(c, 10);
    f.add_province(st, c, c, 0, 1.0, 0.0);
    f.add_line(c, rifle, 1, 1.0, 1.0);
    f.add_line(c, rifle, 1, 1.0, 1.0);
    return f;
}

}  // namespace

HOI_TEST(allocation_order_is_deterministic_and_prioritised) {
    Fixture f = build_competing_world();
    const CountryId c = f.add_country("BBB");  // a second country, processed after the first
    (void)c;
    f.advance_industry(1);
    const ProductionLine& first = f.line(f.g.world.countries.raw_items()[0].id, 0);
    const ProductionLine& second = f.line(f.g.world.countries.raw_items()[0].id, 1);
    CHECK_LT(first.resource_shortage, second.resource_shortage);
    CHECK_GT(first.output_today, second.output_today);
    // The first line takes everything: 1 steel/day is 1/24 per hour while its
    // factory wants 2/24, so it runs at half output and the next line gets 0.
    CHECK_NEAR(second.resource_shortage, 0.9, 1e-12);

    Fixture left = build_competing_world();
    Fixture right = build_competing_world();
    left.advance_industry(240);
    right.advance_industry(240);
    const CountryId lc = left.g.world.countries.raw_items()[0].id;
    const CountryId rc = right.g.world.countries.raw_items()[0].id;
    CHECK_EQ(left.country(lc)->equipment_stockpile.size(),
             right.country(rc)->equipment_stockpile.size());
    for (size_t i = 0; i < left.country(lc)->equipment_stockpile.size(); ++i) {
        // Bit-exact: the allocation order and float operations are reproducible.
        CHECK_EQ(left.country(lc)->equipment_stockpile[i], right.country(rc)->equipment_stockpile[i]);
    }
    CHECK_GT(left.country(lc)->equipment_stockpile[0], 0.0);
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        CHECK_EQ(left.country(lc)->resources_consumed[r], right.country(rc)->resources_consumed[r]);
    }
    CHECK_EQ(left.line(lc, 0).efficiency, right.line(rc, 0).efficiency);
    CHECK_EQ(left.line(lc, 1).efficiency, right.line(rc, 1).efficiency);
}

// ------------------------------------------------- reinforcement ------------

HOI_TEST(reinforcement_draws_from_stockpile_within_template_limits) {
    Fixture f;
    const CountryId c = f.add_country("AAA");
    const EquipmentId rifle = f.equipment("rifle", "infantry_rifle", 4.0, 1.0,
                                          EquipmentCategory::Infantry, 10.0, 50.0);
    const TemplateId tpl = f.division_template("infantry", rifle, 10, 1000.0, 100.0);
    const StateId st = f.add_state(c, 10);
    const ProvinceId p = f.add_province(st, c, c, 3, 0.0, 0.0);

    const DivisionId did = f.add_division(c, tpl, p, 0.5);
    Division* d = f.g.world.division(did);
    d->equipment.assign(1, 5.0);
    d->manpower = 500.0;
    f.set_stockpile(c, rifle, 3.0);

    // Missing gear (10 - 5) dominates the strength deficit (10 * 0.5).
    std::vector<double> demand;
    compute_equipment_demand(f.g, c, &demand);
    CHECK_EQ(demand.size(), size_t{1});
    CHECK_NEAR(demand[0], 5.0, 1e-12);

    // An off-map (training) division needs its whole template, but only once.
    const DivisionId trainee = f.add_division(c, tpl, ProvinceId{}, 1.0);
    f.country(c)->training.push_back(TrainingDivision{trainee, tpl, 10.0});
    f.country(c)->training.push_back(TrainingDivision{DivisionId{}, tpl, 10.0});
    compute_equipment_demand(f.g, c, &demand);
    CHECK_NEAR(demand[0], 5.0 + 10.0 + 10.0, 1e-12);

    // Reinforcement is limited by what the stockpile holds, then by the template.
    // (The division pointer is re-fetched: creating the trainee above can move the
    // store's storage.)
    d = f.g.world.division(did);
    CHECK_NEAR(reinforce_division(f.g, *d, rifle, 2.0), 2.0, 1e-12);
    CHECK_NEAR(f.stockpile(c, rifle), 1.0, 1e-12);
    CHECK_NEAR(d->equipment[0], 7.0, 1e-12);
    // 2 rifles * 10 hit points each on a 100 hit point template.
    CHECK_NEAR(d->strength, 0.7, 1e-12);
    CHECK_NEAR(d->manpower, 700.0, 1e-12);

    CHECK_NEAR(reinforce_division(f.g, *d, rifle, 100.0), 1.0, 1e-12);
    CHECK_NEAR(f.stockpile(c, rifle), 0.0, 1e-12);
    CHECK_NEAR(reinforce_division(f.g, *d, rifle, 1.0), 0.0, 1e-12);  // stockpile empty

    // The template cap holds: extra gear cannot push past 1.0 strength.
    f.set_stockpile(c, rifle, 100.0);
    CHECK_NEAR(reinforce_division(f.g, *d, rifle, 100.0), 2.0, 1e-12);
    CHECK_NEAR(d->equipment[0], 10.0, 1e-12);
    CHECK_NEAR(d->strength, 1.0, 1e-12);
    CHECK_NEAR(d->manpower, 1000.0, 1e-12);
    CHECK_NEAR(reinforce_division(f.g, *d, rifle, 100.0), 0.0, 1e-12);
    CHECK_NEAR(f.stockpile(c, rifle), 98.0, 1e-12);

    // Factories drive count_factories/industry_intact.
    f.state(st)->civilian_factories = 2;
    int civ = 0, mil = 0, dock = 0;
    count_factories(f.g.world, c, &civ, &mil, &dock);
    CHECK_EQ(civ, 2);
    CHECK_EQ(mil, 0);
    CHECK_EQ(dock, 0);
    CHECK(industry_intact(f.g.world, c));
    CHECK(industry_intact(f.g.world, CountryId{}) == false);
}
