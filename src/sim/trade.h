#pragma once
// International trade (spec section 46): resources move between countries along land
// routes or sea routes, cost civilian industry, consume convoys and can be blockaded.
//
// Trade is a real flow, not a modifier: what a route delivers lands in the importer's
// resource pool and is consumed by its production lines in the ordinary industry step.

#include <vector>

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// The TradeRoute state itself lives in sim/world.h (it is world state).

// Surplus (positive) or deficit (negative) of one resource for a country, in units per
// day, after domestic production and consumption.
double resource_balance(const Game& g, CountryId country, Resource resource);

// True when a route between the two countries is possible at all: not at war, and
// either land-connected through controlled or allied territory, or connected by sea
// with both sides holding a usable port. `out_sea` and `out_region` report the chosen
// route.
bool trade_route_possible(const Game& g, CountryId importer, CountryId exporter, bool* out_sea,
                          RegionId* out_region);

// Convoy units one unit of a sea-borne resource costs per day.
double trade_convoy_use_per_unit(const Game& g);

// Civilian factories the importer ties up per unit per day (scaled by its trade law).
double trade_factory_cost_per_unit(const Game& g, CountryId importer);

// Creates or updates a route (importer side). Returns false when no route is possible,
// the amount is not positive, or the importer cannot pay the factory cost.
bool trade_start(Game& g, CountryId importer, CountryId exporter, Resource resource,
                 double amount_per_day);

// Removes every route between the two countries for that resource.
bool trade_cancel(Game& g, CountryId importer, CountryId exporter, Resource resource);

// Daily trade step: recompute balances, keep existing routes feasible (blockade, war,
// convoys, factories), and let the AI top up deficits from available surplus. Called by
// phase_trade, once per day.
void phase_trade(Game& g);

// AI: open or close routes to cover the country's resource deficits, scored by the
// deficit, the factory cost and the political situation; records reasons.
void ai_trade_layer(Game& g, Country& c);

}  // namespace hoi
