// Map graph algorithm tests. Worlds are built in memory on a small grid so the
// tests never depend on the generator or on the committed map data.

#include <algorithm>
#include <vector>

#include "sim/map.h"
#include "sim/world.h"
#include "test.h"

namespace {

using namespace hoi;

struct Grid {
    World world;
    int w = 0;
    int h = 0;
    std::vector<ProvinceId> at;

    ProvinceId id(int x, int y) const { return at[static_cast<size_t>(y) * w + x]; }
};

// Builds a fully connected w x h grid of land provinces. Province creation order
// is row-major, so a province's index is y*w + x.
Grid make_grid(int w, int h, Terrain terrain = Terrain::Plains, int infra = 3) {
    Grid g;
    g.w = w;
    g.h = h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Province p;
            p.terrain = terrain;
            p.infrastructure = infra;
            p.population = 1000.0;
            const ProvinceId id = g.world.provinces.create(p);
            g.world.provinces[id].id = id;
            g.at.push_back(id);
        }
    }
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            Province& p = g.world.provinces[g.id(x, y)];
            for (int d = 0; d < 4; ++d) {
                const int nx = x + dx[d];
                const int ny = y + dy[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                p.adj.push_back(g.id(nx, ny));
            }
            std::sort(p.adj.begin(), p.adj.end());
        }
    }
    return g;
}

PathRequest unrestricted(CountryId c) {
    PathRequest req;
    req.country = c;
    req.require_controlled = false;
    req.allow_hostile = true;
    return req;
}

WarId make_war(World& w, CountryId attacker, CountryId defender) {
    War war;
    war.active = true;
    war.aggressor = attacker;
    war.attackers.push_back(WarParticipant{attacker, 0.0, 0.0, 0.0});
    war.defenders.push_back(WarParticipant{defender, 0.0, 0.0, 0.0});
    return w.wars.create(war);
}

}  // namespace

HOI_TEST(map_path_plain_grid) {
    Grid g = make_grid(5, 5);
    const CountryId none;
    const std::vector<ProvinceId> path =
        find_path(g.world, g.id(0, 0), g.id(4, 0), unrestricted(none));
    CHECK_EQ(path.size(), static_cast<size_t>(4));
    CHECK_EQ(path.front(), g.id(1, 0));
    CHECK_EQ(path.back(), g.id(4, 0));
    for (ProvinceId p : path) CHECK_EQ(p, g.id(static_cast<int>(p.v) % 5, 0));
    // Already at the destination: nothing to traverse.
    CHECK(find_path(g.world, g.id(2, 2), g.id(2, 2), unrestricted(none)).empty());
}

HOI_TEST(map_path_around_blocked_province) {
    Grid g = make_grid(5, 5);
    // Wall column 1 except its bottom row: the only crossing is at the bottom.
    for (int y = 0; y < 4; ++y) g.world.provinces[g.id(1, y)].is_sea = true;

    const CountryId none;
    const std::vector<ProvinceId> path =
        find_path(g.world, g.id(0, 0), g.id(4, 0), unrestricted(none));
    CHECK(!path.empty());
    CHECK_EQ(path.back(), g.id(4, 0));
    for (ProvinceId p : path) CHECK(!g.world.provinces[p].is_sea);
    // Down column 0 (4), across the gap (1), along row 4 (3), up column 4 (4).
    CHECK_EQ(path.size(), static_cast<size_t>(12));
    // A sea province is never a destination.
    CHECK(find_path(g.world, g.id(0, 0), g.id(1, 0), unrestricted(none)).empty());
    // Complete wall: unreachable.
    g.world.provinces[g.id(1, 4)].is_sea = true;
    CHECK(find_path(g.world, g.id(0, 0), g.id(4, 4), unrestricted(none)).empty());
}

HOI_TEST(map_terrain_cost_ordering) {
    Province plains;
    Province forest;
    forest.terrain = Terrain::Forest;
    Province hills;
    hills.terrain = Terrain::Hills;
    Province mountain;
    mountain.terrain = Terrain::Mountain;
    Province marsh;
    marsh.terrain = Terrain::Marsh;
    Province desert;
    desert.terrain = Terrain::Desert;
    Province jungle;
    jungle.terrain = Terrain::Jungle;
    std::vector<Province*> all{&plains, &forest, &hills, &mountain, &marsh, &desert, &jungle};
    for (Province* p : all) p->infrastructure = 0;

    // Table order from ARCHITECTURE 5.4: plains 1.0, desert 1.1, forest 1.5,
    // hills 1.6, marsh 2.0, jungle 2.5, mountain 3.0.
    CHECK_LT(terrain_move_cost(plains, false), terrain_move_cost(desert, false));
    CHECK_LT(terrain_move_cost(desert, false), terrain_move_cost(forest, false));
    CHECK_LT(terrain_move_cost(forest, false), terrain_move_cost(hills, false));
    CHECK_LT(terrain_move_cost(hills, false), terrain_move_cost(marsh, false));
    CHECK_LT(terrain_move_cost(marsh, false), terrain_move_cost(jungle, false));
    CHECK_LT(terrain_move_cost(jungle, false), terrain_move_cost(mountain, false));
    CHECK_NEAR(terrain_move_cost(mountain, false), 3.0, 1e-12);
    CHECK_GT(terrain_move_cost(plains, true), terrain_move_cost(plains, false));
    Province paved = plains;
    paved.infrastructure = 6;
    CHECK_LT(terrain_move_cost(paved, false), terrain_move_cost(plains, false));

    // Shortest paths weigh terrain: a 5x2 strip with two mountains in the direct
    // row detours through the other row (6 plains vs 1+3+3+1).
    Grid g = make_grid(5, 2, Terrain::Plains, 0);
    g.world.provinces[g.id(1, 0)].terrain = Terrain::Mountain;
    g.world.provinces[g.id(2, 0)].terrain = Terrain::Mountain;
    const CountryId none;
    const std::vector<ProvinceId> path =
        find_path(g.world, g.id(0, 0), g.id(4, 0), unrestricted(none));
    CHECK_EQ(path.size(), static_cast<size_t>(6));
    CHECK_EQ(path.front(), g.id(0, 1));
    CHECK_EQ(path.back(), g.id(4, 0));
    for (ProvinceId p : path) CHECK(g.world.provinces[p].terrain != Terrain::Mountain);
}

HOI_TEST(map_hop_distances) {
    Grid g = make_grid(5, 5);
    const CountryId none;
    std::vector<int32_t> dist;
    compute_hop_distances(g.world, g.id(0, 0), unrestricted(none), &dist);
    CHECK_EQ(dist[g.id(0, 0).v], 0);
    CHECK_EQ(dist[g.id(4, 0).v], 4);
    CHECK_EQ(dist[g.id(0, 4).v], 4);
    CHECK_EQ(dist[g.id(4, 4).v], 8);
    CHECK_EQ(dist[g.id(2, 2).v], 4);

    // A wall cuts the far side off.
    for (int y = 0; y < 5; ++y) g.world.provinces[g.id(2, y)].is_sea = true;
    compute_hop_distances(g.world, g.id(0, 0), unrestricted(none), &dist);
    CHECK_EQ(dist[g.id(1, 1).v], 2);
    CHECK_EQ(dist[g.id(3, 3).v], -1);
}

HOI_TEST(map_encirclement_detection) {
    Grid g = make_grid(3, 3);
    const CountryId a = g.world.countries.create(Country{});
    const CountryId b = g.world.countries.create(Country{});

    // B holds only the centre, and the centre is B's supply hub; A holds the ring.
    for (ProvinceId pid : g.at) {
        Province& p = g.world.provinces[pid];
        p.owner = a;
        p.controller = a;
    }
    Province& centre = g.world.provinces[g.id(1, 1)];
    centre.owner = b;
    centre.controller = b;
    centre.supply_hub = true;

    // A source is always in supply with itself...
    CHECK(!province_is_encircled(g.world, g.id(1, 1)));
    // ...while A owns no source at all, so every A province is cut off.
    CHECK(province_is_encircled(g.world, g.id(0, 0)));

    // B holds an adjacent province that is B's hub: reachable, so not encircled.
    centre.supply_hub = false;
    Province& neighbour = g.world.provinces[g.id(0, 1)];
    neighbour.owner = b;
    neighbour.controller = b;
    neighbour.supply_hub = true;
    CHECK(!province_is_encircled(g.world, g.id(1, 1)));

    // At war with the ring owner, B cannot trace supply through A's provinces, so
    // a hub on the far side of the ring does not reach the centre.
    Province& far_hub = g.world.provinces[g.id(2, 2)];
    far_hub.owner = b;
    far_hub.controller = b;
    far_hub.supply_hub = true;
    neighbour.controller = a;
    neighbour.supply_hub = false;
    make_war(g.world, b, a);
    CHECK(province_is_encircled(g.world, g.id(1, 1)));
    CHECK(!province_is_encircled(g.world, g.id(2, 2)));
}

HOI_TEST(map_front_line_and_reserves) {
    // Columns: 0 = A, 1 = B, 2 = A (front), 3 = A (behind the front).
    Grid g = make_grid(4, 3);
    const CountryId a = g.world.countries.create(Country{});
    const CountryId b = g.world.countries.create(Country{});
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            Province& p = g.world.provinces[g.id(x, y)];
            p.controller = (x == 1) ? b : a;
            p.owner = p.controller;
        }
    }
    // No war yet: no front line even with hostile neighbours.
    CHECK(compute_front_line(g.world, a).empty());

    make_war(g.world, a, b);
    const std::vector<ProvinceId> front_a = compute_front_line(g.world, a);
    const std::vector<ProvinceId> expected_a{g.id(0, 0), g.id(2, 0), g.id(0, 1),
                                             g.id(2, 1), g.id(0, 2), g.id(2, 2)};
    CHECK_EQ(front_a.size(), expected_a.size());
    for (size_t i = 0; i < front_a.size(); ++i) CHECK_EQ(front_a[i], expected_a[i]);
    CHECK(std::is_sorted(front_a.begin(), front_a.end()));

    const std::vector<ProvinceId> front_b = compute_front_line(g.world, b);
    const std::vector<ProvinceId> expected_b{g.id(1, 0), g.id(1, 1), g.id(1, 2)};
    CHECK_EQ(front_b.size(), expected_b.size());
    for (size_t i = 0; i < front_b.size(); ++i) CHECK_EQ(front_b[i], expected_b[i]);

    const std::vector<ProvinceId> reserves = front_reserve_provinces(g.world, a, front_a);
    const std::vector<ProvinceId> expected_r{g.id(3, 0), g.id(3, 1), g.id(3, 2)};
    CHECK_EQ(reserves.size(), expected_r.size());
    for (size_t i = 0; i < reserves.size(); ++i) CHECK_EQ(reserves[i], expected_r[i]);
    // Front provinces are never listed as reserves for themselves.
    for (ProvinceId p : reserves) {
        CHECK(std::find(front_a.begin(), front_a.end(), p) == front_a.end());
    }
    CHECK(front_reserve_provinces(g.world, a, {}).empty());
}
