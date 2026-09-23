// Logistics: a real supply network (spec sections 40-41, ARCHITECTURE 5.6).
//
// Supply is a flow over the province graph: sources (capital, supply hubs) push
// capacity outwards, capacity decays with graph distance, and a province's supply
// level is delivered capacity over local demand. Encirclement needs no special
// case: a province with no controlled path back to a source simply gets zero.

#include "sim/supply.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/diplomacy.h"
#include "sim/map.h"

namespace hoi {
namespace {

// ARCHITECTURE 5.6: capacity(s) = 10 * (1 + 0.15*railway) * (1 + 0.05*infrastructure).
constexpr double kSourceBaseCapacity = 10.0;
// A province with no divisions still consumes a baseline trickle of supply.
constexpr double kBaselineDemand = 0.1;
// Every edge costs at least one "hop" so the range penalty stays a distance measure.
constexpr double kMinEdgeCost = 1.0;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kTieEpsilon = 1e-12;

using Side = std::vector<CountryId>;

bool side_has(const Side& side, CountryId c) {
    return std::binary_search(side.begin(), side.end(), c);
}

// The country plus everyone it shares supply with: co-belligerents in an active
// war and faction partners (see co_belligerents).
Side build_side(const World& w, CountryId country) {
    Side side;
    side.push_back(country);
    for (CountryId c : co_belligerents(w, country)) side.push_back(c);
    std::sort(side.begin(), side.end());
    side.erase(std::unique(side.begin(), side.end()), side.end());
    return side;
}

// Supply may only flow through friendly-held, passable land provinces.
bool traversable(const World& w, ProvinceId id, const Side& side) {
    const Province* p = w.province(id);
    if (p == nullptr || p->is_sea) return false;
    if (!side_has(side, p->controller)) return false;
    const State* s = w.state(p->state);
    if (s != nullptr && s->impassable) return false;
    return true;
}

// Terrain cost of entering `to`, discounted by railway/infrastructure. Development
// buys throughput, so a well-connected province costs less of the range budget.
double supply_edge_cost(const Province& to, const SimConstants& k) {
    double terrain = terrain_move_cost(to, false);
    if (!std::isfinite(terrain) || terrain < kMinEdgeCost) terrain = kMinEdgeCost;
    double bonus = 1.0 + k.supply_rail_bonus_per_level * static_cast<double>(to.railway_level) +
                   k.supply_infrastructure_bonus_per_level * static_cast<double>(to.infrastructure);
    const double cost = terrain / bonus;
    return cost < kMinEdgeCost ? kMinEdgeCost : cost;
}

double source_capacity(const Province& p, const SimConstants& k) {
    return kSourceBaseCapacity *
           (1.0 + k.supply_rail_bonus_per_level * static_cast<double>(p.railway_level)) *
           (1.0 + k.supply_infrastructure_bonus_per_level * static_cast<double>(p.infrastructure));
}

// The country's capital province: the flagged capital when friendly-held, else the
// lowest-id province of the capital state it still holds. INVALID when the capital
// state is entirely lost.
ProvinceId capital_province(const World& w, const Country& c, const Side& side) {
    const State* st = w.state(c.capital);
    if (st == nullptr) return ProvinceId{};
    ProvinceId fallback{};
    for (ProvinceId pid : st->provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr || p->is_sea) continue;
        if (!side_has(side, p->controller)) continue;
        if (p->is_capital) return pid;
        if (!fallback.valid()) fallback = pid;
    }
    return fallback;
}

std::vector<SupplySource> collect_sources(const World& w, CountryId country, const Side& side,
                                          const SimConstants& k) {
    std::vector<SupplySource> out;
    const Country* c = w.country(country);
    if (c == nullptr || !c->alive) return out;

    const ProvinceId cap = capital_province(w, *c, side);
    if (cap.valid()) {
        const Province* p = w.province(cap);
        SupplySource s;
        s.province = cap;
        s.country = country;
        s.capacity = source_capacity(*p, k);
        s.hub = ProvinceId{};
        out.push_back(s);
    }

    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        if (!p.supply_hub || p.is_sea || id == cap) return;
        if (!side_has(side, p.controller)) return;
        const State* st = w.state(p.state);
        if (st != nullptr && st->impassable) return;
        SupplySource s;
        s.province = id;
        s.country = country;
        s.capacity = source_capacity(p, k);
        s.hub = id;
        out.push_back(s);
    });

    std::sort(out.begin(), out.end(),
              [](const SupplySource& a, const SupplySource& b) { return a.province < b.province; });
    return out;
}

// Dijkstra from one source over the friendly-controlled land graph. Costs are
// finite and non-negative, so settle order is fully determined by (distance,
// province id) and never by container layout.
void dijkstra_from(const World& w, const SimConstants& k, const Side& side, ProvinceId source,
                   std::vector<double>* dist, std::vector<ProvinceId>* prev) {
    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());
    dist->assign(n, kInf);
    prev->assign(n, ProvinceId{});
    if (!traversable(w, source, side)) return;

    using Node = std::pair<double, uint32_t>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;
    (*dist)[source.v] = 0.0;
    pq.push(Node{0.0, source.v});

    while (!pq.empty()) {
        const Node top = pq.top();
        pq.pop();
        if (top.first > (*dist)[top.second]) continue;  // stale queue entry
        const Province* p = w.province(ProvinceId(top.second));
        if (p == nullptr) continue;
        for (ProvinceId nb : p->adj) {
            if (!traversable(w, nb, side)) continue;
            const Province* q = w.province(nb);
            const double cost = top.first + supply_edge_cost(*q, k);
            if (cost < (*dist)[nb.v]) {
                (*dist)[nb.v] = cost;
                (*prev)[nb.v] = ProvinceId(top.second);
                pq.push(Node{cost, nb.v});
            }
        }
    }
}

struct SupplySolution {
    std::vector<double> delivered;   // capacity reaching the province
    std::vector<double> distance;    // graph distance to the winning source
    std::vector<ProvinceId> source;  // winning source province (INVALID = unsupplied)
    std::vector<ProvinceId> bottleneck;
    // Per-source Dijkstra results, kept so route/bottleneck maths can re-read the
    // distance of every step on the winning source's own tree.
    std::vector<std::vector<double>> dists;
    std::vector<std::vector<ProvinceId>> prevs;
    std::vector<uint32_t> winner;  // index into `dists`, INVALID_ID when unsupplied
};

// Exact max-over-sources delivery: capacity differs per source, so the nearest
// source is not necessarily the best one. One Dijkstra per source evaluates the
// product graph {provinces} x {sources}, which is the multi-source maximum.
SupplySolution compute_supply(const World& w, const SimConstants& k, const Side& side,
                              const std::vector<SupplySource>& sources) {
    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());
    SupplySolution sol;
    sol.delivered.assign(n, 0.0);
    sol.distance.assign(n, kInf);
    sol.source.assign(n, ProvinceId{});
    sol.bottleneck.assign(n, ProvinceId{});
    sol.dists.resize(sources.size());
    sol.prevs.resize(sources.size());
    sol.winner.assign(n, INVALID_ID);
    const double penalty = k.supply_range_penalty;

    for (size_t si = 0; si < sources.size(); ++si) {
        dijkstra_from(w, k, side, sources[si].province, &sol.dists[si], &sol.prevs[si]);
        const double capacity = sources[si].capacity;
        for (uint32_t v = 0; v < n; ++v) {
            if (!std::isfinite(sol.dists[si][v])) continue;
            const double delivered = capacity / (1.0 + penalty * sol.dists[si][v]);
            const bool better = delivered > sol.delivered[v] + kTieEpsilon;
            const bool tied = std::fabs(delivered - sol.delivered[v]) <= kTieEpsilon &&
                              (!sol.source[v].valid() || sources[si].province < sol.source[v]);
            if (!better && !tied) continue;
            sol.delivered[v] = delivered;
            sol.distance[v] = sol.dists[si][v];
            sol.source[v] = sources[si].province;
            sol.winner[v] = static_cast<uint32_t>(si);
        }
    }

    // The bottleneck is where capacity along the chosen route drops hardest: the
    // province that most constrains what finally arrives.
    for (uint32_t v = 0; v < n; ++v) {
        if (sol.winner[v] == INVALID_ID) continue;
        const uint32_t si = sol.winner[v];
        const std::vector<double>& d = sol.dists[si];
        const std::vector<ProvinceId>& pv = sol.prevs[si];
        const double capacity = sources[si].capacity;
        ProvinceId step = ProvinceId(v);
        ProvinceId worst = sources[si].province;
        double best_drop = -1.0;
        for (size_t guard = 0; guard < n; ++guard) {
            const ProvinceId pr = pv[step.v];
            if (!pr.valid()) break;
            const double cap_step = capacity / (1.0 + penalty * d[step.v]);
            const double cap_prev = capacity / (1.0 + penalty * d[pr.v]);
            const double drop = cap_prev - cap_step;
            if (drop > best_drop + kTieEpsilon) {
                best_drop = drop;
                worst = step;
            }
            step = pr;
        }
        sol.bottleneck[v] = worst;
    }
    return sol;
}

// Capacity at one step of a route, read from the winning source's own distance tree.
double route_step_capacity(const SupplySolution& sol, const std::vector<SupplySource>& sources,
                           ProvinceId province, ProvinceId step, double penalty) {
    const uint32_t si = sol.winner[province.v];
    if (si == INVALID_ID) return 0.0;
    const double dist = sol.dists[si][step.v];
    if (!std::isfinite(dist)) return 0.0;
    return sources[si].capacity / (1.0 + penalty * dist);
}

double division_supply_demand(const Game& g, const Division& d) {
    const DivisionTemplate* t = g.content.template_def(d.template_id);
    return t != nullptr ? t->supply_use : 0.0;
}

double division_fuel_demand_hour(const Game& g, const Division& d) {
    const DivisionTemplate* t = g.content.template_def(d.template_id);
    if (t == nullptr) return 0.0;
    return t->fuel_use * g.content.constants.fuel_demand_per_day;
}

// Route from the winning source to `province`, with the capacity at each step.
void build_route(const SupplySolution& sol, const std::vector<SupplySource>& sources,
                 ProvinceId province, double penalty, std::vector<SupplyRouteStep>* out_path) {
    if (out_path == nullptr) return;
    out_path->clear();
    if (province.v >= sol.winner.size() || sol.winner[province.v] == INVALID_ID) return;
    const std::vector<ProvinceId>& pv = sol.prevs[sol.winner[province.v]];

    std::vector<ProvinceId> reverse;
    ProvinceId step = province;
    for (size_t guard = 0; guard <= pv.size(); ++guard) {
        reverse.push_back(step);
        const ProvinceId pr = pv[step.v];
        if (!pr.valid()) break;
        step = pr;
    }
    for (auto it = reverse.rbegin(); it != reverse.rend(); ++it) {
        SupplyRouteStep s;
        s.province = *it;
        s.capacity = route_step_capacity(sol, sources, province, *it, penalty);
        out_path->push_back(s);
    }
}

}  // namespace

std::vector<SupplySource> supply_sources(const World& w, CountryId country) {
    // This entry point has no Content access, so it reports capacities under the
    // documented default constants (the ARCHITECTURE 5.6 numbers). phase_supply uses
    // the tuned SimConstants from content; only an override of the two rail/
    // infrastructure bonuses can make the two differ.
    const SimConstants k;
    const Side side = build_side(w, country);
    return collect_sources(w, country, side, k);
}

void phase_supply(Game& g) {
    World& w = g.world;
    const SimConstants& k = g.content.constants;
    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());

    // Local demand: a baseline trickle plus what the divisions standing there need.
    std::vector<double> demand(n, kBaselineDemand);
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (!d.location.valid()) return;  // off-map: still training
        if (d.location.v >= n) return;
        demand[d.location.v] += division_supply_demand(g, d);
    });

    std::vector<double> distance_by_province(n, kInf);

    // Pass 1: province supply levels, countries ascending (so an ally's network is
    // already written when a division standing in allied territory reads it).
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const Side side = build_side(w, cid);
        const std::vector<SupplySource> sources = collect_sources(w, cid, side, k);
        const SupplySolution sol = compute_supply(w, k, side, sources);
        w.provinces.for_each([&](ProvinceId pid, Province& p) {
            if (p.is_sea || p.controller != cid) return;
            const double dem = demand[pid.v] > kBaselineDemand ? demand[pid.v] : kBaselineDemand;
            p.supply_level = clamp01(safe_div(sol.delivered[pid.v], dem));
            p.supply_source = sol.source[pid.v];
            p.supply_bottleneck = sol.bottleneck[pid.v];
            distance_by_province[pid.v] = sol.distance[pid.v];
        });
    });

    // Pass 2: divisions read their location's level, lose effectiveness beyond hub
    // range, and draw fuel from the national stock in ascending division order.
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const Side side = build_side(w, cid);
        double fuel_left = std::isfinite(c.fuel) && c.fuel > 0.0 ? c.fuel : 0.0;
        w.divisions.for_each([&](DivisionId, Division& d) {
            if (d.country != cid) return;
            if (!d.location.valid()) return;  // off-map: still training
            const Province* p = w.province(d.location);
            double level = 0.0;
            if (p != nullptr && !p->is_sea && side_has(side, p->controller)) {
                level = p->supply_level;
                const double dist = distance_by_province[p->id.v];
                if (std::isfinite(dist) && dist > k.supply_hub_radius) {
                    level /= (1.0 + k.supply_range_penalty * (dist - k.supply_hub_radius));
                }
            }
            d.supply = clamp01(level);

            const double demand_hour = division_fuel_demand_hour(g, d);
            if (demand_hour <= 0.0) {
                d.fuel = d.supply > 0.0 ? 1.0 : 0.0;
                return;
            }
            if (d.supply <= 0.0) {
                d.fuel = 0.0;
                return;
            }
            const double got = fuel_left < demand_hour ? fuel_left : demand_hour;
            d.fuel = clamp01(safe_div(got, demand_hour));
            fuel_left -= got;
        });
        c.fuel = fuel_left > 0.0 ? fuel_left : 0.0;
    });
}

double explain_supply_route(const Game& g, CountryId country, ProvinceId province,
                            std::vector<SupplyRouteStep>* out_path, ProvinceId* bottleneck) {
    if (out_path != nullptr) out_path->clear();
    if (bottleneck != nullptr) *bottleneck = ProvinceId{};
    if (!province.valid()) return 0.0;

    const World& w = g.world;
    if (w.province(province) == nullptr) return 0.0;

    const SimConstants& k = g.content.constants;
    const Side side = build_side(w, country);
    const std::vector<SupplySource> sources = collect_sources(w, country, side, k);
    if (sources.empty()) return 0.0;

    const SupplySolution sol = compute_supply(w, k, side, sources);
    if (province.v >= sol.delivered.size() || !sol.source[province.v].valid()) return 0.0;

    build_route(sol, sources, province, k.supply_range_penalty, out_path);
    if (bottleneck != nullptr) *bottleneck = sol.bottleneck[province.v];
    return sol.delivered[province.v];
}

}  // namespace hoi
