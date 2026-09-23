// Supply network tests: hand-built worlds, no scenario files, no simulation loop
// except where the phase itself is under test.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "game/game.h"
#include "save/save.h"
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

// ---- ports, sea zones and convoys -------------------------------------------

// A sea zone: a sea province in a sea region. Sea provinces carry the full generated
// neighbour list in `adj` and sea neighbours in `sea_adj`, exactly as the loader
// leaves them.
RegionId make_sea_region(World& w, const std::string& name) {
    Region r;
    r.name = name;
    r.is_sea = true;
    const RegionId id = w.regions.create(r);
    w.regions.try_get(id)->id = id;
    return id;
}

ProvinceId make_sea(World& w, const std::string& name, RegionId region) {
    ProvinceId id = w.provinces.create();
    Province* p = w.provinces.try_get(id);
    p->id = id;
    p->name = name;
    p->region = region;
    p->is_sea = true;
    p->terrain = Terrain::Ocean;
    w.regions.try_get(region)->provinces.push_back(id);
    return id;
}

// Land province <-> sea zone: the land side records the zone in sea_adj, the sea side
// records the land province in its full neighbour list.
void link_coast(World& w, ProvinceId land, ProvinceId sea) {
    w.provinces.try_get(land)->coastal = true;
    w.provinces.try_get(land)->sea_adj.push_back(sea);
    w.provinces.try_get(sea)->adj.push_back(land);
}

void link_sea_zones(World& w, ProvinceId a, ProvinceId b) {
    Province* pa = w.provinces.try_get(a);
    Province* pb = w.provinces.try_get(b);
    pa->adj.push_back(b);
    pa->sea_adj.push_back(b);
    pb->adj.push_back(a);
    pb->sea_adj.push_back(a);
}

// The derived naval-control table phase_naval writes: (country, share) ascending.
void set_naval_control(World& w, RegionId region,
                       std::vector<std::pair<CountryId, double>> shares) {
    std::sort(shares.begin(), shares.end(),
              [](const std::pair<CountryId, double>& a, const std::pair<CountryId, double>& b) {
                  return a.first.v < b.first.v;
              });
    w.regions.try_get(region)->naval_control = std::move(shares);
}

EquipmentId add_convoy_equipment(Game& g) {
    EquipmentDef e;
    e.key = "convoy_1";
    e.name = "Convoy I";
    e.category = EquipmentCategory::Convoy;
    e.max_strength = 40.0;
    e.build_cost = 8.0;
    const EquipmentId id(static_cast<uint32_t>(g.content.equipment.size()));
    e.id = id;
    g.content.equipment.push_back(e);
    g.content.equipment_by_key[e.key] = id;
    return id;
}

void set_convoy_stock(World& w, CountryId c, EquipmentId convoy, double count) {
    Country* country = w.countries.try_get(c);
    if (country->equipment_stockpile.size() <= convoy.v) {
        country->equipment_stockpile.resize(convoy.v + 1, 0.0);
    }
    country->equipment_stockpile[convoy.v] = count;
}

double convoy_stock(const World& w, CountryId c, EquipmentId convoy) {
    const Country* country = w.country(c);
    if (country == nullptr || convoy.v >= country->equipment_stockpile.size()) return 0.0;
    return country->equipment_stockpile[convoy.v];
}

ShipId add_ship(World& w, CountryId c, ProvinceId port, RegionId sea) {
    Ship s;
    s.country = c;
    s.port = port;
    s.sea_region = sea;
    s.at_sea = true;
    s.strength = 1.0;
    const ShipId id = w.ships.create(s);
    w.ships.try_get(id)->id = id;
    return id;
}

TaskForceId add_task_force(World& w, CountryId c, RegionId sea, NavalMission mission,
                           std::vector<ShipId> ships) {
    TaskForce tf;
    tf.country = c;
    tf.sea_region = sea;
    tf.mission = mission;
    tf.at_sea = true;
    tf.ships = std::move(ships);
    const TaskForceId id = w.task_forces.create(tf);
    w.task_forces.try_get(id)->id = id;
    for (ShipId s : w.task_forces.try_get(id)->ships) w.ships.try_get(s)->task_force = id;
    return id;
}

// Two land masses joined only by sea: a home province with a port, an overseas
// province with a port, and one sea-zone hop between them. Nothing else links them, so
// the overseas province is unreachable by land.
struct OverseasWorld {
    Game game;
    CountryId alpha;
    CountryId beta;
    ProvinceId capital;
    ProvinceId home_port;
    ProvinceId overseas_port;
    ProvinceId overseas_land;
    RegionId home_sea;
    RegionId far_sea;
    EquipmentId convoy;
};

OverseasWorld make_overseas_world() {
    OverseasWorld o;
    Game& g = o.game;
    World& w = g.world;
    const RegionId land_region = w.regions.create();
    const RegionId home_sea = make_sea_region(w, "home_sea");
    const RegionId far_sea = make_sea_region(w, "far_sea");
    o.home_sea = home_sea;
    o.far_sea = far_sea;

    const StateId home = make_state(w, "home", land_region);
    const StateId colony = make_state(w, "colony", land_region);
    o.alpha = make_country(w, "AAA");
    o.beta = make_country(w, "BBB");

    o.capital = make_province(w, "capital", home, land_region);
    o.home_port = make_province(w, "home_port", home, land_region);
    link(w, o.capital, o.home_port);
    give_state(w, home, o.alpha);
    w.provinces.try_get(o.capital)->is_capital = true;
    w.countries.try_get(o.alpha)->capital = home;

    o.overseas_port = make_province(w, "overseas_port", colony, land_region);
    o.overseas_land = make_province(w, "overseas_land", colony, land_region);
    link(w, o.overseas_port, o.overseas_land);
    give_state(w, colony, o.alpha);

    w.provinces.try_get(o.home_port)->naval_base = 2;
    w.provinces.try_get(o.overseas_port)->naval_base = 2;
    const ProvinceId sea1 = make_sea(w, "sea1", home_sea);
    const ProvinceId sea2 = make_sea(w, "sea2", far_sea);
    link_coast(w, o.home_port, sea1);
    link_coast(w, o.overseas_port, sea2);
    link_sea_zones(w, sea1, sea2);

    set_naval_control(w, home_sea, {{o.alpha, 0.9}});
    set_naval_control(w, far_sea, {{o.alpha, 0.9}});
    o.convoy = add_convoy_equipment(g);
    set_convoy_stock(w, o.alpha, o.convoy, 50.0);
    return o;
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

// A port is a supply source like a hub, and a port the land network cannot reach is
// fed over sea: this is what supplies an overseas province cut off by water.
HOI_TEST(supply_port_supplies_overseas_province) {
    OverseasWorld o = make_overseas_world();

    // No friendly control in the far sea zone: the overseas port delivers nothing and
    // the colony, reachable only by sea, is unsupplied.
    set_naval_control(o.game.world, o.far_sea, {});
    CHECK_NEAR(explain_supply_route(o.game, o.alpha, o.overseas_land, nullptr, nullptr), 0.0,
               1e-12);

    // Friendly control restores the route: home port (naval base 2 -> capacity 20)
    // feeds the overseas port one sea-zone hop away, and one land hop reaches the
    // colony. 20 / 1.15 (sea) / 1.1 (land).
    set_naval_control(o.game.world, o.far_sea, {{o.alpha, 0.9}});
    const double expected = 20.0 / 1.15 / 1.1;
    CHECK_NEAR(explain_supply_route(o.game, o.alpha, o.overseas_land, nullptr, nullptr), expected,
               1e-9);
    // A port adds capacity to its own province exactly like a hub.
    CHECK_NEAR(explain_supply_route(o.game, o.alpha, o.home_port, nullptr, nullptr), 20.0, 1e-9);

    phase_supply(o.game);
    const Province* colony = o.game.world.provinces.try_get(o.overseas_land);
    CHECK_EQ(colony->supply_source, o.overseas_port);
    CHECK_NEAR(colony->supply_level, 1.0, 1e-12);

    // The auditor stays clean with ports, sea zones and convoys in the world.
    const std::vector<std::string> problems = check_invariants(o.game);
    CHECK(problems.empty());
}

// Enemy naval control in a port's zone scales the port's capacity down, and full
// enemy control starves it; a hostile convoy-raiding force in the zone does the same.
HOI_TEST(supply_blockade_and_raiders_starve_overseas_port) {
    OverseasWorld o = make_overseas_world();
    Game& g = o.game;
    CHECK(declare_war(g, o.alpha, o.beta, {}).valid());

    // No enemy: the route delivers in full.
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr),
               20.0 / 1.15 / 1.1, 1e-9);

    // 10% enemy control: capacity scaled by (1 - 0.1).
    set_naval_control(g.world, o.far_sea, {{o.alpha, 0.9}, {o.beta, 0.1}});
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr),
               20.0 * 0.9 / 1.15 / 1.1, 1e-9);

    // Heavy enemy control: nearly everything is lost.
    set_naval_control(g.world, o.far_sea, {{o.alpha, 0.1}, {o.beta, 0.9}});
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr),
               20.0 * 0.1 / 1.15 / 1.1, 1e-9);

    // No friendly control at all: the port delivers nothing.
    set_naval_control(g.world, o.far_sea, {{o.beta, 1.0}});
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr), 0.0, 1e-12);

    // Friendly control restored, but a raiding force above the threshold cuts it.
    set_naval_control(g.world, o.far_sea, {{o.alpha, 1.0}});
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr),
               20.0 / 1.15 / 1.1, 1e-9);
    const ShipId raider_a = add_ship(g.world, o.beta, o.overseas_port, o.far_sea);
    const ShipId raider_b = add_ship(g.world, o.beta, o.overseas_port, o.far_sea);
    add_task_force(g.world, o.beta, o.far_sea, NavalMission::ConvoyRaid, {raider_a, raider_b});
    CHECK_NEAR(explain_supply_route(g, o.alpha, o.overseas_land, nullptr, nullptr), 0.0, 1e-12);
}

// Overseas supply is drawn from the convoy stock: it costs convoys while it runs, and
// a country with none loses the overseas network but keeps its home network.
HOI_TEST(supply_overseas_supply_needs_convoys) {
    OverseasWorld o = make_overseas_world();

    phase_supply(o.game);
    const Province* colony = o.game.world.provinces.try_get(o.overseas_land);
    CHECK_EQ(colony->supply_source, o.overseas_port);
    CHECK_NEAR(colony->supply_level, 1.0, 1e-12);
    // The sea route drew convoy_1: throughput 20/1.15, use 0.02 per capacity-hour.
    CHECK_NEAR(convoy_stock(o.game.world, o.alpha, o.convoy), 50.0 - 20.0 / 1.15 * 0.02, 1e-9);

    // No convoys left: the overseas province starves, the home port still flows.
    set_convoy_stock(o.game.world, o.alpha, o.convoy, 0.0);
    phase_supply(o.game);
    colony = o.game.world.provinces.try_get(o.overseas_land);
    CHECK_NEAR(colony->supply_level, 0.0, 1e-12);
    CHECK(!colony->supply_source.valid());
    CHECK_NEAR(o.game.world.provinces.try_get(o.home_port)->supply_level, 1.0, 1e-12);
    CHECK_NEAR(o.game.world.provinces.try_get(o.capital)->supply_level, 1.0, 1e-12);
    CHECK_NEAR(explain_supply_route(o.game, o.alpha, o.overseas_land, nullptr, nullptr), 0.0,
               1e-12);
}

// Determinism: an identical setup computes an identical network, so the whole world
// hashes the same across two runs.
HOI_TEST(supply_overseas_is_deterministic) {
    auto run = []() {
        OverseasWorld o = make_overseas_world();
        phase_supply(o.game);
        phase_supply(o.game);
        return world_hash(o.game);
    };
    CHECK_EQ(run(), run());
}
