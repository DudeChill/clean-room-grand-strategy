// Map graph algorithms (spec section 31).
//
// Movement, supply, front detection and encirclement all run on the same land
// adjacency graph, so path legality is defined exactly once here. Every search is
// deterministic: ties break on ascending province id, never on insertion or
// address order.

#include "sim/map.h"

#include <algorithm>
#include <queue>
#include <vector>

#include "core/math.h"

namespace hoi {
namespace {

// Movement-time multiplier of a target province: terrain table times the river
// crossing penalty, divided by the infrastructure speed bonus. Weather is applied
// later by the movement phase (it is region state, not province state).
double terrain_cost_table(Terrain t) {
    switch (t) {
        case Terrain::Plains: return 1.0;
        case Terrain::Forest: return 1.5;
        case Terrain::Hills: return 1.6;
        case Terrain::Mountain: return 3.0;
        case Terrain::Urban: return 1.4;
        case Terrain::Marsh: return 2.0;
        case Terrain::Desert: return 1.1;
        case Terrain::Jungle: return 2.5;
        case Terrain::Ocean:
        case Terrain::ShallowSea:
        case Terrain::DeepOcean:
        case Terrain::Lakes:
            return 4.0;  // water: not traversable by land paths, cost kept finite
        case Terrain::Count: break;
    }
    return 1.0;
}

// True when `country` may enter `p` under the request's rules. `has_access` covers
// the country itself, its allies, faction members and puppets; `at_war` decides
// whether a non-accessible province is hostile.
// The province id is passed explicitly rather than read from Province::id: the
// stored id is authoritative only after a loader has filled it, while a search
// always knows the true store slot of the province it is looking at.
bool can_enter(const World& w, CountryId country, ProvinceId id, const PathRequest& req) {
    const Province* prov = w.province(id);
    if (prov == nullptr) return false;
    const Province& p = *prov;
    if (p.is_sea) return false;
    const bool accessible = w.has_access(country, id);
    if (accessible) return true;
    if (req.require_controlled) {
        // Only hostile provinces can be entered by an offensive path, and only
        // when the caller allows it.
        return req.allow_hostile && w.at_war(country, p.controller);
    }
    // Unrestricted paths may cross neutral ground but never hostile ground
    // unless explicitly allowed.
    if (!req.allow_hostile && w.at_war(country, p.controller)) return false;
    return true;
}

struct HeapNode {
    double cost;
    uint32_t id;
};

// Min-heap ordered by (cost, id): equal-cost nodes always pop in ascending id
// order, which makes path selection independent of the heap's internal order.
struct HeapGreater {
    bool operator()(const HeapNode& a, const HeapNode& b) const {
        if (a.cost != b.cost) return a.cost > b.cost;
        return a.id > b.id;
    }
};

}  // namespace

double terrain_move_cost(const Province& p, bool river_crossing) {
    double cost = terrain_cost_table(p.terrain);
    if (river_crossing) cost *= (1.0 + 0.30);
    const int infra = clamp(p.infrastructure, 0, 10);
    return cost / (1.0 + 0.05 * static_cast<double>(infra));
}

bool adjacent(const World& w, ProvinceId a, ProvinceId b) {
    const Province* pa = w.province(a);
    if (pa == nullptr) return false;
    return std::find(pa->adj.begin(), pa->adj.end(), b) != pa->adj.end();
}

std::vector<ProvinceId> find_path(const World& w, ProvinceId from, ProvinceId to,
                                  const PathRequest& req) {
    std::vector<ProvinceId> path;
    const Province* start = w.province(from);
    const Province* goal = w.province(to);
    if (start == nullptr || goal == nullptr || start->is_sea || goal->is_sea) return path;
    if (from == to) return path;  // already there: no provinces to traverse
    if (!can_enter(w, req.country, to, req)) return path;

    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());
    if (from.v >= n || to.v >= n) return path;

    std::vector<double> best(n, -1.0);
    std::vector<uint32_t> parent(n, INVALID_ID);
    std::priority_queue<HeapNode, std::vector<HeapNode>, HeapGreater> open;

    best[from.v] = 0.0;
    open.push(HeapNode{0.0, from.v});
    while (!open.empty()) {
        const HeapNode node = open.top();
        open.pop();
        if (node.cost > best[node.id]) continue;  // stale entry
        if (node.id == to.v) break;
        const Province* cur = w.province(ProvinceId(node.id));
        if (cur == nullptr) continue;
        for (ProvinceId nb : cur->adj) {
            const Province* np = w.province(nb);
            if (np == nullptr) continue;
            if (!can_enter(w, req.country, nb, req)) continue;
            const double edge = terrain_move_cost(*np, false);
            const double candidate = node.cost + (edge > 0.0 ? edge : 1.0);
            if (best[nb.v] >= 0.0 && candidate >= best[nb.v]) continue;
            best[nb.v] = candidate;
            parent[nb.v] = node.id;
            open.push(HeapNode{candidate, nb.v});
        }
    }

    if (parent[to.v] == INVALID_ID) return path;  // unreachable
    std::vector<ProvinceId> reversed;
    for (uint32_t id = to.v; id != from.v; id = parent[id]) {
        reversed.push_back(ProvinceId(id));
        if (parent[id] == INVALID_ID) return std::vector<ProvinceId>{};  // defensive
    }
    path.assign(reversed.rbegin(), reversed.rend());
    return path;
}

void compute_hop_distances(const World& w, ProvinceId source, const PathRequest& req,
                           std::vector<int32_t>* out) {
    if (out == nullptr) return;
    const uint32_t n = static_cast<uint32_t>(w.provinces.capacity());
    out->assign(n, -1);
    const Province* s = w.province(source);
    if (s == nullptr || s->is_sea || source.v >= n) return;
    out->at(source.v) = 0;

    std::vector<ProvinceId> frontier;
    frontier.push_back(source);
    for (size_t head = 0; head < frontier.size(); ++head) {
        const ProvinceId cur = frontier[head];
        const Province* cp = w.province(cur);
        if (cp == nullptr) continue;
        const int32_t d = out->at(cur.v);
        for (ProvinceId nb : cp->adj) {
            if (nb.v >= n || out->at(nb.v) != -1) continue;
            if (!can_enter(w, req.country, nb, req)) continue;
            out->at(nb.v) = d + 1;
            frontier.push_back(nb);
        }
    }
}

bool province_is_encircled(const World& w, ProvinceId p) {
    const Province* prov = w.province(p);
    if (prov == nullptr || prov->is_sea) return false;
    const CountryId controller = prov->controller;
    if (!controller.valid()) return false;  // nobody supplies an unowned province

    std::vector<ProvinceId> sources;
    w.provinces.for_each([&](ProvinceId id, const Province& other) {
        if (other.is_sea) return;
        if (other.controller != controller) return;
        if (other.is_capital || other.supply_hub) sources.push_back(id);
    });
    if (sources.empty()) return true;  // controller holds no source at all

    PathRequest req;
    req.country = controller;
    req.require_controlled = true;
    req.allow_hostile = false;
    req.for_supply = true;

    std::vector<int32_t> dist;
    compute_hop_distances(w, p, req, &dist);
    for (ProvinceId s : sources) {
        if (s.v < dist.size() && dist[s.v] >= 0) return false;
    }
    return true;
}

std::vector<ProvinceId> compute_front_line(const World& w, CountryId country) {
    std::vector<ProvinceId> front;
    if (!country.valid()) return front;
    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        if (p.is_sea || p.controller != country) return;
        for (ProvinceId nb : p.adj) {
            const Province* np = w.province(nb);
            if (np == nullptr || np->is_sea) continue;
            if (np->controller == country) continue;
            if (w.at_war(country, np->controller)) {
                front.push_back(id);
                return;
            }
        }
    });
    // for_each visits ascending id, so `front` is already sorted.
    return front;
}

std::vector<ProvinceId> front_reserve_provinces(const World& w, CountryId country,
                                                const std::vector<ProvinceId>& front) {
    std::vector<ProvinceId> reserves;
    if (!country.valid() || front.empty()) return reserves;
    std::vector<uint8_t> is_front(static_cast<size_t>(w.provinces.capacity()), 0);
    for (ProvinceId f : front) {
        if (f.v < is_front.size()) is_front[f.v] = 1;
    }
    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        if (p.is_sea || p.controller != country) return;
        if (id.v < is_front.size() && is_front[id.v]) return;  // on the line, not behind it
        for (ProvinceId nb : p.adj) {
            if (nb.v < is_front.size() && is_front[nb.v]) {
                reserves.push_back(id);
                return;
            }
        }
    });
    return reserves;
}

}  // namespace hoi
