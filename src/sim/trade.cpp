// International trade (spec section 46).
//
// Trade is a real flow, not a modifier. Every day the trade phase recomputes what
// each country produces and what its production lines require, then walks the
// standing routes in the one order that makes the outcome deterministic - the
// (importer, exporter, resource) sort order of World::trade_routes - and moves
// resources from exporter to importer. What a route delivers lands in the
// importer's `resources_imported`, which phase_industry pools with domestic
// extraction when it allocates resources to production lines. Nothing here
// guarantees a flow: war, a lost port, a sunk convoy pool or an exhausted surplus
// all reduce what arrives, and a route that can move nothing is deactivated.
//
// Units, so every term is dimensionally consistent with industry.cpp:
//   * `amount`, `delivered`, `resources_imported` and `resources_exported` are
//     resource units per DAY,
//   * a production line's requirement is `EquipmentDef::resources[r] * factories`
//     per day (industry spreads that daily figure over its 24 hourly steps),
//   * `trade_convoy_use_per_unit` is convoy units per resource unit per day,
//   * `trade_factory_cost_per_unit` is civilian factories tied up per resource
//     unit per day.
//
// Documented rules chosen where the spec left a choice:
//   * Convoys are drawn from the IMPORTER's stockpile: the importer is the one
//     moving the goods and paying the route's costs, so sea trade fails when the
//     importer's merchant marine is gone.
//   * `amount` is the standing request; a route whose feasible delivery is lower
//     scales `amount` down to the feasible level. A route with a feasible level of
//     zero is deactivated (`active = false`) and stays in `trade_routes` for
//     reporting; only a new StartTrade command revives it.
//   * A blockade does not scale `amount` down - it scales the delivery of an
//     otherwise feasible route by `max(0, 1 - enemy_naval_share)` - because enemy
//     naval control is a temporary condition, not a structural ceiling.
//   * `sea_region` is the DESTINATION (importer-side) sea zone of the crossing.

#include "sim/trade.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/math.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/industry.h"
#include "sim/navy.h"

namespace hoi {
namespace {

// Law kind index used by SetTradePolicy (matches commands.cpp); the trade law
// scales how many civilian factories the importer ties up per resource unit.
constexpr int kTradeLawKind = 2;

// Comparisons never treat a sub-unit trickle as trade.
constexpr double kTradeEps = 1e-9;
constexpr double kTradeMinAmount = 1e-6;

// AI scoring weights. Like every other layer, the trade planner keeps its weights
// named and file-local: the debugging story is the AiReason factors, not these
// numbers.
constexpr double kTradeAiDeficitWeight = 20.0;   // per unit of deficit covered
constexpr double kTradeAiFactoryWeight = 150.0;  // per civilian factory tied up
constexpr double kTradeAiSeaPenalty = 5.0;       // sea routes need convoys
constexpr double kTradeAiWarPenalty = 10.0;      // an exporter already at war is risk
constexpr double kTradeAiMinScore = 1.0;         // below this a route is not worth it
constexpr double kTradeAiMinDeficit = 0.5;       // ignore trivial deficits
constexpr size_t kTradeMaxReasons = 16;          // mirrors ai.cpp's reason-log cap

// ------------------------------------------------------------- surplus math --

// Daily resource requirement of the country's production lines: each assigned
// factory draws `EquipmentDef::resources[r]` per day, exactly what
// `line_resource_factor` charges per hour scaled back up to a day.
void daily_resource_requirement(const Game& g, CountryId country, double out[RESOURCE_COUNT]) {
    for (int r = 0; r < RESOURCE_COUNT; ++r) out[r] = 0.0;
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return;
    for (const ProductionLine& line : c->lines) {
        if (line.factories <= 0) continue;
        const EquipmentDef* def = g.content.equipment_def(line.equipment);
        if (def == nullptr) continue;
        double price[RESOURCE_COUNT];
        equipment_resource_cost(*def, price);
        const double factories = static_cast<double>(line.factories);
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            if (price[r] > 0.0) out[r] += price[r] * factories;
        }
    }
    for (int r = 0; r < RESOURCE_COUNT; ++r) out[r] = finite_or(out[r], 0.0);
}

// Production minus requirement for one country, in units per day. Positive is a
// surplus available to export, negative is a deficit the country must import.
void country_net(const Game& g, CountryId country, double out[RESOURCE_COUNT]) {
    double produced[RESOURCE_COUNT];
    double required[RESOURCE_COUNT];
    compute_resource_production(g.world, g.content, country, produced);
    daily_resource_requirement(g, country, required);
    for (int r = 0; r < RESOURCE_COUNT; ++r) out[r] = finite_or(produced[r] - required[r], 0.0);
}

// --------------------------------------------------------------- convoys -----

// Convoy-category units in the country's stockpile. One category definition, the
// same one naval raiding and the invasion system draw on.
double convoy_stock(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr) return 0.0;
    double total = 0.0;
    const size_t n = std::min(c->equipment_stockpile.size(), g.content.equipment.size());
    for (size_t i = 0; i < n; ++i) {
        const EquipmentDef* def = g.content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
        if (def == nullptr || def->category != EquipmentCategory::Convoy) continue;
        const double have = c->equipment_stockpile[i];
        if (have > 0.0) total += have;
    }
    return total;
}

// Spends `want` convoy units out of the country's stockpile, taking from the
// lowest-id convoy model first so two identical runs spend identically. Returns
// the amount actually taken.
double take_convoys(Game& g, CountryId country, double want) {
    Country* c = g.world.country(country);
    if (c == nullptr || !(want > 0.0)) return 0.0;
    double taken = 0.0;
    const size_t n = std::min(c->equipment_stockpile.size(), g.content.equipment.size());
    for (size_t i = 0; i < n && taken + kTradeEps < want; ++i) {
        const EquipmentDef* def = g.content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
        if (def == nullptr || def->category != EquipmentCategory::Convoy) continue;
        double& stock = c->equipment_stockpile[i];
        if (!(stock > 0.0)) continue;
        const double take = std::min(stock, want - taken);
        stock -= take;
        if (stock < 0.0) stock = 0.0;
        taken += take;
    }
    return taken;
}

// ------------------------------------------------------------- route choice --

// The province a country's trade network starts from: the flagged capital province
// of its capital state, else the lowest-id land province it controls, else none.
ProvinceId trade_anchor(const World& w, CountryId country) {
    const Country* c = w.country(country);
    if (c == nullptr) return ProvinceId{};
    ProvinceId best;
    if (const State* s = w.state(c->capital)) {
        for (ProvinceId p : s->provinces) {
            const Province* pr = w.province(p);
            if (pr == nullptr || pr->is_sea) continue;
            if (pr->is_capital) return p;
            if (!best.valid() || p < best) best = p;
        }
    }
    if (best.valid()) return best;
    w.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (best.valid() || p.is_sea) return;
        if (p.controller != country) return;
        best = pid;
    });
    return best;
}

// True when a land path exists through provinces controlled by (or friendly to)
// the importer, the exporter, or a co-belligerent of either. Breadth-first over
// the land graph, so the first hit is the shortest and the answer is deterministic.
bool land_route_exists(const Game& g, CountryId importer, CountryId exporter) {
    const World& w = g.world;
    const ProvinceId start = trade_anchor(w, importer);
    if (!start.valid()) return false;
    const size_t n = w.provinces.capacity();
    if (start.v >= n) return false;
    std::vector<uint8_t> seen(n, 0);
    std::vector<ProvinceId> queue;
    queue.reserve(64);
    seen[start.v] = 1;
    queue.push_back(start);
    for (size_t head = 0; head < queue.size(); ++head) {
        const ProvinceId cur = queue[head];
        if (cur != start && w.has_access(exporter, cur)) return true;
        const Province* p = w.province(cur);
        if (p == nullptr) continue;
        for (ProvinceId next : p->adj) {
            if (!next.valid() || next.v >= n || seen[next.v] != 0) continue;
            const Province* np = w.province(next);
            if (np == nullptr || np->is_sea) continue;
            if (!w.has_access(importer, next) && !w.has_access(exporter, next)) continue;
            seen[next.v] = 1;
            queue.push_back(next);
        }
    }
    return false;
}

// Lowest-id usable port of a country (deterministic tie-break) and the sea zone it
// opens onto.
bool find_port(const Game& g, CountryId country, RegionId* out_zone) {
    ProvinceId best;
    RegionId best_zone;
    g.world.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (best.valid() || p.is_sea) return;
        if (!is_usable_port(g, country, pid)) return;
        const RegionId zone = adjacent_sea_region(g, pid);
        if (!zone.valid()) return;
        best = pid;
        best_zone = zone;
    });
    if (!best.valid()) return false;
    if (out_zone != nullptr) *out_zone = best_zone;
    return true;
}

// A sea route needs a usable port on both ends and a sea path between the zones.
// `out_region` reports the destination (importer-side) zone.
bool sea_route_exists(const Game& g, CountryId importer, CountryId exporter, RegionId* out_region) {
    RegionId import_zone;
    RegionId export_zone;
    if (!find_port(g, importer, &import_zone)) return false;
    if (!find_port(g, exporter, &export_zone)) return false;
    if (sea_region_distance(g, import_zone, export_zone) < 0) return false;
    if (out_region != nullptr) *out_region = import_zone;
    return true;
}

// Land first, sea second; war and surplus are the caller's business.
bool route_geometry(const Game& g, CountryId importer, CountryId exporter, bool* out_sea,
                    RegionId* out_region) {
    if (out_sea != nullptr) *out_sea = false;
    if (out_region != nullptr) *out_region = RegionId{};
    if (land_route_exists(g, importer, exporter)) return true;
    RegionId zone;
    if (sea_route_exists(g, importer, exporter, &zone)) {
        if (out_sea != nullptr) *out_sea = true;
        if (out_region != nullptr) *out_region = zone;
        return true;
    }
    return false;
}

// Enemy naval control share in a sea zone, summed over every country at war with
// the importer. The naval phase owns Region::naval_control; this reads it.
double enemy_naval_share(const Game& g, CountryId importer, RegionId region) {
    const Region* rg = g.world.regions.try_get(region);
    if (rg == nullptr) return 0.0;
    double total = 0.0;
    for (const auto& entry : rg->naval_control) {
        if (!entry.first.valid()) continue;
        if (!g.world.at_war(importer, entry.first)) continue;
        if (entry.second > 0.0) total += entry.second;
    }
    return clamp01(total);
}

// Civilian factories already tied up by the importer's standing routes, valued at
// the requested amount (not last day's delivery), so a route's cost is stable.
double committed_trade_factories(const Game& g, CountryId importer, double cost_per_unit) {
    double total = 0.0;
    for (const TradeRoute& r : g.world.trade_routes) {
        if (!r.active || r.importer != importer) continue;
        total += r.amount * cost_per_unit;
    }
    return total;
}

// Keeps World::trade_routes sorted by (importer, exporter, resource). Returns the
// matching route, or nullptr.
TradeRoute* find_route(World& w, CountryId importer, CountryId exporter, Resource resource) {
    for (TradeRoute& r : w.trade_routes) {
        if (r.importer == importer && r.exporter == exporter && r.resource == resource) {
            return &r;
        }
    }
    return nullptr;
}

void log_trade(Game& g, const std::string& text, CountryId country) {
    g.log_event("trade", text, country);
}

std::string resource_name_of(Resource r) { return std::string(resource_name(r)); }

// ---------------------------------------------------------------- AI helper --

// Appends an AiReason to the industry layer's log with the same cap/replace rule
// ai.cpp uses (the trade planner runs inside the industry layer's slot).
void record_trade_reason(Game& g, AiReason reason) {
    AiLayerState& st = g.ai.layer(AiLayer::Industry);
    reason.score = finite_or(reason.score, 0.0);
    for (auto& f : reason.factors) f.second = finite_or(f.second, 0.0);
    if (st.last_reasons.size() < kTradeMaxReasons) {
        st.last_reasons.push_back(std::move(reason));
    } else {
        size_t weakest = 0;
        for (size_t i = 1; i < st.last_reasons.size(); ++i) {
            if (st.last_reasons[i].score < st.last_reasons[weakest].score) weakest = i;
        }
        if (reason.score > st.last_reasons[weakest].score) {
            st.last_reasons[weakest] = std::move(reason);
        }
    }
}

bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

}  // namespace

// ------------------------------------------------------------------ queries --

double resource_balance(const Game& g, CountryId country, Resource resource) {
    const int ri = static_cast<int>(resource);
    if (ri < 0 || ri >= RESOURCE_COUNT) return 0.0;
    double net[RESOURCE_COUNT];
    country_net(g, country, net);
    return finite_or(net[ri], 0.0);
}

double trade_convoy_use_per_unit(const Game& g) {
    const double per_unit = g.content.constants.trade_convoy_use_per_unit;
    return std::isfinite(per_unit) && per_unit > 0.0 ? per_unit : 0.0;
}

double trade_factory_cost_per_unit(const Game& g, CountryId importer) {
    const double base = g.content.constants.trade_factory_cost_per_unit;
    if (!std::isfinite(base) || base <= 0.0) return 0.0;
    const Country* c = g.world.country(importer);
    if (c == nullptr) return base;
    int level = 0;
    if (kTradeLawKind < static_cast<int>(c->law_levels.size())) {
        level = static_cast<int>(c->law_levels[kTradeLawKind]);
    }
    const double scale = g.content.constants.trade_factory_cost_law_scale;
    const double factor = clamp(1.0 - (std::isfinite(scale) ? scale : 0.0) * level, 0.1, 1.0);
    return base * factor;
}

bool trade_route_possible(const Game& g, CountryId importer, CountryId exporter, bool* out_sea,
                          RegionId* out_region) {
    if (out_sea != nullptr) *out_sea = false;
    if (out_region != nullptr) *out_region = RegionId{};
    if (!importer.valid() || !exporter.valid() || importer == exporter) return false;
    const Country* imp = g.world.country(importer);
    const Country* exp = g.world.country(exporter);
    if (imp == nullptr || exp == nullptr || !imp->alive || !exp->alive) return false;
    if (g.world.at_war(importer, exporter)) return false;
    // The pair must be able to move something: an exporter with no surplus of any
    // resource cannot feed a route. The per-resource test happens in trade_start.
    bool any_surplus = false;
    double net[RESOURCE_COUNT];
    country_net(g, exporter, net);
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        if (net[r] > kTradeEps) {
            any_surplus = true;
            break;
        }
    }
    if (!any_surplus) return false;
    return route_geometry(g, importer, exporter, out_sea, out_region);
}

// ------------------------------------------------------------------ mutate --

bool trade_start(Game& g, CountryId importer, CountryId exporter, Resource resource,
                 double amount_per_day) {
    World& w = g.world;
    const int ri = static_cast<int>(resource);
    if (ri < 0 || ri >= RESOURCE_COUNT) return false;
    if (!std::isfinite(amount_per_day) || amount_per_day <= 0.0) return false;
    if (!trade_route_possible(g, importer, exporter, nullptr, nullptr)) return false;

    // Clamp the request to what the exporter can actually make available once the
    // routes already claiming its surplus are accounted for.
    const double surplus = resource_balance(g, exporter, resource);
    double claimed = 0.0;
    for (const TradeRoute& r : w.trade_routes) {
        if (r.active && r.exporter == exporter && r.resource == resource) claimed += r.amount;
    }
    const double free = surplus - claimed;
    if (!(free > kTradeEps)) return false;
    double amount = std::min(amount_per_day, free);

    // Clamp to the civilian factories the importer can still tie up.
    const double cost_per_unit = trade_factory_cost_per_unit(g, importer);
    if (cost_per_unit > 0.0) {
        int civ = 0;
        count_factories(w, importer, &civ, nullptr, nullptr);
        if (civ <= 0) return false;
        const double committed = committed_trade_factories(g, importer, cost_per_unit);
        const double affordable = (static_cast<double>(civ) - committed) / cost_per_unit;
        if (!(affordable > kTradeEps)) return false;
        amount = std::min(amount, affordable);
    }
    if (!(amount > kTradeMinAmount)) return false;

    bool sea = false;
    RegionId zone;
    route_geometry(g, importer, exporter, &sea, &zone);

    if (TradeRoute* existing = find_route(w, importer, exporter, resource)) {
        existing->amount = amount;
        existing->active = true;
        existing->sea_route = sea;
        existing->sea_region = sea ? zone : RegionId{};
        log_trade(g, "trade route updated: " + resource_name_of(resource) + " from " +
                         (w.country(exporter) ? w.country(exporter)->name : std::string("?")) +
                         " (" + std::to_string(amount) + "/day, " +
                         (sea ? "sea" : "land") + ")",
                  importer);
        return true;
    }

    TradeRoute route;
    route.importer = importer;
    route.exporter = exporter;
    route.resource = resource;
    route.amount = amount;
    route.delivered = 0.0;
    route.sea_route = sea;
    route.sea_region = sea ? zone : RegionId{};
    route.convoy_use = 0.0;
    route.factory_cost = 0.0;
    route.active = true;

    auto before = [](const TradeRoute& a, const TradeRoute& b) {
        if (a.importer != b.importer) return a.importer < b.importer;
        if (a.exporter != b.exporter) return a.exporter < b.exporter;
        return static_cast<int>(a.resource) < static_cast<int>(b.resource);
    };
    const auto pos = std::lower_bound(w.trade_routes.begin(), w.trade_routes.end(), route, before);
    w.trade_routes.insert(pos, route);
    log_trade(g, "trade route opened: " + resource_name_of(resource) + " from " +
                     (w.country(exporter) ? w.country(exporter)->name : std::string("?")) + " (" +
                     std::to_string(amount) + "/day, " + (sea ? "sea" : "land") + ")",
              importer);
    return true;
}

bool trade_cancel(Game& g, CountryId importer, CountryId exporter, Resource resource) {
    World& w = g.world;
    const int ri = static_cast<int>(resource);
    if (ri < 0 || ri >= RESOURCE_COUNT) return false;
    bool removed = false;
    const std::string name = resource_name_of(resource);
    for (size_t i = w.trade_routes.size(); i-- > 0;) {
        const TradeRoute& r = w.trade_routes[i];
        if (r.importer != importer || r.exporter != exporter || r.resource != resource) continue;
        log_trade(g, "trade route closed: " + name + " from " +
                         (w.country(exporter) ? w.country(exporter)->name : std::string("?")),
                  importer);
        w.trade_routes.erase(w.trade_routes.begin() + static_cast<std::ptrdiff_t>(i));
        removed = true;
    }
    return removed;
}

// -------------------------------------------------------------------- phase --

void phase_trade(Game& g) {
    World& w = g.world;
    // One daily step, aligned with phase_industry's own day boundary so the imports
    // it reads are the ones set here.
    if (w.date.hour != 0) return;

    // (a) Clear the day's reporting totals; every delivery below adds to them.
    w.countries.for_each([&](CountryId, Country& c) {
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            c.resources_imported[r] = 0.0;
            c.resources_exported[r] = 0.0;
        }
    });

    const size_t nc = w.countries.capacity();
    const size_t cells = nc * static_cast<size_t>(RESOURCE_COUNT);
    // Production/requirement tables for every alive country, computed once per day.
    std::vector<double> net(cells, 0.0);
    w.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive) return;
        double row[RESOURCE_COUNT];
        country_net(g, id, row);
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            net[id.v * RESOURCE_COUNT + static_cast<size_t>(r)] = finite_or(row[r], 0.0);
        }
    });
    // Surplus already promised to routes processed earlier in the sorted order.
    std::vector<double> exported(cells, 0.0);
    // Per-importer pools: -1 = not loaded yet. Convoys are physical stockpiles,
    // factories are a daily budget that nothing else consumes (see header note).
    std::vector<double> convoy_pool(nc, -1.0);
    std::vector<double> factory_pool(nc, -1.0);
    const double convoy_per_unit = trade_convoy_use_per_unit(g);

    for (TradeRoute& r : w.trade_routes) {
        const int ri = static_cast<int>(r.resource);
        const size_t cell = r.exporter.v * RESOURCE_COUNT + static_cast<size_t>(ri);
        if (!r.active) {
            r.delivered = 0.0;
            r.convoy_use = 0.0;
            r.factory_cost = 0.0;
            continue;
        }
        Country* imp = w.country(r.importer);
        Country* exp = w.country(r.exporter);
        if (ri < 0 || ri >= RESOURCE_COUNT || imp == nullptr || exp == nullptr || !imp->alive ||
            !exp->alive) {
            r.active = false;
            r.amount = 0.0;
            r.delivered = 0.0;
            r.convoy_use = 0.0;
            r.factory_cost = 0.0;
            log_trade(g, "trade route closed: partner no longer exists", r.importer);
            continue;
        }
        if (w.at_war(r.importer, r.exporter)) {
            r.active = false;
            r.amount = 0.0;
            r.delivered = 0.0;
            r.convoy_use = 0.0;
            r.factory_cost = 0.0;
            log_trade(g, "trade route cut by war with " + exp->name, r.importer);
            continue;
        }

        bool sea = false;
        RegionId zone;
        if (!route_geometry(g, r.importer, r.exporter, &sea, &zone)) {
            r.active = false;
            r.amount = 0.0;
            r.delivered = 0.0;
            r.convoy_use = 0.0;
            r.factory_cost = 0.0;
            log_trade(g, "trade route closed: no land or sea path to " + exp->name, r.importer);
            continue;
        }
        r.sea_route = sea;
        r.sea_region = sea ? zone : RegionId{};

        const double request = r.amount > 0.0 ? r.amount : 0.0;
        // Structural ceilings: exporter surplus still free, importer factories,
        // importer convoys. Delivery is then scaled by the blockade below.
        double limit = request;
        const double surplus_left = net[cell] - exported[cell];
        if (surplus_left < limit) limit = surplus_left > 0.0 ? surplus_left : 0.0;

        const double cost_per_unit = trade_factory_cost_per_unit(g, r.importer);
        double& factories = factory_pool[r.importer.v];
        if (factories < 0.0) {
            int civ = 0;
            count_factories(w, r.importer, &civ, nullptr, nullptr);
            factories = civ > 0 ? static_cast<double>(civ) : 0.0;
        }
        if (cost_per_unit > 0.0) {
            const double affordable = factories / cost_per_unit;
            if (affordable < limit) limit = affordable;
        }
        if (sea && convoy_per_unit > 0.0) {
            double& pool = convoy_pool[r.importer.v];
            if (pool < 0.0) pool = convoy_stock(g, r.importer);
            const double shippable = pool / convoy_per_unit;
            if (shippable < limit) limit = shippable;
        }

        const double feasible = clamp(finite_or(limit, 0.0), 0.0, request);
        if (!(feasible > kTradeEps)) {
            r.active = false;
            r.amount = 0.0;
            r.delivered = 0.0;
            r.convoy_use = 0.0;
            r.factory_cost = 0.0;
            log_trade(g, "trade route closed: no deliverable " + resource_name_of(r.resource) +
                             " from " + exp->name,
                      r.importer);
            continue;
        }

        const double blockade = sea ? clamp01(1.0 - enemy_naval_share(g, r.importer, zone)) : 1.0;
        double delivered = feasible * blockade;

        // Convoys are physically spent for what actually sails.
        double convoys_taken = 0.0;
        if (sea && convoy_per_unit > 0.0 && delivered > 0.0) {
            const double need = delivered * convoy_per_unit;
            convoys_taken = take_convoys(g, r.importer, need);
            if (convoys_taken + kTradeEps < need) {
                delivered = need > 0.0 ? delivered * (convoys_taken / need) : 0.0;
            }
        } else if (sea && convoy_per_unit > 0.0) {
            convoys_taken = 0.0;
        }

        delivered = finite_or(delivered, 0.0);
        if (delivered < 0.0) delivered = 0.0;

        // Commit what was actually delivered.
        r.amount = feasible;
        r.delivered = delivered;
        r.convoy_use = convoys_taken;
        r.factory_cost = delivered * cost_per_unit;
        if (sea && convoy_per_unit > 0.0) convoy_pool[r.importer.v] -= convoys_taken;
        factories -= delivered * cost_per_unit;
        if (factories < 0.0) factories = 0.0;

        exported[cell] += delivered;
        exp->resources_exported[ri] = finite_or(exp->resources_exported[ri] + delivered, 0.0);
        imp->resources_imported[ri] = finite_or(imp->resources_imported[ri] + delivered, 0.0);

        if (sea && blockade < 1.0 - kTradeEps && delivered > 0.0) {
            log_trade(g, "trade route cut by blockade (" + resource_name_of(r.resource) + " from " +
                             exp->name + ": " + std::to_string(delivered) + " of " +
                             std::to_string(feasible) + " delivered)",
                      r.importer);
        }
    }
}

// ----------------------------------------------------------------------- AI --

void ai_trade_layer(Game& g, Country& c) {
    if (!c.alive) return;
    World& w = g.world;
    const int nc = static_cast<int>(w.countries.capacity());
    if (nc <= 0) return;

    // One production/requirement table for every country: the planner scores
    // candidate exporters against their real surplus, and computing it per
    // candidate would repeat a province scan for nothing.
    std::vector<double> net(static_cast<size_t>(nc) * RESOURCE_COUNT, 0.0);
    w.countries.for_each([&](CountryId id, Country& cc) {
        if (!cc.alive) return;
        double row[RESOURCE_COUNT];
        country_net(g, id, row);
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            net[static_cast<size_t>(id.v) * RESOURCE_COUNT + static_cast<size_t>(r)] =
                finite_or(row[r], 0.0);
        }
    });
    auto net_of = [&](CountryId id, int r) -> double {
        if (!id.valid() || static_cast<int>(id.v) >= nc || r < 0 || r >= RESOURCE_COUNT) return 0.0;
        return net[static_cast<size_t>(id.v) * RESOURCE_COUNT + static_cast<size_t>(r)];
    };

    int civ = 0;
    count_factories(w, c.id, &civ, nullptr, nullptr);
    const double factories_budget = civ > 0 ? static_cast<double>(civ) : 0.0;
    const double cost_per_unit = trade_factory_cost_per_unit(g, c.id);

    // ---- pass 1: audit standing routes -------------------------------------
    // Close what cannot pay off: a dead or hostile partner, a deficit that has
    // gone away, or a factory bill the importer can no longer afford.
    double factory_used = 0.0;
    double committed[RESOURCE_COUNT] = {};
    for (const TradeRoute& r : w.trade_routes) {
        if (r.importer != c.id || !r.active) continue;
        const int ri = static_cast<int>(r.resource);
        if (ri < 0 || ri >= RESOURCE_COUNT) continue;
        const Country* exp = w.country(r.exporter);
        const bool partner_ok = exp != nullptr && exp->alive && !w.at_war(c.id, r.exporter);
        const double deficit = -net_of(c.id, ri);
        const double cost = cost_per_unit * r.amount;
        const char* why = nullptr;
        if (!partner_ok) {
            why = "trade_partner_hostile";
        } else if (deficit <= kTradeAiMinDeficit) {
            why = "trade_no_deficit";
        } else if (factory_used + cost > factories_budget + kTradeEps) {
            why = "trade_unaffordable";
        }
        if (why != nullptr) {
            Command cmd;
            cmd.type = CommandType::CancelTrade;
            cmd.country = c.id;
            cmd.target_country = r.exporter;
            cmd.value = ri;
            if (push_if_valid(g, std::move(cmd))) {
                AiReason reason;
                reason.what = why;
                reason.score = deficit;
                reason.factors = {{"deficit", deficit}, {"factory_cost", cost}};
                record_trade_reason(g, std::move(reason));
            }
            continue;
        }
        factory_used += cost;
        committed[ri] += r.amount;
    }

    // ---- pass 2: cover remaining deficits ----------------------------------
    for (int ri = 0; ri < RESOURCE_COUNT; ++ri) {
        const double deficit = -net_of(c.id, ri);
        if (deficit <= kTradeAiMinDeficit) continue;
        const double uncovered = deficit - committed[ri];
        if (uncovered <= kTradeAiMinDeficit) continue;
        if (factory_used >= factories_budget) {
            AiReason reason;
            reason.what = "trade_no_factories";
            reason.score = deficit;
            reason.factors = {{"deficit", deficit}, {"factory_budget", factories_budget}};
            record_trade_reason(g, std::move(reason));
            continue;
        }

        CountryId best;
        double best_score = kTradeAiMinScore;
        double best_surplus = 0.0;
        double best_sea = 0.0;
        double best_war = 0.0;
        w.countries.for_each([&](CountryId eid, Country& e) {
            if (eid == c.id || !e.alive) return;
            if (w.at_war(c.id, eid)) return;
            const double surplus = net_of(eid, ri);
            if (surplus <= kTradeEps) return;
            bool sea = false;
            RegionId zone;
            if (!trade_route_possible(g, c.id, eid, &sea, &zone)) return;
            const double covered = std::min(uncovered, surplus);
            const double cost = cost_per_unit * covered;
            double score = kTradeAiDeficitWeight * covered - kTradeAiFactoryWeight * cost;
            if (sea) score -= kTradeAiSeaPenalty;
            if (e.at_war) score -= kTradeAiWarPenalty;
            if (score <= best_score) return;  // ties go to the lower exporter id
            best = eid;
            best_score = score;
            best_surplus = surplus;
            best_sea = sea ? 1.0 : 0.0;
            best_war = e.at_war ? 1.0 : 0.0;
        });

        if (!best.valid()) {
            AiReason reason;
            reason.what = "trade_no_exporter";
            reason.score = deficit;
            reason.factors = {{"deficit", deficit}, {"committed", committed[ri]}};
            record_trade_reason(g, std::move(reason));
            continue;
        }

        double desired = std::min(uncovered, best_surplus);
        if (cost_per_unit > 0.0) {
            const double room = (factories_budget - factory_used) / cost_per_unit;
            if (room < desired) desired = room;
        }
        if (desired <= kTradeAiMinDeficit) continue;

        bool up_to_date = false;
        for (const TradeRoute& o : w.trade_routes) {
            if (!o.active || o.importer != c.id || o.exporter != best) continue;
            if (static_cast<int>(o.resource) != ri) continue;
            if (o.amount >= desired - kTradeEps) up_to_date = true;
        }
        AiReason reason;
        reason.what = up_to_date ? "trade_route_sufficient" : "trade_open_route";
        reason.score = best_score;
        reason.factors = {{"deficit", deficit},
                          {"surplus", best_surplus},
                          {"factory_cost", desired * cost_per_unit},
                          {"sea", best_sea},
                          {"war", best_war}};
        if (!up_to_date) {
            Command cmd;
            cmd.type = CommandType::StartTrade;
            cmd.country = c.id;
            cmd.target_country = best;
            cmd.value = ri;
            cmd.value_f = desired;
            if (push_if_valid(g, std::move(cmd))) {
                factory_used += desired * cost_per_unit;
                committed[ri] += desired;
            } else {
                reason.what = "trade_command_rejected";
            }
        }
        record_trade_reason(g, std::move(reason));
    }
}

}  // namespace hoi