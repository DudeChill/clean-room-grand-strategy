#pragma once
// Industry, resources and construction (spec sections 42-46, 168).
//
// All entry points are pure functions of world + content state so they can be unit
// tested without a running simulation; `phase_industry` applies them once per tick.

#include <vector>

#include "core/types.h"
#include "data/content.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Country-level resource accounting for one day, used by production and UI.
struct ResourceBalance {
    double produced[RESOURCE_COUNT] = {};
    double consumed[RESOURCE_COUNT] = {};
    double imported[RESOURCE_COUNT] = {};
    double exported[RESOURCE_COUNT] = {};
    double stock[RESOURCE_COUNT] = {};  // strategic stockpile (fuel handled separately)
};

// Sum of resource yields of states controlled by `country`, scaled by province
// infrastructure. Pure.
void compute_resource_production(const World& w, const Content& c, CountryId country,
                                 double out[RESOURCE_COUNT]);

// Resources one unit of `equipment` requires.
void equipment_resource_cost(const EquipmentDef& def, double out[RESOURCE_COUNT]);

// Factory output of a single line for one hour, before resource shortage.
// = factories * ic_per_factory * efficiency * (1 + factory output modifiers)
double line_hourly_output(const Game& g, const Country& c, const ProductionLine& line);

// Resource satisfaction factor (0..1) for a line given the resources the country
// actually has available after higher-priority lines consumed theirs.
double line_resource_factor(const Game& g, const Country& c, const ProductionLine& line,
                            const double available[RESOURCE_COUNT]);

// Equipment cost in industrial capacity for one unit (with cost modifiers applied).
double equipment_unit_cost(const Game& g, const Country& c, const EquipmentDef& def);

// Advances every production line of every country by one hour: resource allocation
// in line order, efficiency growth/decay, output into stockpile.
void phase_industry(Game& g);

// Civilian industry output per hour available for construction after consumer
// goods and repairs.
double construction_output(const Game& g, const Country& c);

// Total consumer goods factories required by a country.
double consumer_goods_factories(const Game& g, const Country& c);

// Number of factories of each kind in states controlled by `country`.
void count_factories(const World& w, CountryId country, int* civ, int* mil, int* dock);

// Equipment deficit for a country: sum over divisions and training queue of
// missing equipment per model. Used by AI and the reinforcement system.
void compute_equipment_demand(const Game& g, CountryId country,
                              std::vector<double>* demand_by_equipment);

// Attempts to move `count` units of `equipment` from stockpile into `division`
// (strength/manpower follow). Returns the number actually transferred.
double reinforce_division(Game& g, Division& d, EquipmentId equipment, double count);

// True when the country's industry can still be considered functional (used by
// capitulation checks and AI sanity).
bool industry_intact(const World& w, CountryId country);

}  // namespace hoi
