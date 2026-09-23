#pragma once
// Logistics: a real supply network (spec sections 40-41).
//
// Supply is a flow over the province graph from sources (capital, supply hubs,
// ports) through controlled territory, limited by rail/infrastructure capacity and
// distance. Divisions read their supply level from the province they stand in.

#include <vector>

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

struct SupplySource {
    ProvinceId province;
    CountryId country;
    double capacity = 0.0;
    ProvinceId hub;  // INVALID for the capital source
};

struct SupplyRouteStep {
    ProvinceId province;
    double capacity;
};

// Sources usable by `country`: controlled capital plus controlled supply hubs.
std::vector<SupplySource> supply_sources(const World& w, CountryId country);

// Recomputes province supply levels for every country, then sets each division's
// supply and fuel level from its province. Deterministic: countries processed in
// ascending id order, provinces in ascending id order.
void phase_supply(Game& g);

// Full route explanation for the supply debugger (spec section 41): fills
// `out_path` with the provinces supply travels through and returns the delivered
// capacity for `province`.
double explain_supply_route(const Game& g, CountryId country, ProvinceId province,
                            std::vector<SupplyRouteStep>* out_path,
                            ProvinceId* bottleneck);

}  // namespace hoi
