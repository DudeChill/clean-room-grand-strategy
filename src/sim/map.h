#pragma once
// Map graph algorithms. The province adjacency graph is authoritative for
// movement, supply, front detection and encirclement (spec section 31).

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct PathRequest {
    CountryId country;
    bool allow_hostile = false;   // path through provinces at war with `country`
    bool require_controlled = true;  // only provinces controlled by `country` or allies
    bool for_supply = false;      // supply paths may cross allied territory
};

// Shortest path over the land graph in province hops, terrain- and
// infrastructure-weighted. Returns provinces after `from`, ending with `to`.
// Empty when unreachable.
std::vector<ProvinceId> find_path(const World& w, ProvinceId from, ProvinceId to,
                                  const PathRequest& req);

// Terrain and infrastructure multiplier applied to movement time.
double terrain_move_cost(const Province& p, bool river_crossing);

// True when the two provinces share an edge in the land graph.
bool adjacent(const World& w, ProvinceId a, ProvinceId b);

// Distance in hops from `source` over land, honouring `req` legality. Unreachable
// provinces are left at -1.
void compute_hop_distances(const World& w, ProvinceId source, const PathRequest& req,
                           std::vector<int32_t>* out);

// Encirclement: a province is isolated when no land path exists to any supply
// source of its controller that stays inside controlled territory.
bool province_is_encircled(const World& w, ProvinceId p);

// Front-line provinces for `country` against hostile neighbours: controlled
// provinces that touch a province controlled by a country it is at war with.
std::vector<ProvinceId> compute_front_line(const World& w, CountryId country);

// Provinces controlled by `country` that are adjacent to `front` and can host
// reserves. Sorted by province id for deterministic assignment.
std::vector<ProvinceId> front_reserve_provinces(const World& w, CountryId country,
                                                const std::vector<ProvinceId>& front);

}  // namespace hoi
