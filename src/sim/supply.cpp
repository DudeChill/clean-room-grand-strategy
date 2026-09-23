// Logistics: a real supply network (spec sections 40-41, ARCHITECTURE 5.6).
//
// Supply is a flow over the province graph: sources (capital, supply hubs) push
// capacity outwards, capacity decays with graph distance, and a province's supply
// level is delivered capacity over local demand. Encirclement needs no special
// case: a province with no controlled path back to a source simply gets zero.
//
// Performance model (measured on the shipped scenario: 2275 provinces, 332 hubs,
// 10 countries, ~580 source searches per tick without the measures below):
//   * the network (delivered capacity, distance, source, bottleneck per province) is
//     memoized and only recomputed when a deterministic signature of the
//     supply-relevant world changes, or after kSupplyRecomputeHours;
//   * sources are grouped by capacity and each group runs ONE multi-source Dijkstra,
//     which is exactly equivalent to running one per source (for a fixed capacity the
//     best source is the nearest, ties to the lowest province id);
//   * every search is bounded by a hop cap and a delivered-capacity floor;
//   * distance/parent/settled/heap buffers are reused between sources and countries.
// Province levels and division supply/fuel are still refreshed EVERY tick from the
// cached network plus the current demand and fuel stock, so encirclement reacts
// within one tick even when the network itself was not recomputed.
//
// Measured on the shipped scenario (`game --days 5 --quiet`): supply 43.4 -> 1.2
// ms/tick and whole-tick p95 56.7 -> 7.2 ms. A recompute costs ~6 ms and lands at
// most once every kSupplyRecomputeHours; the other ticks cost ~0.1 ms.

#include "sim/supply.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
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

// --- bounded-search constants (documented error bounds) -----------------------
// A branch is abandoned where the source would deliver less than this. A pruned
// province then reports delivered = 0 instead of a value below the floor, so the
// supply_level error is at most kMinDeliveredCapacity / demand, i.e. <= 0.5 for the
// 0.1 baseline demand, and the division-supply error is at most 0.5. With the
// shipped constants (capacity 15.2..22.8, range penalty 0.1) the floor only bites
// beyond 454..4554 hops of graph distance, so it is a safety bound, not a routine
// prune; it matters for maps or constant sets with a much larger range.
constexpr double kMinDeliveredCapacity = 0.05;
// Hard cap on graph distance from a source. Provinces beyond it report 0; their true
// delivered capacity would be at most capacity / (1 + 0.1 * hops) (0.16 at 128 hops
// for the largest shipped hub), so the supply_level error is bounded by that over
// demand. The longest supply route settled on the shipped scenario is 74.07 hops, so
// the cap does not change any result there (verified by comparing a digest of every
// province level/source/bottleneck and division supply/fuel against the unbounded
// implementation: identical).
constexpr double kMaxSupplyHops = 128.0;
// The network is recomputed at least this often (game hours) even when the
// signature is unchanged; 6 hours keeps a stale network shorter-lived than a day
// while cutting the recompute cost to at most 1 tick in 6.
constexpr Tick kSupplyRecomputeHours = 6;

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

// --------------------------------------------------------------- search core --

// Sources that deliver identically at a given distance share one search. Capacity is
// grouped bit-exactly: for a fixed capacity the best source at a province is the
// nearest one, so a multi-source Dijkstra over the group is exact, and the per-source
// result is recovered by the (distance, source id) ordering inside the search.
struct CapacityGroup {
    double capacity = 0.0;
    std::vector<ProvinceId> sources;  // ascending province id
};

struct QueueNode {
    double dist = 0.0;
    uint32_t source = INVALID_ID;    // source province that the path started from
    uint32_t province = INVALID_ID;
    uint32_t from = INVALID_ID;      // predecessor province (INVALID at a source)
};

struct QueueNodeGreater {
    bool operator()(const QueueNode& a, const QueueNode& b) const {
        if (a.dist != b.dist) return a.dist > b.dist;
        if (a.source != b.source) return a.source > b.source;
        if (a.province != b.province) return a.province > b.province;
        return a.from > b.from;
    }
};

// Reusable per-group buffers. `reset` touches only the provinces the previous search
// visited, so a search costs O(visited) plus the relaxations, never O(map).
struct GroupSearch {
    std::vector<double> dist;
    std::vector<ProvinceId> parent;
    std::vector<ProvinceId> origin;  // source province that settled each province
    std::vector<uint8_t> settled;
    std::vector<ProvinceId> touched;  // provinces with a distance written
    std::vector<ProvinceId> order;    // settle order, for the combine pass
    std::vector<QueueNode> heap;      // manual heap: clear() keeps the allocation

    void ensure(uint32_t slots) {
        if (dist.size() == slots) return;
        dist.assign(slots, kInf);
        parent.assign(slots, ProvinceId{});
        origin.assign(slots, ProvinceId{});
        settled.assign(slots, 0);
        touched.clear();
        order.clear();
        heap.clear();
    }

    void reset() {
        for (ProvinceId p : touched) {
            dist[p.v] = kInf;
            parent[p.v] = ProvinceId{};
            origin[p.v] = ProvinceId{};
            settled[p.v] = 0;
        }
        touched.clear();
        order.clear();
        heap.clear();
    }

    void push(const QueueNode& n) {
        heap.push_back(n);
        std::push_heap(heap.begin(), heap.end(), QueueNodeGreater{});
    }
    QueueNode pop() {
        std::pop_heap(heap.begin(), heap.end(), QueueNodeGreater{});
        const QueueNode n = heap.back();
        heap.pop_back();
        return n;
    }
};

// Scratch shared by every country of one recompute (and by the route explainer).
struct SupplyScratch {
    std::vector<CapacityGroup> groups;
    std::vector<GroupSearch> searches;
    std::vector<uint32_t> winner;   // group index that won each province
    std::vector<uint8_t> reachable;  // per country: land, passable, held by the side
    // Terrain/rail/infrastructure cost of entering each province, precomputed once per
    // recompute so the hot relaxation loop does no cross-module calls.
    std::vector<double> edge_cost;
};

void ensure_edge_costs(const World& w, const SimConstants& k, SupplyScratch* scratch) {
    const uint32_t slots = static_cast<uint32_t>(w.provinces.capacity());
    if (scratch->edge_cost.size() != slots) scratch->edge_cost.assign(slots, kMinEdgeCost);
    for (uint32_t v = 0; v < slots; ++v) {
        const Province* p = w.province(ProvinceId(v));
        scratch->edge_cost[v] = p != nullptr ? supply_edge_cost(*p, k) : kMinEdgeCost;
    }
}

// Which provinces this country's network may use. Built once per country instead of
// running a side lookup per edge relaxation.
void ensure_reachable(const World& w, const Side& side, SupplyScratch* scratch) {
    const uint32_t slots = static_cast<uint32_t>(w.provinces.capacity());
    if (scratch->reachable.size() != slots) scratch->reachable.assign(slots, 0);
    for (uint32_t v = 0; v < slots; ++v) {
        scratch->reachable[v] = traversable(w, ProvinceId(v), side) ? 1 : 0;
    }
}

struct CountrySolution {
    uint32_t country = INVALID_ID;
    std::vector<double> delivered;
    std::vector<double> distance;
    std::vector<ProvinceId> source;
    std::vector<ProvinceId> bottleneck;
};

// The memo of the last computed network. Pure cache: it holds no gameplay state and
// is keyed by a signature of everything the network depends on.
struct NetworkCache {
    bool valid = false;
    uint64_t signature = 0;
    Tick computed_tick = 0;
    uint32_t province_slots = 0;
    std::vector<CountrySolution> countries;  // ascending country id
    std::vector<double> demand;              // scratch: last demand vector
    SupplyScratch scratch;
};

NetworkCache& network_cache() {
    static NetworkCache cache;
    return cache;
}

uint64_t fnv_mix(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

uint64_t fnv_mix_double(uint64_t h, double v) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit for bit-exact hashing");
    std::memcpy(&bits, &v, sizeof(bits));
    return fnv_mix(h, bits);
}

// Deterministic signature of everything the supply network depends on: the land
// graph, per-province controller/hub/rail/infrastructure, the tuned supply constants,
// and the war/faction structure that decides which provinces are traversable and
// which hubs count as sources (the per-country source count follows from those).
uint64_t supply_signature(const World& w, const SimConstants& k) {
    uint64_t h = 14695981039346656037ull;
    h = fnv_mix_double(h, k.supply_hub_radius);
    h = fnv_mix_double(h, k.supply_range_penalty);
    h = fnv_mix_double(h, k.supply_rail_bonus_per_level);
    h = fnv_mix_double(h, k.supply_infrastructure_bonus_per_level);
    h = fnv_mix(h, w.provinces.capacity());
    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, p.controller.v);
        h = fnv_mix(h, p.state.v);
        h = fnv_mix(h, p.supply_hub ? 1u : 0u);
        h = fnv_mix(h, static_cast<uint32_t>(p.railway_level));
        h = fnv_mix(h, static_cast<uint32_t>(p.infrastructure));
        h = fnv_mix(h, p.is_sea ? 1u : 0u);
        h = fnv_mix(h, p.is_capital ? 1u : 0u);
        for (ProvinceId a : p.adj) h = fnv_mix(h, a.v);
        h = fnv_mix(h, 0xADADADADu);
    });
    h = fnv_mix(h, w.states.capacity());
    w.states.for_each([&](StateId id, const State& st) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, st.impassable ? 1u : 0u);
    });
    h = fnv_mix(h, w.countries.capacity());
    w.countries.for_each([&](CountryId id, const Country& c) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, c.alive ? 1u : 0u);
        h = fnv_mix(h, c.faction);
        h = fnv_mix(h, c.capital.v);
    });
    h = fnv_mix(h, w.wars.capacity());
    w.wars.for_each([&](WarId id, const War& war) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, war.active ? 1u : 0u);
        h = fnv_mix(h, war.aggressor.v);
        for (const WarParticipant& p : war.attackers) h = fnv_mix(h, p.country.v);
        h = fnv_mix(h, 0xB0B0B0B0u);
        for (const WarParticipant& p : war.defenders) h = fnv_mix(h, p.country.v);
        h = fnv_mix(h, 0xC0C0C0C0u);
    });
    h = fnv_mix(h, w.factions.size());
    for (const Faction& f : w.factions) {
        h = fnv_mix(h, f.id);
        h = fnv_mix(h, f.leader.v);
        for (CountryId m : f.members) h = fnv_mix(h, m.v);
        h = fnv_mix(h, 0xD0D0D0D0u);
    }
    return h;
}

// Grouping is by bit-exact capacity on purpose: sources whose capacity differs in the
// last bit must not share a search, and identical capacity is exactly the condition
// under which one multi-source search reproduces the per-source answer.
void group_sources(const std::vector<SupplySource>& sources, std::vector<CapacityGroup>* groups) {
    groups->clear();
    for (const SupplySource& s : sources) {
        CapacityGroup* found = nullptr;
        for (CapacityGroup& g : *groups) {
            if (g.capacity == s.capacity) {
                found = &g;
                break;
            }
        }
        if (found == nullptr) {
            groups->push_back(CapacityGroup{});
            found = &groups->back();
            found->capacity = s.capacity;
        }
        found->sources.push_back(s.province);
    }
}

// Furthest graph distance worth exploring for a group: the hop cap or the point where
// the capacity floor prunes the branch, whichever comes first.
double group_max_distance(double capacity, const SimConstants& k) {
    const double penalty = k.supply_range_penalty;
    if (penalty <= 0.0) return kMaxSupplyHops;
    const double floor_hops = (capacity / kMinDeliveredCapacity - 1.0) / penalty;
    return std::min(kMaxSupplyHops, floor_hops > 0.0 ? floor_hops : 0.0);
}

void run_group_search(const World& w, const SupplyScratch& scratch, const CapacityGroup& group,
                      double max_distance, GroupSearch* s) {
    s->reset();
    const std::vector<uint8_t>& reachable = scratch.reachable;
    const std::vector<double>& edge_cost = scratch.edge_cost;
    for (ProvinceId src : group.sources) {
        if (!reachable[src.v]) continue;
        if (s->dist[src.v] == kInf) s->touched.push_back(src);
        s->dist[src.v] = 0.0;
        s->parent[src.v] = ProvinceId{};
        s->origin[src.v] = src;
        QueueNode node;
        node.dist = 0.0;
        node.source = src.v;
        node.province = src.v;
        s->push(node);
    }

    // First pop of a province settles it: the queue orders by (distance, source id,
    // province), so the settling state is the (nearest, lowest-id) source for that
    // province - exactly the per-source answer.
    while (!s->heap.empty()) {
        const QueueNode node = s->pop();
        const uint32_t v = node.province;
        if (s->settled[v]) continue;
        s->settled[v] = 1;
        s->dist[v] = node.dist;
        s->origin[v] = ProvinceId(node.source);
        s->parent[v] = ProvinceId(node.from);
        s->order.push_back(ProvinceId(v));
        if (node.dist >= max_distance) continue;  // settled but not expanded

        const Province* p = w.province(ProvinceId(v));
        if (p == nullptr) continue;
        for (ProvinceId nb : p->adj) {
            if (s->settled[nb.v]) continue;
            if (!reachable[nb.v]) continue;
            const double next = node.dist + edge_cost[nb.v];
            if (next > max_distance) continue;
            if (next < s->dist[nb.v]) {
                if (s->dist[nb.v] == kInf) s->touched.push_back(nb);
                s->dist[nb.v] = next;
                QueueNode out;
                out.dist = next;
                out.source = node.source;
                out.province = nb.v;
                out.from = v;
                s->push(out);
            }
        }
    }
}

// Recomputes one country's network into `out`. Deterministic: sources ascend, groups
// follow source order, ties go to the lowest source province id.
void compute_country_supply(const World& w, const SimConstants& k, const Side& side,
                            const std::vector<SupplySource>& sources, uint32_t slots,
                            SupplyScratch* scratch, CountrySolution* out) {
    ensure_reachable(w, side, scratch);
    const double penalty = k.supply_range_penalty;
    out->delivered.assign(slots, 0.0);
    out->distance.assign(slots, kInf);
    out->source.assign(slots, ProvinceId{});
    out->bottleneck.assign(slots, ProvinceId{});

    group_sources(sources, &scratch->groups);
    if (scratch->searches.size() < scratch->groups.size()) {
        scratch->searches.resize(scratch->groups.size());
    }
    for (GroupSearch& s : scratch->searches) s.ensure(slots);
    scratch->winner.assign(slots, INVALID_ID);

    for (size_t gi = 0; gi < scratch->groups.size(); ++gi) {
        const CapacityGroup& group = scratch->groups[gi];
        GroupSearch& search = scratch->searches[gi];
        run_group_search(w, *scratch, group, group_max_distance(group.capacity, k), &search);
        const double capacity = group.capacity;
        for (ProvinceId v : search.order) {
            const double dist = search.dist[v.v];
            const double delivered = capacity / (1.0 + penalty * dist);
            const bool better = delivered > out->delivered[v.v] + kTieEpsilon;
            const bool tied = std::fabs(delivered - out->delivered[v.v]) <= kTieEpsilon &&
                              (!out->source[v.v].valid() || search.origin[v.v] < out->source[v.v]);
            if (!better && !tied) continue;
            out->delivered[v.v] = delivered;
            out->distance[v.v] = dist;
            out->source[v.v] = search.origin[v.v];
            scratch->winner[v.v] = static_cast<uint32_t>(gi);
        }
    }

    // The bottleneck is where capacity along the chosen route drops hardest: the
    // province that most constrains what finally arrives.
    for (uint32_t v = 0; v < slots; ++v) {
        const uint32_t gi = scratch->winner[v];
        if (gi == INVALID_ID) continue;
        const GroupSearch& search = scratch->searches[gi];
        const double capacity = scratch->groups[gi].capacity;
        ProvinceId step = ProvinceId(v);
        ProvinceId worst = search.origin[v];
        double best_drop = -1.0;
        for (size_t guard = 0; guard < slots; ++guard) {
            const ProvinceId pr = search.parent[step.v];
            if (!pr.valid()) break;
            const double cap_step = capacity / (1.0 + penalty * search.dist[step.v]);
            const double cap_prev = capacity / (1.0 + penalty * search.dist[pr.v]);
            const double drop = cap_prev - cap_step;
            if (drop > best_drop + kTieEpsilon) {
                best_drop = drop;
                worst = step;
            }
            step = pr;
        }
        out->bottleneck[v] = worst;
    }
}

// Capacity at one step of a route, read from the winning group's own distance tree.
double route_step_capacity(const SupplyScratch& scratch, ProvinceId province, ProvinceId step,
                           double penalty) {
    const uint32_t gi = scratch.winner[province.v];
    if (gi == INVALID_ID) return 0.0;
    const double dist = scratch.searches[gi].dist[step.v];
    if (!std::isfinite(dist)) return 0.0;
    return scratch.groups[gi].capacity / (1.0 + penalty * dist);
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
void build_route(const SupplyScratch& scratch, ProvinceId province, double penalty,
                 std::vector<SupplyRouteStep>* out_path) {
    if (out_path == nullptr) return;
    out_path->clear();
    const uint32_t gi = scratch.winner[province.v];
    if (gi == INVALID_ID) return;
    const std::vector<ProvinceId>& parents = scratch.searches[gi].parent;

    std::vector<ProvinceId> reverse;
    ProvinceId step = province;
    for (size_t guard = 0; guard <= parents.size(); ++guard) {
        reverse.push_back(step);
        const ProvinceId pr = parents[step.v];
        if (!pr.valid()) break;
        step = pr;
    }
    for (auto it = reverse.rbegin(); it != reverse.rend(); ++it) {
        SupplyRouteStep s;
        s.province = *it;
        s.capacity = route_step_capacity(scratch, province, *it, penalty);
        out_path->push_back(s);
    }
}

const CountrySolution* find_solution(const NetworkCache& cache, CountryId c) {
    if (!c.valid()) return nullptr;
    for (const CountrySolution& sol : cache.countries) {
        if (sol.country == c.v) return &sol;
    }
    return nullptr;
}

// Full network rebuild: countries ascending, one grouped search set per country.
void rebuild_network(const Game& g, uint32_t slots, NetworkCache* cache) {
    const World& w = g.world;
    const SimConstants& k = g.content.constants;
    ensure_edge_costs(w, k, &cache->scratch);
    size_t index = 0;
    w.countries.for_each([&](CountryId cid, const Country& c) {
        if (!c.alive) return;
        if (cache->countries.size() <= index) cache->countries.push_back(CountrySolution{});
        CountrySolution& sol = cache->countries[index];
        ++index;
        sol.country = cid.v;
        const Side side = build_side(w, cid);
        const std::vector<SupplySource> sources = collect_sources(w, cid, side, k);
        compute_country_supply(w, k, side, sources, slots, &cache->scratch, &sol);
    });
    cache->countries.resize(index);
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
    NetworkCache& cache = network_cache();

    // Recompute the network only when it can have changed, or when the cached copy is
    // older than the refresh interval.
    const uint64_t signature = supply_signature(w, k);
    const bool stale = !cache.valid || cache.province_slots != n || cache.signature != signature ||
                       w.tick < cache.computed_tick ||
                       w.tick - cache.computed_tick >= kSupplyRecomputeHours;
    if (stale) {
        rebuild_network(g, n, &cache);
        cache.signature = signature;
        cache.computed_tick = w.tick;
        cache.province_slots = n;
        cache.valid = true;
    }

    // Local demand: a baseline trickle plus what the divisions standing there need.
    // Recomputed every tick, so a division arriving in a province changes its level
    // immediately even when the network is cached.
    std::vector<double>& demand = cache.demand;
    demand.assign(n, kBaselineDemand);
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (!d.location.valid()) return;  // off-map: still training
        if (d.location.v >= n) return;
        demand[d.location.v] += division_supply_demand(g, d);
    });

    // Pass 1: province supply levels, countries ascending (so an ally's network is
    // already written when a division standing in allied territory reads it).
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const CountrySolution* sol = find_solution(cache, cid);
        if (sol == nullptr) return;
        w.provinces.for_each([&](ProvinceId pid, Province& p) {
            if (p.is_sea || p.controller != cid) return;
            const double dem = demand[pid.v] > kBaselineDemand ? demand[pid.v] : kBaselineDemand;
            p.supply_level = clamp01(safe_div(sol->delivered[pid.v], dem));
            p.supply_source = sol->source[pid.v];
            p.supply_bottleneck = sol->bottleneck[pid.v];
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
            const CountrySolution* holder =
                p != nullptr ? find_solution(cache, p->controller) : nullptr;
            if (p != nullptr && !p->is_sea && side_has(side, p->controller) && holder != nullptr) {
                const double dem = demand[p->id.v] > kBaselineDemand ? demand[p->id.v] : kBaselineDemand;
                level = clamp01(safe_div(holder->delivered[p->id.v], dem));
                const double dist = holder->distance[p->id.v];
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
    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());
    const Side side = build_side(w, country);
    const std::vector<SupplySource> sources = collect_sources(w, country, side, k);
    if (sources.empty()) return 0.0;

    // This is the debugger path (console/UI), so it recomputes the network it needs
    // rather than reading the phase cache, whose refresh interval could make the
    // answer differ from what the player sees on screen.
    SupplyScratch scratch;
    CountrySolution sol;
    ensure_edge_costs(w, k, &scratch);
    compute_country_supply(w, k, side, sources, n, &scratch, &sol);
    if (province.v >= sol.delivered.size() || !sol.source[province.v].valid()) return 0.0;

    build_route(scratch, province, k.supply_range_penalty, out_path);
    if (bottleneck != nullptr) *bottleneck = sol.bottleneck[province.v];
    return sol.delivered[province.v];
}

}  // namespace hoi
