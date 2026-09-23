// Logistics: a real supply network (spec sections 40-41, ARCHITECTURE 5.6).
//
// Supply is a flow over the province graph: sources (capital, supply hubs, ports) push
// capacity outwards, capacity decays with graph distance, and a province's supply
// level is delivered capacity over local demand. Encirclement needs no special
// case: a province with no controlled path back to a source simply gets zero.
//
// Ports and overseas supply (LOG-006): a coastal province with a naval base is a
// source like a hub, scaled by the naval-base level. A port the land network reaches
// is a *home* port; any other usable port is *overseas* and is fed by a sea route to
// the nearest home port in friendly-controlled sea zones (one extra capacity decay
// per sea-zone hop). Enemy naval control in a port's zone scales its capacity down
// (max(0, 1 - enemy share)), a hostile convoy-raiding force above
// naval_supply_raid_threshold at the port zeroes it, and a country with no convoy_1
// in stock loses its whole overseas network while its home network keeps flowing. The
// sea route draws convoy_1 from the stockpile every tick it operates.
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
// A country with ports additionally caches two networks (one without the overseas
// ports, one with them) and each tick reads whichever the convoy stock allows, so
// running out of convoys needs no recompute.
// Province levels and division supply/fuel are still refreshed EVERY tick from the
// cached network plus the current demand and fuel stock, so encirclement reacts
// within one tick even when the network itself was not recomputed.
//
// Measured on the shipped scenario (`game --days 5 --quiet`): supply 43.4 -> 1.2
// ms/tick and whole-tick p95 56.7 -> 7.2 ms. A recompute costs ~6 ms and lands at
// most once every kSupplyRecomputeHours; the other ticks cost ~0.1 ms.
//
// Ports add a little work: every country in the shipped scenario now has a level-1/2
// naval base, so each rebuild computes an extra port-source network, and the naval
// control the routes depend on enters the signature (a control change on a tick
// forces a recompute). Measured on the same scenario: 1.58 -> 1.76 ms/tick for the
// supply phase, whole-tick p95 13.1 -> 13.5 ms. The port path adds no O(ships^2)
// work: ports are scanned once per rebuild, raiders are bucketed by sea zone, and a
// country with no usable port takes the original single-network path.

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
#include "sim/navy.h"

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

// The land network's sources: the controlled capital plus every controlled supply
// hub. Ports are added separately by collect_port_sources.
std::vector<SupplySource> collect_land_sources(const World& w, CountryId country, const Side& side,
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
    // Full network: capital + land hubs + every usable port, overseas ports included.
    std::vector<double> delivered;
    std::vector<double> distance;
    std::vector<ProvinceId> source;
    std::vector<ProvinceId> bottleneck;

    // Ports and convoys. `has_ports` is set when the country has any usable port, in
    // which case the base network below is populated. `has_overseas` means at least
    // one port is fed by a sea route, so the network needs convoys to keep delivering.
    bool has_ports = false;
    bool has_overseas = false;
    // Sum of the overseas ports' capacities: the throughput the sea routes draw
    // convoys against.
    double overseas_capacity = 0.0;
    // Network without the overseas ports (capital + land hubs + home ports), read when
    // the country has no convoys so overseas supply stops while the home network flows.
    std::vector<double> base_delivered;
    std::vector<double> base_distance;
    std::vector<ProvinceId> base_source;
    std::vector<ProvinceId> base_bottleneck;
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
    // Per country: whether overseas supply is on this tick (convoys in stock). Filled
    // every tick, not part of the cached signature.
    std::vector<uint8_t> overseas_active;
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
        h = fnv_mix(h, p.region.v);
        h = fnv_mix(h, p.supply_hub ? 1u : 0u);
        h = fnv_mix(h, static_cast<uint32_t>(p.railway_level));
        h = fnv_mix(h, static_cast<uint32_t>(p.infrastructure));
        h = fnv_mix(h, static_cast<uint32_t>(p.naval_base));
        h = fnv_mix(h, p.coastal ? 1u : 0u);
        h = fnv_mix(h, p.is_sea ? 1u : 0u);
        h = fnv_mix(h, p.is_capital ? 1u : 0u);
        for (ProvinceId a : p.adj) h = fnv_mix(h, a.v);
        h = fnv_mix(h, 0xACACACACu);
        for (ProvinceId a : p.sea_adj) h = fnv_mix(h, a.v);
        h = fnv_mix(h, 0xADADADADu);
    });
    h = fnv_mix(h, w.states.capacity());
    w.states.for_each([&](StateId id, const State& st) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, st.impassable ? 1u : 0u);
    });
    // Sea zones and naval control (ports are sources whose capacity depends on both).
    h = fnv_mix_double(h, k.naval_base_supply_per_level);
    h = fnv_mix_double(h, k.naval_supply_sea_range_penalty);
    h = fnv_mix(h, w.regions.capacity());
    w.regions.for_each([&](RegionId id, const Region& r) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, r.is_sea ? 1u : 0u);
        for (const std::pair<CountryId, double>& e : r.naval_control) {
            h = fnv_mix(h, e.first.v);
            h = fnv_mix_double(h, e.second);
        }
        h = fnv_mix(h, 0xAEAEAEAEu);
    });
    // Raiding task forces in a zone zero a port's route.
    h = fnv_mix_double(h, k.naval_supply_raid_threshold);
    h = fnv_mix(h, w.task_forces.capacity());
    w.task_forces.for_each([&](TaskForceId id, const TaskForce& tf) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, tf.country.v);
        h = fnv_mix(h, tf.sea_region.v);
        h = fnv_mix(h, static_cast<uint32_t>(tf.mission));
        for (ShipId sid : tf.ships) h = fnv_mix(h, sid.v);
        h = fnv_mix(h, 0xAFAFAFAFu);
    });
    h = fnv_mix(h, w.ships.capacity());
    w.ships.for_each([&](ShipId id, const Ship& sh) {
        h = fnv_mix(h, id.v);
        h = fnv_mix(h, sh.country.v);
        h = fnv_mix(h, sh.task_force.v);
        h = fnv_mix_double(h, sh.strength);
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

// ------------------------------------------------------------ ports / convoys --

// A usable port before its sea-route factors are applied.
struct RawPort {
    ProvinceId province;
    RegionId zone;       // adjacent sea zone
    double base = 0.0;   // capacity before naval control / route / raid factors
    bool reachable = false;  // land network reaches it -> home port
};

struct PortSource {
    ProvinceId province;
    double capacity = 0.0;
    bool overseas = false;
};

// Capacity a usable port contributes before sea-route factors: like a hub (rail and
// infrastructure) scaled by the naval-base level.
double port_capacity(const Province& p, const SimConstants& k) {
    return source_capacity(p, k) *
           (1.0 + k.naval_base_supply_per_level * static_cast<double>(p.naval_base));
}

// Friendly control share in a sea zone, read from the derived table phase_naval writes
// (the same pattern air reads Region::air_control by).
double friendly_control_share(const Region& region, const Side& side) {
    double sum = 0.0;
    for (const std::pair<CountryId, double>& e : region.naval_control) {
        if (side_has(side, e.first)) sum += clamp01(e.second);
    }
    return clamp01(sum);
}

// Strongest hostile control share in a sea zone, over countries at war with `country`.
double hostile_control_share(const World& w, const Region& region, CountryId country,
                             const Side& side) {
    double worst = 0.0;
    for (const std::pair<CountryId, double>& e : region.naval_control) {
        if (!e.first.valid() || side_has(side, e.first)) continue;
        if (!countries_at_war(w, country, e.first)) continue;
        worst = std::max(worst, clamp01(e.second));
    }
    return clamp01(worst);
}

// Hostile convoy-raiding strength bucketed by sea zone: the summed remaining
// strength of ships in ConvoyRaid task forces. One pass over the task-force store,
// so a port's zone lookup is O(1) and nothing is O(ships * ports).
void collect_raider_strength(const World& w, CountryId country, const Side& side,
                             std::vector<double>* out) {
    out->assign(w.regions.capacity(), 0.0);
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (tf.mission != NavalMission::ConvoyRaid) return;
        if (!tf.sea_region.valid() || tf.sea_region.v >= out->size()) return;
        if (side_has(side, tf.country)) return;
        if (!countries_at_war(w, country, tf.country)) return;
        double strength = 0.0;
        for (ShipId sid : tf.ships) {
            const Ship* ship = w.ship(sid);
            if (ship != nullptr) strength += clamp01(ship->strength);
        }
        (*out)[tf.sea_region.v] += strength;
    });
}

// Usable ports for `country`: coastal, with a naval base, controlled by the side, with
// friendly naval control in their zone (a port whose zone has no friendly control at
// all delivers nothing). Ascending province id.
void scan_usable_ports(const Game& g, const Side& side, std::vector<RawPort>* out) {
    const World& w = g.world;
    const SimConstants& k = g.content.constants;
    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        if (p.is_sea || !p.coastal || p.naval_base <= 0) return;
        if (!side_has(side, p.controller)) return;
        const State* st = w.state(p.state);
        if (st != nullptr && st->impassable) return;

        const RegionId zone = adjacent_sea_region(g, id);
        if (!zone.valid()) return;  // landlocked despite the coastal flag
        const Region* region = w.regions.try_get(zone);
        if (region == nullptr) return;
        if (!(friendly_control_share(*region, side) > 0.0)) return;

        RawPort port;
        port.province = id;
        port.zone = zone;
        port.base = port_capacity(p, k);
        out->push_back(port);
    });
}

// Provinces the land network can reach from `sources` over friendly, passable land.
// Used only to tell a home port (fed over land) from an overseas one; a plain BFS is
// enough and much cheaper than a second Dijkstra per ported country.
void land_reachable(const World& w, const Side& side, const std::vector<SupplySource>& sources,
                    std::vector<uint8_t>* out) {
    const uint32_t slots = static_cast<uint32_t>(w.provinces.capacity());
    out->assign(slots, 0);
    std::vector<ProvinceId> queue;
    for (const SupplySource& s : sources) {
        if (!s.province.valid() || s.province.v >= slots) continue;
        if (!traversable(w, s.province, side)) continue;
        if ((*out)[s.province.v]) continue;
        (*out)[s.province.v] = 1;
        queue.push_back(s.province);
    }
    for (size_t i = 0; i < queue.size(); ++i) {
        const Province* p = w.province(queue[i]);
        if (p == nullptr) continue;
        for (ProvinceId nb : p->adj) {
            if (nb.v >= slots || (*out)[nb.v]) continue;
            if (!traversable(w, nb, side)) continue;
            (*out)[nb.v] = 1;
            queue.push_back(nb);
        }
    }
}

// Resolve each usable port's sea-route factors. `land_reach` is the land network's
// reach, used to tell a home port (fed over land) from an overseas one (fed over sea).
void resolve_port_sources(const Game& g, CountryId country, const Side& side,
                          const std::vector<uint8_t>& land_reach, const std::vector<RawPort>& raw,
                          std::vector<PortSource>* out) {
    const World& w = g.world;
    const SimConstants& k = g.content.constants;
    if (raw.empty()) return;

    std::vector<uint8_t> reachable(raw.size(), 0);
    for (size_t i = 0; i < raw.size(); ++i) {
        const uint32_t pv = raw[i].province.v;
        reachable[i] = pv < land_reach.size() && land_reach[pv] ? 1 : 0;
    }
    std::vector<double> raiders;
    collect_raider_strength(w, country, side, &raiders);

    for (size_t idx = 0; idx < raw.size(); ++idx) {
        const RawPort& port = raw[idx];
        const Region* region = w.regions.try_get(port.zone);
        if (region == nullptr) continue;
        if (raiders[port.zone.v] >= k.naval_supply_raid_threshold) continue;  // raided out

        // Blockade: hostile control in the zone cuts the capacity linearly.
        const double enemy = hostile_control_share(w, *region, country, side);
        double capacity = port.base * std::max(0.0, 1.0 - enemy);

        bool overseas = false;
        if (!reachable[idx]) {
            // No land path to the capital: only a sea route to a home port keeps it
            // alive. Best route = fewest sea-zone hops; ties to the lowest port id
            // because `raw` ascends.
            overseas = true;
            double best = 0.0;
            for (size_t j = 0; j < raw.size(); ++j) {
                if (!reachable[j]) continue;
                const int hops = sea_region_distance(g, raw[j].zone, port.zone);
                if (hops < 0) continue;
                const double factor =
                    1.0 / (1.0 + k.naval_supply_sea_range_penalty * static_cast<double>(hops));
                if (factor > best) best = factor;
            }
            capacity *= best;
        }
        if (!(capacity > 0.0)) continue;

        PortSource src;
        src.province = port.province;
        src.capacity = capacity;
        src.overseas = overseas;
        out->push_back(src);
    }
}

// True when the country holds convoy_1 in its stockpile. A country with no convoys
// cannot sustain an overseas network.
bool country_has_convoys(const Content& content, const Country& c) {
    const EquipmentId convoy = content.equipment_id("convoy_1");
    if (!convoy.valid() || convoy.v >= c.equipment_stockpile.size()) return false;
    return c.equipment_stockpile[convoy.v] > 0.0;
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

// Whether the full network is the one to read this tick: false only for a country
// with overseas ports and no convoys in stock, which reads the base network instead.
bool overseas_network_active(const NetworkCache& cache, const CountrySolution& sol) {
    if (!sol.has_ports) return true;
    return sol.country < cache.overseas_active.size() && cache.overseas_active[sol.country] != 0;
}

// Full network rebuild: countries ascending, one grouped search set per country.
// A country with no usable port takes exactly the pre-naval path (one search set,
// buffers reused). A country with ports classifies them as home (land-fed) or
// overseas (sea-fed) and caches both the base network and the full network, so a
// convoy shortage switches between them without a recompute.
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
        const std::vector<SupplySource> land = collect_land_sources(w, cid, side, k);

        std::vector<RawPort> raw;
        scan_usable_ports(g, side, &raw);
        if (raw.empty()) {
            sol.has_ports = false;
            sol.has_overseas = false;
            sol.overseas_capacity = 0.0;
            sol.base_delivered.clear();
            sol.base_distance.clear();
            sol.base_source.clear();
            sol.base_bottleneck.clear();
            compute_country_supply(w, k, side, land, slots, &cache->scratch, &sol);
            return;
        }

        // The land-only reach decides which ports are home ports.
        std::vector<uint8_t> land_reach;
        land_reachable(w, side, land, &land_reach);

        std::vector<PortSource> ports;
        resolve_port_sources(g, cid, side, land_reach, raw, &ports);

        std::vector<SupplySource> base = land;
        std::vector<SupplySource> full = land;
        double overseas_capacity = 0.0;
        bool has_overseas = false;
        for (const PortSource& p : ports) {
            SupplySource s;
            s.province = p.province;
            s.country = cid;
            s.capacity = p.capacity;
            s.hub = p.province;
            if (p.overseas) {
                full.push_back(s);
                has_overseas = true;
                overseas_capacity += p.capacity;
            } else {
                base.push_back(s);
            }
        }
        auto by_province = [](const SupplySource& a, const SupplySource& b) {
            return a.province < b.province;
        };
        std::sort(base.begin(), base.end(), by_province);
        std::sort(full.begin(), full.end(), by_province);

        CountrySolution base_sol;
        compute_country_supply(w, k, side, base, slots, &cache->scratch, &base_sol);

        sol.has_ports = true;
        sol.has_overseas = has_overseas;
        sol.overseas_capacity = overseas_capacity;
        sol.base_delivered = std::move(base_sol.delivered);
        sol.base_distance = std::move(base_sol.distance);
        sol.base_source = std::move(base_sol.source);
        sol.base_bottleneck = std::move(base_sol.bottleneck);
        if (has_overseas) {
            compute_country_supply(w, k, side, full, slots, &cache->scratch, &sol);
        } else {
            sol.delivered = sol.base_delivered;
            sol.distance = sol.base_distance;
            sol.source = sol.base_source;
            sol.bottleneck = sol.base_bottleneck;
        }
    });
    cache->countries.resize(index);
}

}  // namespace

std::vector<SupplySource> supply_sources(const World& w, CountryId country) {
    // This entry point has no Content access, so it reports capacities under the
    // documented default constants (the ARCHITECTURE 5.6 numbers) and only the land
    // network (capital + hubs): ports need the Game for their sea routes and are
    // reported by phase_supply's network. phase_supply uses the tuned SimConstants.
    const SimConstants k;
    const Side side = build_side(w, country);
    return collect_land_sources(w, country, side, k);
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

    // Pass 0: overseas supply needs convoys. A country with no convoy_1 in stock reads
    // the base network (home network still flows, overseas stops); a country with
    // stock and overseas routes draws convoy_1 from the stockpile this tick.
    std::vector<uint8_t>& active = cache.overseas_active;
    active.assign(w.countries.capacity(), 0);
    const EquipmentId convoy = g.content.equipment_id("convoy_1");
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const CountrySolution* sol = find_solution(cache, cid);
        if (sol == nullptr || !sol->has_overseas) return;
        if (!convoy.valid() || convoy.v >= c.equipment_stockpile.size()) return;
        double& stock = c.equipment_stockpile[convoy.v];
        if (!(stock > 0.0)) {
            stock = 0.0;
            return;
        }
        active[cid.v] = 1;
        const double use = sol->overseas_capacity * k.naval_supply_convoy_use_per_capacity;
        stock -= std::min(stock, use);
        if (stock < 0.0) stock = 0.0;
    });

    // Pass 1: province supply levels, countries ascending (so an ally's network is
    // already written when a division standing in allied territory reads it).
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const CountrySolution* sol = find_solution(cache, cid);
        if (sol == nullptr) return;
        const bool full = overseas_network_active(cache, *sol);
        const std::vector<double>& delivered = full ? sol->delivered : sol->base_delivered;
        const std::vector<ProvinceId>& source = full ? sol->source : sol->base_source;
        const std::vector<ProvinceId>& bottleneck = full ? sol->bottleneck : sol->base_bottleneck;
        w.provinces.for_each([&](ProvinceId pid, Province& p) {
            if (p.is_sea || p.controller != cid) return;
            const double dem = demand[pid.v] > kBaselineDemand ? demand[pid.v] : kBaselineDemand;
            p.supply_level = clamp01(safe_div(delivered[pid.v], dem));
            p.supply_source = source[pid.v];
            p.supply_bottleneck = bottleneck[pid.v];
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
                const bool full = overseas_network_active(cache, *holder);
                const std::vector<double>& delivered =
                    full ? holder->delivered : holder->base_delivered;
                const std::vector<double>& distance =
                    full ? holder->distance : holder->base_distance;
                const double dem = demand[p->id.v] > kBaselineDemand ? demand[p->id.v] : kBaselineDemand;
                level = clamp01(safe_div(delivered[p->id.v], dem));
                const double dist = distance[p->id.v];
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
    const std::vector<SupplySource> land = collect_land_sources(w, country, side, k);

    // This is the debugger path (console/UI), so it recomputes the network it needs
    // rather than reading the phase cache, whose refresh interval could make the
    // answer differ from what the player sees on screen. Ports are added the same way
    // the phase adds them, gated by the country's current convoy stock.
    SupplyScratch scratch;
    ensure_edge_costs(w, k, &scratch);
    CountrySolution sol;

    std::vector<SupplySource> sources = land;
    std::vector<RawPort> raw;
    scan_usable_ports(g, side, &raw);
    if (!raw.empty()) {
        std::vector<uint8_t> land_reach;
        land_reachable(w, side, land, &land_reach);
        std::vector<PortSource> ports;
        resolve_port_sources(g, country, side, land_reach, raw, &ports);
        const Country* c = w.country(country);
        const bool convoy_ok = c != nullptr && country_has_convoys(g.content, *c);
        for (const PortSource& p : ports) {
            if (p.overseas && !convoy_ok) continue;  // no convoys: overseas routes off
            SupplySource s;
            s.province = p.province;
            s.country = country;
            s.capacity = p.capacity;
            s.hub = p.province;
            sources.push_back(s);
        }
        std::sort(sources.begin(), sources.end(),
                  [](const SupplySource& a, const SupplySource& b) {
                      return a.province < b.province;
                  });
    }
    if (sources.empty()) return 0.0;

    compute_country_supply(w, k, side, sources, n, &scratch, &sol);
    if (province.v >= sol.delivered.size() || !sol.source[province.v].valid()) return 0.0;

    build_route(scratch, province, k.supply_range_penalty, out_path);
    if (bottleneck != nullptr) *bottleneck = sol.bottleneck[province.v];
    return sol.delivered[province.v];
}

}  // namespace hoi
