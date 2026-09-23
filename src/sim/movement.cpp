// Division movement across the province graph (ARCHITECTURE section 5.4).
//
// Edge time is computed from the target province's terrain, its region's weather,
// a river-crossing proxy and the target's infrastructure:
//
//   hours_per_edge = base_hours_per_province / max(min_speed, division_speed)
//                    * terrain_cost(target) * weather_cost(region)
//                    * (1 + river_crossing_penalty if crossing)
//                    / (1 + infrastructure_bonus * infrastructure)
//
// move_progress advances by 1/hours_per_edge each hour; the division arrives at
// >= 1.0. A division with supply below 0.2 moves at half speed.
//
// The frozen Province struct has no river flag, so entering a marsh province is
// used as the documented river-crossing proxy (see the military report).

#include "sim/phases.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/math.h"
#include "game/game.h"
#include "sim/combat.h"
#include "sim/map.h"
#include "sim/politics.h"
#include "sim/world.h"

namespace hoi {

// War-side helpers shared by the military phases; implemented in combat.cpp so
// that the war-participant scan lives in exactly one place.
bool mil_at_war(const World& w, CountryId a, CountryId b);
bool mil_same_side(const World& w, CountryId a, CountryId b);

namespace {

constexpr double kUnsuppliedSpeedFactor = 2.0;  // half speed below 0.2 supply
constexpr double kMarshRiverProxy = 1.0;        // marsh counts as a crossing

double terrain_move_cost_table(Terrain t) {
    switch (t) {
        case Terrain::Plains: return 1.0;
        case Terrain::Forest: return 1.5;
        case Terrain::Hills: return 1.6;
        case Terrain::Mountain: return 3.0;
        case Terrain::Urban: return 1.4;
        case Terrain::Marsh: return 2.0;
        case Terrain::Desert: return 1.1;
        case Terrain::Jungle: return 2.5;
        default: return 1.0;
    }
}

// Weather movement multipliers are shared with the other systems and live in
// politics.h (implemented in weather.cpp).

bool local_adjacent(const World& w, ProvinceId a, ProvinceId b) {
    const Province* pa = w.province(a);
    if (!pa) return false;
    for (ProvinceId q : pa->adj) {
        if (q == b) return true;
    }
    const Province* pb = w.province(b);
    if (pb) {
        for (ProvinceId q : pb->adj) {
            if (q == a) return true;
        }
    }
    return false;
}

bool province_has_hostile(const World& w, ProvinceId p, CountryId c) {
    bool found = false;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (found || !d.location.valid()) return;
        if (d.location == p && mil_at_war(w, d.country, c)) found = true;
    });
    return found;
}

void cancel_movement(Division& d) {
    d.path.clear();
    d.moving = false;
    d.move_from = ProvinceId{};
    d.move_to = ProvinceId{};
    d.move_progress = 0.0;
}

}  // namespace

void phase_movement(Game& g) {
    World& w = g.world;
    const SimConstants& k = g.content.constants;

    w.divisions.for_each([&](DivisionId, Division& d) {
        if (!d.location.valid()) return;  // off-map (training)
        if (d.in_combat()) return;        // committed to a battle

        if (d.path.empty()) {
            if (d.moving) {
                d.moving = false;
                d.move_from = ProvinceId{};
                d.move_to = ProvinceId{};
                d.move_progress = 0.0;
            }
            if (d.retreating) d.retreating = false;  // relocation already applied
            return;
        }

        if (!w.province(d.location)) {
            cancel_movement(d);
            return;
        }

        ProvinceId target = d.path[0];
        if (!w.province(target)) {
            cancel_movement(d);
            return;
        }

        // Path legality is re-checked every tick. An illegal hop (a province the
        // graph no longer connects) is recomputed toward the final destination,
        // or the move is cancelled when no route exists.
        if (!local_adjacent(w, d.location, target)) {
            PathRequest req;
            req.country = d.country;
            req.allow_hostile = true;
            req.require_controlled = false;
            std::vector<ProvinceId> np = find_path(w, d.location, d.path.back(), req);
            if (np.empty()) {
                cancel_movement(d);
                return;
            }
            d.path = std::move(np);
            target = d.path[0];
            if (!w.province(target)) {
                cancel_movement(d);
                return;
            }
        }

        d.moving = true;
        d.move_from = d.location;
        d.move_to = target;

        const Province* tp = w.province(target);

        // Hostile troops in the target block the crossing: the division halts at
        // the edge and the combat phase opens a battle for it instead.
        if (province_has_hostile(w, target, d.country)) {
            d.order_target = target;
            d.move_progress = 0.0;
            return;
        }

        // Entering territory controlled by someone else requires either an
        // offensive order or an undefended province of a country we are at war
        // with (ARCHITECTURE 5.4).
        if (!mil_same_side(w, d.country, tp->controller)) {
            const bool at_war = mil_at_war(w, d.country, tp->controller);
            if (d.order != OrderKind::Offensive && !at_war) {
                cancel_movement(d);
                return;
            }
        }

        DivisionStats st = compute_division_stats(g, d);
        double speed = st.speed;
        if (!(speed > 0.0) || !std::isfinite(speed)) speed = k.min_division_speed;
        speed = std::max(k.min_division_speed, speed);

        double hours = k.base_hours_per_province / speed;
        hours *= terrain_move_cost_table(tp->terrain);
        const Region* reg = w.regions.try_get(tp->region);
        if (reg) hours *= weather_movement_multiplier(*reg);
        if (tp->terrain == Terrain::Marsh) {
            hours *= (1.0 + k.river_crossing_penalty * kMarshRiverProxy);
        }
        hours /= (1.0 + k.supply_infrastructure_bonus_per_level *
                           static_cast<double>(tp->infrastructure));
        if (clamp01(d.supply) < 0.2) hours *= kUnsuppliedSpeedFactor;

        if (!(hours > 0.0) || !std::isfinite(hours)) hours = k.base_hours_per_province;

        d.move_progress += 1.0 / hours;
        if (!std::isfinite(d.move_progress)) d.move_progress = 1.0;

        if (d.move_progress < 1.0) return;

        // Arrival.
        d.previous_location = d.location;
        d.location = target;
        d.move_progress = 0.0;
        d.retreating = false;
        d.path.erase(d.path.begin());
        if (d.path.empty()) {
            d.moving = false;
            d.move_from = ProvinceId{};
            d.move_to = ProvinceId{};
            if (d.order_target == d.location) d.order_target = ProvinceId{};
        } else {
            d.move_from = d.location;
            d.move_to = d.path[0];
        }
    });
}

}  // namespace hoi
