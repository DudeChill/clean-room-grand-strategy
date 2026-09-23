// Supply network tests: hand-built worlds, no scenario files, no simulation loop
// except where the phase itself is under test.

#include <algorithm>
#include <string>
#include <vector>

#include "game/game.h"
#include "sim/diplomacy.h"
#include "sim/supply.h"
#include "test.h"

namespace {

using namespace hoi;

// ---------------------------------------------------------------- builders --

StateId make_state(World& w, const std::string& name, RegionId region) {
    StateId id = w.states.create();
    State* s = w.states.try_get(id);
    s->id = id;
    s->name = name;
    s->region = region;
    return id;
}

// Provinces start at zero infrastructure/railway so the expected capacity is exactly
// `10 * (1 + 0.15 * railway)`.
ProvinceId make_province(World& w, const std::string& name, StateId state, RegionId region) {
    ProvinceId id = w.provinces.create();
    Province* p = w.provinces.try_get(id);
    p->id = id;
    p->name = name;
    p->state = state;
    p->region = region;
    p->terrain = Terrain::Plains;
    p->infrastructure = 0;
    p->railway_level = 0;
    w.states.try_get(state)->provinces.push_back(id);
    return id;
}

void link(World& w, ProvinceId a, ProvinceId b) {
    w.provinces.try_get(a)->adj.push_back(b);
    w.provinces.try_get(b)->adj.push_back(a);
    for (ProvinceId p : {a, b}) {
        std::vector<ProvinceId>& adj = w.provinces.try_get(p)->adj;
        std::sort(adj.begin(), adj.end());
        adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
    }
}

CountryId make_country(World& w, const std::string& tag) {
    CountryId id = w.countries.create();
    Country* c = w.countries.try_get(id);
    c->id = id;
    c->tag = tag;
    c->name = tag;
    return id;
}

void give_state(World& w, StateId state, CountryId owner) {
    State* s = w.states.try_get(state);
    s->owner = owner;
    s->controller = owner;
    s->core_owners.push_back(owner);
    for (ProvinceId pid : s->provinces) {
        Province* p = w.provinces.try_get(pid);
        p->owner = owner;
        p->controller = owner;
    }
}

// A straight chain of `count` provinces in one state owned by `owner`, with the
// first one flagged as the capital.
std::vector<ProvinceId> make_chain(World& w, size_t count, StateId state, RegionId region,
                                   CountryId owner) {
    std::vector<ProvinceId> chain;
    for (size_t i = 0; i < count; ++i) {
        chain.push_back(make_province(w, "p" + std::to_string(i), state, region));
    }
    for (size_t i = 1; i < chain.size(); ++i) link(w, chain[i - 1], chain[i]);
    give_state(w, state, owner);
    w.provinces.try_get(chain.front())->is_capital = true;
    w.countries.try_get(owner)->capital = state;
    return chain;
}

// Divisions reference template 0, registered by the test before phase_supply runs.
DivisionId add_division(World& w, CountryId c, ProvinceId location) {
    DivisionId id = w.divisions.create();
    Division* d = w.divisions.try_get(id);
    d->id = id;
    d->country = c;
    d->location = location;
    d->strength = 1.0;
    d->supply = 1.0;
    d->fuel = 1.0;
    d->template_id = TemplateId(0);
    return id;
}

void register_template(Game& g, double supply_use, double fuel_use) {
    DivisionTemplate t;
    t.id = TemplateId(0);
    t.key = "test_infantry";
    t.name = "Test Infantry";
    t.supply_use = supply_use;
    t.fuel_use = fuel_use;
    g.content.templates.push_back(t);
}

}  // namespace

// Capacity falls with graph distance from the source: each hop costs 10% of it.
HOI_TEST(supply_drops_with_distance_from_hub) {
    Game g;
    const RegionId region = g.world.regions.create();
    const StateId state = make_state(g.world, "home", region);
    const CountryId alpha = make_country(g.world, "AAA");
    const std::vector<ProvinceId> chain = make_chain(g.world, 5, state, region, alpha);

    std::vector<SupplyRouteStep> path;
    ProvinceId bottleneck;
    const double far = explain_supply_route(g, alpha, chain[4], &path, &bottleneck);

    // Capital capacity 10, four hops: 10 / (1 + 0.1 * 4).
    CHECK_NEAR(far, 10.0 / 1.4, 1e-9);
    CHECK_NEAR(explain_supply_route(g, alpha, chain[0], nullptr, nullptr), 10.0, 1e-9);
    CHECK_NEAR(explain_supply_route(g, alpha, chain[1], nullptr, nullptr), 10.0 / 1.1, 1e-9);
    CHECK(path.size() == 5);
    CHECK_EQ(path.front().province, chain[0]);
    CHECK_EQ(path.back().province, chain[4]);
    for (size_t i = 1; i < path.size(); ++i) CHECK_LT(path[i].capacity, path[i - 1].capacity);
    CHECK(bottleneck.valid());

    phase_supply(g);
    for (ProvinceId pid : chain) {
        CHECK(g.world.provinces.try_get(pid)->supply_source == chain[0]);
        CHECK_NEAR(g.world.provinces.try_get(pid)->supply_level, 1.0, 1e-12);
    }
}

// A hub on a rail line outranks the capital once the capital is far enough away, and
// losing it cuts every province beyond it off the network.
HOI_TEST(supply_capturing_only_hub_zeroes_far_provinces) {
    Game g;
    const RegionId region = g.world.regions.create();
    const StateId state = make_state(g.world, "home", region);
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const std::vector<ProvinceId> chain = make_chain(g.world, 4, state, region, alpha);

    Province* hub = g.world.provinces.try_get(chain[1]);
    hub->supply_hub = true;
    hub->railway_level = 3;  // capacity 10 * 1.45
    CHECK(supply_sources(g.world, alpha).size() == 2);

    phase_supply(g);
    // Two hops from the hub: 14.5 / (1 + 0.1 * 2), more than the capital delivers.
    CHECK_EQ(g.world.provinces.try_get(chain[3])->supply_source, chain[1]);
    CHECK_NEAR(explain_supply_route(g, alpha, chain[3], nullptr, nullptr), 14.5 / 1.2, 1e-9);

    hub->controller = beta;  // the only hub is taken
    CHECK(supply_sources(g.world, alpha).size() == 1);
    phase_supply(g);
    CHECK_NEAR(g.world.provinces.try_get(chain[3])->supply_level, 0.0, 1e-12);
    CHECK(!g.world.provinces.try_get(chain[3])->supply_source.valid());
    CHECK_NEAR(g.world.provinces.try_get(chain[2])->supply_level, 0.0, 1e-12);
    CHECK_NEAR(g.world.provinces.try_get(chain[0])->supply_level, 1.0, 1e-12);
}

// A pocket held by the country but surrounded by enemy ground has no path back to a
// source: encirclement is emergent, not a special case.
HOI_TEST(supply_encircled_province_gets_zero) {
    Game g;
    const RegionId region = g.world.regions.create();
    const StateId home = make_state(g.world, "home", region);
    const StateId front = make_state(g.world, "front", region);
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");

    const std::vector<ProvinceId> chain = make_chain(g.world, 2, home, region, alpha);
    give_state(g.world, front, beta);

    const ProvinceId pocket = make_province(g.world, "pocket", front, region);
    const ProvinceId around_a = make_province(g.world, "around_a", front, region);
    const ProvinceId around_b = make_province(g.world, "around_b", front, region);
    link(g.world, pocket, around_a);
    link(g.world, pocket, around_b);
    for (ProvinceId pid : {around_a, around_b}) {
        g.world.provinces.try_get(pid)->owner = beta;
        g.world.provinces.try_get(pid)->controller = beta;
    }
    g.world.provinces.try_get(pocket)->owner = alpha;
    g.world.provinces.try_get(pocket)->controller = alpha;

    phase_supply(g);
    CHECK_NEAR(g.world.provinces.try_get(pocket)->supply_level, 0.0, 1e-12);
    CHECK(!g.world.provinces.try_get(pocket)->supply_source.valid());
    CHECK_NEAR(explain_supply_route(g, alpha, pocket, nullptr, nullptr), 0.0, 1e-12);
    CHECK_NEAR(g.world.provinces.try_get(chain[1])->supply_level, 1.0, 1e-12);
}

// Province level drives the division level, the national fuel stock is divided
// across consuming divisions, and supply tapers past the hub radius.
HOI_TEST(supply_division_reads_province_and_hub_radius) {
    Game g;
    const RegionId region = g.world.regions.create();
    const StateId state = make_state(g.world, "home", region);
    const CountryId alpha = make_country(g.world, "AAA");
    // Nine provinces: the last is eight hops out, beyond the six-province hub radius.
    const std::vector<ProvinceId> chain = make_chain(g.world, 9, state, region, alpha);
    register_template(g, 1.0, 2.0);
    const DivisionId far = add_division(g.world, alpha, chain[8]);
    const DivisionId near = add_division(g.world, alpha, chain[1]);
    g.world.countries.try_get(alpha)->fuel = 1000.0;

    phase_supply(g);
    const Division* d = g.world.divisions.try_get(far);
    const Division* n = g.world.divisions.try_get(near);
    CHECK_NEAR(n->supply, 1.0, 1e-12);
    // 10 / 1.8 is still above demand, so only the radius taper applies: 1 / (1 + 0.1*2).
    CHECK_NEAR(d->supply, 1.0 / 1.2, 1e-9);
    CHECK_NEAR(d->fuel, 1.0, 1e-12);
    CHECK_NEAR(g.world.countries.try_get(alpha)->fuel, 1000.0 - 2.0 * 0.02 * 2.0, 1e-9);

    // A starved country cannot fully fuel its divisions; the stock is spent in
    // ascending division order, so the lowest id takes what is left and the rest get
    // nothing this tick.
    g.world.countries.try_get(alpha)->fuel = 0.02;  // half of one division's demand
    phase_supply(g);
    CHECK_LT(far.v, near.v);
    CHECK_NEAR(g.world.divisions.try_get(far)->fuel, 0.5, 1e-9);
    CHECK_NEAR(g.world.divisions.try_get(near)->fuel, 0.0, 1e-12);
    CHECK_NEAR(g.world.countries.try_get(alpha)->fuel, 0.0, 1e-12);
}

// A hub held by a co-belligerent is a valid source, and the best source (not the
// nearest one) wins.
HOI_TEST(supply_co_belligerent_hub_supplies_ally) {
    Game g;
    const RegionId region = g.world.regions.create();
    const StateId alpha_home = make_state(g.world, "alpha_home", region);
    const StateId gamma_home = make_state(g.world, "gamma_home", region);
    const StateId beta_home = make_state(g.world, "beta_home", region);
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId gamma = make_country(g.world, "CCC");
    const CountryId beta = make_country(g.world, "BBB");

    const std::vector<ProvinceId> chain =
        make_chain(g.world, 3, alpha_home, region, alpha);
    give_state(g.world, gamma_home, gamma);
    give_state(g.world, beta_home, beta);

    const ProvinceId hub = make_province(g.world, "hub", gamma_home, region);
    link(g.world, hub, chain[2]);
    g.world.provinces.try_get(hub)->owner = gamma;
    g.world.provinces.try_get(hub)->controller = gamma;
    g.world.provinces.try_get(hub)->supply_hub = true;
    g.world.provinces.try_get(hub)->railway_level = 2;  // gamma's hub: capacity 13

    Faction faction;
    faction.id = 7;
    faction.leader = alpha;
    faction.members = {alpha, gamma};
    g.world.factions.push_back(faction);
    g.world.countries.try_get(alpha)->faction = 7;
    g.world.countries.try_get(gamma)->faction = 7;
    CHECK(declare_war(g, alpha, beta, {}).valid());

    const std::vector<CountryId> allies = co_belligerents(g.world, alpha);
    CHECK(std::find(allies.begin(), allies.end(), gamma) != allies.end());

    phase_supply(g);
    // The ally hub wins on capacity, not proximity: one hop out it delivers
    // 13 / 1.1, where the capital delivers 10 / 1.2 at two hops.
    CHECK_EQ(g.world.provinces.try_get(chain[2])->supply_source, hub);
    CHECK_NEAR(g.world.provinces.try_get(chain[2])->supply_level, 1.0, 1e-12);

    // Once the ally is neither at war nor in the faction, its hub stops counting.
    g.world.wars.for_each([&](WarId, War& war) { war.active = false; });
    g.world.factions[0].members = {alpha};
    CHECK(supply_sources(g.world, alpha).size() == 1);
}
