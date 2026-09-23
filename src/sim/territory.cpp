// Territorial control, occupation and capitulation effects (ARCHITECTURE phase 5).
//
// A province whose controller is at war with troops standing in it passes to the
// occupying country. Control changes cancel construction in that province and
// invalidate the supply caches that referenced it, state control follows when all
// of a state's provinces agree, and a country whose capital is lost is checked for
// capitulation once per day.

#include "sim/phases.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "core/math.h"
#include "game/game.h"
#include "sim/diplomacy.h"
#include "sim/world.h"

namespace hoi {

// War-side helpers shared by the military phases (implemented in combat.cpp).
bool mil_at_war(const World& w, CountryId a, CountryId b);
bool mil_same_side(const World& w, CountryId a, CountryId b);

void phase_territory(Game& g) {
    World& w = g.world;

    // Troops currently standing in each province, in ascending division id order.
    std::map<ProvinceId, std::vector<CountryId>> occupants;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (!d.location.valid()) return;  // off-map (training)
        if (!w.province(d.location)) return;
        occupants[d.location].push_back(d.country);
    });

    std::vector<ProvinceId> changed;
    w.provinces.for_each([&](ProvinceId pid, Province& p) {
        if (p.is_sea) return;
        const State* st = w.state(p.state);
        if (st && st->impassable) return;
        auto it = occupants.find(pid);
        if (it == occupants.end() || it->second.empty()) return;

        // The lowest-id country that is hostile to the current controller takes
        // the province; an unowned province is taken by its lowest-id occupant.
        CountryId best = p.controller;
        bool found = false;
        for (CountryId c : it->second) {
            if (!c.valid()) continue;
            const bool takes = !p.controller.valid() || mil_at_war(w, c, p.controller);
            if (!takes) continue;
            if (!found || c.v < best.v) {
                best = c;
                found = true;
            }
        }
        if (found && best != p.controller) {
            p.controller = best;
            changed.push_back(pid);
        }
    });

    for (ProvinceId pid : changed) {
        Province* p = w.province(pid);
        if (!p) continue;
        const Country* holder = w.country(p->controller);
        g.log_event("occupation",
                    "province '" + p->name + "' controlled by " +
                        (holder ? holder->tag : std::string("?")),
                    p->controller);

        // Construction in the province can no longer proceed.
        w.countries.for_each([&](CountryId, Country& c) {
            auto& q = c.construction.queue;
            q.erase(std::remove_if(q.begin(), q.end(),
                                   [&](const ConstructionProject& pr) {
                                       return pr.province == pid;
                                   }),
                    q.end());
        });

        // Supply caches that referenced the province are stale.
        w.provinces.for_each([&](ProvinceId, Province& q) {
            if (q.supply_source == pid) q.supply_source = ProvinceId{};
            if (q.supply_bottleneck == pid) q.supply_bottleneck = ProvinceId{};
        });
        p->supply_level = 0.0;
        p->supply_source = ProvinceId{};
        p->supply_bottleneck = ProvinceId{};
    }

    // A state follows its provinces once they agree on a controller.
    if (!changed.empty()) {
        w.states.for_each([&](StateId, State& s) {
            CountryId first;
            bool have = false;
            bool all_same = true;
            for (ProvinceId pp : s.provinces) {
                const Province* q = w.province(pp);
                if (!q || q->is_sea) continue;
                if (!have) {
                    first = q->controller;
                    have = true;
                } else if (q->controller != first) {
                    all_same = false;
                    break;
                }
            }
            if (have && all_same && first.valid()) s.controller = first;
        });
    }

    // Occupation progress runs once per game day for every alive country;
    // phase_territory is the single caller of phase_occupation.
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) == 0) {
        w.countries.for_each([&](CountryId cid, Country& c) {
            if (c.alive) phase_occupation(g, cid);
        });
    }

    // Capitulation: a country that has lost its capital is checked immediately
    // when the event happens, and at most once a day otherwise.
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive || !c.capital.valid()) return;
        const State* cap = w.state(c.capital);
        if (!cap) return;
        if (cap->controller == cid) return;
        if (mil_same_side(w, cid, cap->controller)) return;

        bool capital_state_changed = false;
        for (ProvinceId pid : changed) {
            const Province* q = w.province(pid);
            if (q && q->state == c.capital) {
                capital_state_changed = true;
                break;
            }
        }
        if (!capital_state_changed && w.tick < c.last_capitulation_check + TICKS_PER_DAY) {
            return;
        }
        c.last_capitulation_check = w.tick;
        check_capitulation(g, cid);
    });
}

}  // namespace hoi
