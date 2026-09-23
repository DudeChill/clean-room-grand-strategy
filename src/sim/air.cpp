// Air warfare (spec sections 51-52; docs/mechanics/air_warfare.md).
//
// Air control is derived, never stored twice: `air_control_share` recomputes it from
// the wings actually flying in a region, and `phase_air` writes the same values into
// Region::air_control (sorted by country id) so observers and land combat can read
// them without walking the wing store. `air_support_modifier` is the land-combat
// reader: it consumes that cache and never needs a wing to exist, which is what makes
// the combat phase independent of the air phase's position in the tick.
//
// Determinism: every store is walked in ascending id, per-region work is bucketed by
// region id, and the only random draws are one RngStream::Combat roll per wing that
// is actually engaged in air combat this hour (documented at the call site). No
// unordered container is read in a gameplay decision and no NaN/Inf is left behind.

#include "sim/air.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/combat.h"

namespace hoi {
namespace {

// Tuning lives in SimConstants ("// Air", src/data/content.h); everything below is
// read from Content so data can retune the model without a recompile.

// Bombing / logistics damage scale against a province defended by anti-air.
double anti_air_defence_factor(const SimConstants& k, int anti_air) {
    if (anti_air <= 0) return 1.0;
    return 1.0 / (1.0 + k.air_anti_air_bombing_reduction * static_cast<double>(anti_air));
}

// Mission weight for air control: missions that contest the airspace count in full,
// ground-attack and reconnaissance missions count half (spec: "CAS/bombers half").
double mission_weight(AirMission m, const SimConstants& k) {
    switch (m) {
        case AirMission::AirSuperiority:
        case AirMission::Interception: return k.air_mission_weight_contested;
        case AirMission::CloseAirSupport:
        case AirMission::StrategicBombing:
        case AirMission::LogisticsStrike:
        case AirMission::Reconnaissance: return k.air_mission_weight_support;
        case AirMission::None:
        case AirMission::Count: return 0.0;
    }
    return 0.0;
}

// 0 = attacker side, 1 = defender side, -1 = not a participant (mirrors diplomacy).
int war_side_of(const War& war, CountryId c) {
    for (const WarParticipant& p : war.attackers) {
        if (p.country == c) return 0;
    }
    for (const WarParticipant& p : war.defenders) {
        if (p.country == c) return 1;
    }
    return -1;
}

// True when a and b are the same country, share a faction, or fight on the same side
// of an active war. This is the "co-belligerent" relation air control groups by.
bool same_side(const World& w, CountryId a, CountryId b) {
    if (!a.valid() || !b.valid()) return false;
    if (a == b) return true;
    if (w.at_war(a, b)) return false;
    const Country* ca = w.country(a);
    const Country* cb = w.country(b);
    if (ca != nullptr && cb != nullptr && ca->faction != 0 && ca->faction == cb->faction) {
        return true;
    }
    bool found = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (found || !war.active) return;
        const int side = war_side_of(war, a);
        if (side < 0) return;
        if (war_side_of(war, b) == side) found = true;
    });
    return found;
}

struct WingStats {
    double air_attack = 0.0;
    double air_defence = 0.0;
    double agility = 0.0;
    double ground_attack = 0.0;
    double max_strength = 1.0;
    bool aircraft = false;
};

// Aircraft statistics. A wing whose model is missing or is not an aircraft is inert
// (no combat, no mission effect) rather than a crash or a source of garbage numbers.
WingStats wing_stats(const Content& content, EquipmentId equipment) {
    WingStats s;
    const EquipmentDef* def = content.equipment_def(equipment);
    if (def == nullptr || def->category != EquipmentCategory::Aircraft) return s;
    s.aircraft = true;
    s.air_attack = std::max(0.0, def->air_attack);
    s.air_defence = std::max(0.0, def->air_defence);
    s.agility = std::max(0.0, def->agility);
    s.ground_attack = std::max(0.0, def->ground_attack);
    s.max_strength = def->max_strength > 0.0 ? def->max_strength : 1.0;
    return s;
}

// A wing flies in its mission region when it has aircraft, a mission, a live region
// and a base it may operate from (its own or a co-belligerent's). Range is checked
// when the mission is set (wing_can_reach), not per tick: the spec rejects out-of-
// range regions at command time.
bool wing_flying(const Game& g, const AirWing& x) {
    if (x.planes <= 0) return false;
    if (x.mission == AirMission::None) return false;
    if (!x.region.valid() || !g.world.regions.alive(x.region)) return false;
    if (!x.base.valid() || g.world.province(x.base) == nullptr) return false;
    return g.world.has_access(x.country, x.base);
}

// What one country flies in one region this hour. Doubles are summed in ascending
// wing id (callers walk the bucket in id order), so the result is reproducible.
struct RegionCountryAgg {
    CountryId country;
    double weight = 0.0;            // control weight (mission-weighted)
    double planes = 0.0;            // planes * efficiency
    double defence = 0.0;           // planes * efficiency * air_defence
    double agility = 0.0;           // planes * efficiency * agility
    double cas_ground = 0.0;        // CAS ground-attack power
    double strategic_power = 0.0;   // strategic-bombing power
    double logistics_power = 0.0;   // logistics-strike power
    int anti_air = 0;               // highest anti-air level this country holds here
};

RegionCountryAgg& find_agg(std::vector<RegionCountryAgg>& aggs, CountryId country) {
    for (RegionCountryAgg& a : aggs) {
        if (a.country == country) return a;
    }
    aggs.push_back(RegionCountryAgg{});
    aggs.back().country = country;
    return aggs.back();
}

void add_wing_to_aggs(const Game& g, const AirWing& x, std::vector<RegionCountryAgg>& aggs) {
    const double planes_eff = static_cast<double>(x.planes) * clamp01(x.efficiency);
    if (!(planes_eff > 0.0)) return;
    const WingStats st = wing_stats(g.content, x.equipment);
    RegionCountryAgg& a = find_agg(aggs, x.country);
    a.weight += planes_eff * mission_weight(x.mission, g.content.constants);
    a.planes += planes_eff;
    a.defence += planes_eff * st.air_defence;
    a.agility += planes_eff * st.agility;
    if (x.mission == AirMission::CloseAirSupport) a.cas_ground += planes_eff * st.ground_attack;
    if (x.mission == AirMission::StrategicBombing) {
        a.strategic_power += planes_eff * st.ground_attack;
    }
    if (x.mission == AirMission::LogisticsStrike) {
        a.logistics_power += planes_eff * st.ground_attack;
    }
}

// Recomputes one region's country aggregates from the wing store (ascending id).
void collect_region_countries(const Game& g, RegionId region, std::vector<RegionCountryAgg>* out) {
    out->clear();
    g.world.air_wings.for_each([&](AirWingId, const AirWing& x) {
        if (!wing_flying(g, x) || x.region != region) return;
        add_wing_to_aggs(g, x, *out);
    });
}

// Nearest air base province that can host `planes` more aircraft for `country`,
// measured in land hops from `from`. Returns INVALID when none exists. `occupancy`
// is indexed by province slot and is updated by the caller as wings are moved.
ProvinceId nearest_displacement_base(const Game& g, CountryId country, ProvinceId from, int planes,
                                     double capacity_per_level, const std::vector<int>& occupancy) {
    const World& w = g.world;
    const size_t n = w.provinces.capacity();
    auto viable = [&](const Province& p, ProvinceId pid) {
        if (p.is_sea || p.air_base <= 0) return false;
        if (!w.has_access(country, pid)) return false;
        const int used = pid.v < occupancy.size() ? occupancy[pid.v] : 0;
        return used + planes <= p.air_base * capacity_per_level;
    };

    const Province* src = w.province(from);
    if (src == nullptr || src->is_sea || from.v >= n) {
        // No usable origin: the nearest base is simply the lowest-id viable one.
        ProvinceId best;
        w.provinces.for_each([&](ProvinceId id, const Province& p) {
            if (!viable(p, id)) return;
            if (!best.valid() || id.v < best.v) best = id;
        });
        return best;
    }

    std::vector<int32_t> dist(n, -1);
    std::vector<ProvinceId> queue;
    dist[from.v] = 0;
    queue.push_back(from);
    ProvinceId best;
    int32_t best_dist = -1;
    for (size_t head = 0; head < queue.size(); ++head) {
        const ProvinceId cur = queue[head];
        const int32_t d = dist[cur.v];
        if (best.valid() && best_dist >= 0 && d > best_dist) break;
        const Province* cp = w.province(cur);
        if (cp == nullptr) continue;
        if (viable(*cp, cur) && (!best.valid() || d < best_dist ||
                                 (d == best_dist && cur.v < best.v))) {
            best = cur;
            best_dist = d;
        }
        for (ProvinceId nx : cp->adj) {
            if (!nx.valid() || nx.v >= n || dist[nx.v] >= 0) continue;
            const Province* np = w.province(nx);
            if (np == nullptr || np->is_sea) continue;
            dist[nx.v] = d + 1;
            queue.push_back(nx);
        }
    }
    return best;
}

}  // namespace

// --------------------------------------------------------------- queries ----

int air_base_capacity(const Game& g, ProvinceId province) {
    const Province* p = g.world.province(province);
    if (p == nullptr || p->is_sea || p->air_base <= 0) return 0;
    const double per_level = g.content.constants.air_base_capacity_per_level;
    if (!(per_level > 0.0)) return 0;
    return static_cast<int>(p->air_base * per_level);
}

int planes_stationed_at(const Game& g, ProvinceId province) {
    if (!province.valid()) return 0;
    int total = 0;
    g.world.air_wings.for_each([&](AirWingId, const AirWing& x) {
        if (x.base == province && x.planes > 0) total += x.planes;
    });
    return total;
}

int region_distance_hops(const Game& g, RegionId from, RegionId to) {
    if (!from.valid() || !to.valid()) return -1;
    if (from == to) return 0;
    const Region* rf = g.world.regions.try_get(from);
    const Region* rt = g.world.regions.try_get(to);
    if (rf == nullptr || rt == nullptr) return -1;

    const World& w = g.world;
    const size_t n = w.provinces.capacity();
    std::vector<int32_t> dist(n, -1);
    std::vector<ProvinceId> queue;
    queue.reserve(rf->provinces.size());
    for (ProvinceId pid : rf->provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr || p->is_sea || pid.v >= n || dist[pid.v] >= 0) continue;
        dist[pid.v] = 0;
        queue.push_back(pid);
    }
    // Breadth-first over the land graph; the first province reached inside `to` (from
    // any source province in `from`) gives the shortest region-to-region distance.
    for (size_t head = 0; head < queue.size(); ++head) {
        const ProvinceId cur = queue[head];
        const int32_t d = dist[cur.v];
        const Province* cp = w.province(cur);
        if (cp == nullptr) continue;
        if (cp->region == to) return d;
        for (ProvinceId nx : cp->adj) {
            if (!nx.valid() || nx.v >= n || dist[nx.v] >= 0) continue;
            const Province* np = w.province(nx);
            if (np == nullptr || np->is_sea) continue;
            dist[nx.v] = d + 1;
            queue.push_back(nx);
        }
    }
    return -1;
}

bool wing_can_reach(const Game& g, const AirWing& wing, RegionId region) {
    if (!region.valid() || !g.world.regions.alive(region)) return false;
    const Province* base = g.world.province(wing.base);
    if (base == nullptr || base->is_sea) return false;
    if (!g.world.has_access(wing.country, wing.base)) return false;
    if (base->region == region) return true;  // own region is always in range
    const EquipmentDef* def = g.content.equipment_def(wing.equipment);
    const double range = def != nullptr ? def->range : 0.0;
    if (!(range > 0.0)) return false;  // 0 or negative: own region only
    const int hops = region_distance_hops(g, base->region, region);
    if (hops < 0) return false;
    return static_cast<double>(hops) <= range;
}

double air_control_share(const Game& g, CountryId country, RegionId region) {
    if (!country.valid() || !region.valid() || !g.world.regions.alive(region)) return 0.0;
    std::vector<RegionCountryAgg> aggs;
    collect_region_countries(g, region, &aggs);
    double total = 0.0;
    double own = 0.0;
    for (const RegionCountryAgg& a : aggs) {
        total += a.weight;
        if (a.country == country) own = a.weight;
    }
    return clamp01(safe_div(own, total));
}

double air_support_modifier(const Game& g, CountryId country, ProvinceId province, bool attacker) {
    if (!country.valid()) return 0.0;
    const Province* p = g.world.province(province);
    if (p == nullptr || !p->region.valid()) return 0.0;
    const Region* region = g.world.regions.try_get(p->region);
    if (region == nullptr) return 0.0;

    // Friendly and hostile control shares, grouped by side. Written by phase_air; a
    // region nobody contests has no entry and therefore no modifier.
    double friendly = 0.0;
    double hostile = 0.0;
    for (const auto& entry : region->air_control) {
        const double share = clamp01(entry.second);
        if (!(share > 0.0)) continue;
        if (same_side(g.world, country, entry.first)) {
            friendly += share;
        } else if (g.world.at_war(country, entry.first)) {
            hostile += share;
        }
    }
    if (!(friendly > 0.0) && !(hostile > 0.0)) return 0.0;

    // Both sides get a superiority term; only the attacker gets the CAS term. The
    // spec's cas_planes/(cas_planes + enemy_interceptors) is already folded into the
    // control shares, because CAS missions weigh half and interceptors weigh in full:
    // full friendly control is +air_superiority_effect, no control is the same value
    // negative, and the attacker adds up to air_cas_effect on top.
    const double share = clamp01(safe_div(friendly, friendly + hostile));
    const SimConstants& k = g.content.constants;
    double mod = k.air_superiority_effect * (2.0 * share - 1.0);
    if (attacker) mod += k.air_cas_effect * share;
    return clamp(mod, k.air_support_min_modifier, k.air_support_max_modifier);
}

// ----------------------------------------------------------- maintenance ----

int reinforce_air_wing(Game& g, AirWing& wing) {
    if (wing.max_planes <= 0 || wing.planes >= wing.max_planes) return 0;
    Country* country = g.world.country(wing.country);
    if (country == nullptr) return 0;
    if (!wing.equipment.valid() || wing.equipment.v >= country->equipment_stockpile.size()) {
        return 0;
    }
    if (g.content.equipment_def(wing.equipment) == nullptr) return 0;
    double& stock = country->equipment_stockpile[wing.equipment.v];
    if (!(stock > 0.0)) return 0;
    const int need = wing.max_planes - wing.planes;
    const int delivered = static_cast<int>(
        std::min<double>(static_cast<double>(need), std::floor(stock)));
    if (delivered <= 0) return 0;
    stock -= delivered;
    wing.planes += delivered;
    return delivered;
}

void disband_air_wing(Game& g, AirWingId id) {
    AirWing* wing = g.world.wing(id);
    if (wing == nullptr) return;
    Country* country = g.world.country(wing->country);
    if (country != nullptr && wing->planes > 0 && wing->equipment.valid() &&
        wing->equipment.v < country->equipment_stockpile.size()) {
        country->equipment_stockpile[wing->equipment.v] += wing->planes;
    }
    if (country != nullptr) {
        std::vector<AirWingId>& wings = country->wings;
        wings.erase(std::remove(wings.begin(), wings.end(), id), wings.end());
    }
    g.world.air_wings.destroy(id);
}

// --------------------------------------------------------------- phase ------

void phase_air(Game& g) {
    World& w = g.world;
    Rng& rng = g.rng.get(RngStream::Combat);
    const size_t region_slots = w.regions.capacity();
    const SimConstants& constants = g.content.constants;

    // Bucket flying wings by mission region (ascending id within each bucket) and
    // remember every wing for the maintenance passes below.
    std::vector<AirWingId> all;
    std::vector<std::vector<AirWingId>> by_region(region_slots);
    w.air_wings.for_each([&](AirWingId id, const AirWing& x) {
        all.push_back(id);
        if (!wing_flying(g, x) || x.region.v >= region_slots) return;
        by_region[x.region.v].push_back(id);
    });

    // Battles per region so CAS never scans the world once per region.
    std::vector<std::vector<BattleId>> battles_by_region(region_slots);
    w.battles.for_each([&](BattleId id, const Battle& b) {
        const Province* bp = w.province(b.province);
        if (bp == nullptr || !bp->region.valid() || bp->region.v >= region_slots) return;
        battles_by_region[bp->region.v].push_back(id);
    });

    // Per-wing losses of this hour, applied after every wing has fought so the hour
    // is simultaneous (a wing's own losses never change what it inflicts).
    std::vector<int> loss(w.air_wings.capacity(), 0);

    for (size_t ri = 0; ri < region_slots; ++ri) {
        const std::vector<AirWingId>& ids = by_region[ri];
        if (ids.empty()) continue;
        const RegionId region(static_cast<uint32_t>(ri));
        const Region* reg = w.regions.try_get(region);
        if (reg == nullptr) continue;

        std::vector<RegionCountryAgg> aggs;
        for (AirWingId id : ids) {
            AirWing* x = w.wing(id);
            if (x == nullptr) continue;
            add_wing_to_aggs(g, *x, aggs);
            x->experience = clamp01(x->experience + constants.air_experience_per_mission_hour);
        }
        // Anti-air is a province defence, not a wing: fold the highest level each
        // country holds in the region into its aggregate, so air combat and the
        // strike missions can both find the defender's flak without a second scan.
        for (ProvinceId pid : reg->provinces) {
            const Province* q = w.province(pid);
            if (q == nullptr || q->is_sea || q->anti_air <= 0) continue;
            if (!q->controller.valid()) continue;
            RegionCountryAgg& defender = find_agg(aggs, q->controller);
            defender.anti_air = std::max(defender.anti_air, q->anti_air);
        }

        // Air combat: each wing engages the hostile coalition present in the region.
        // One RngStream::Combat draw per engaged wing this hour — engaged means the
        // wing has aircraft, an aircraft model, and at least one hostile wing flies
        // here; nothing else consumes a draw, so the sequence is reproducible.
        for (AirWingId id : ids) {
            AirWing* x = w.wing(id);
            if (x == nullptr || x->planes <= 0) continue;
            const WingStats st = wing_stats(g.content, x->equipment);
            if (!st.aircraft) continue;
            const double planes_eff = static_cast<double>(x->planes) * clamp01(x->efficiency);
            if (!(planes_eff > 0.0)) continue;

            double enemy_planes = 0.0;
            double enemy_defence = 0.0;
            double enemy_agility = 0.0;
            int enemy_anti_air = 0;
            for (const RegionCountryAgg& a : aggs) {
                if (a.country == x->country) continue;
                if (!w.at_war(x->country, a.country)) continue;
                enemy_planes += a.planes;
                enemy_defence += a.defence;
                enemy_agility += a.agility;
                enemy_anti_air = std::max(enemy_anti_air, a.anti_air);
            }
            if (!(enemy_planes > 0.0)) continue;

            const double avg_defence = enemy_defence / enemy_planes;
            const double avg_agility = enemy_agility / enemy_planes;
            const double agility_mult =
                1.0 + constants.air_agility_weight * (st.agility - avg_agility) /
                          (st.agility + avg_agility + 1.0);
            const double sorties = planes_eff / constants.air_sortie_hours;  // spec sortie_rate
            const double attack_power = sorties * st.air_attack * agility_mult;
            const double defence_factor = clamp01(
                1.0 - safe_div(avg_defence,
                               avg_defence + constants.air_combat_defence_floor + st.air_attack));
            const double roll =
                constants.air_combat_roll_base + rng.next_double();  // documented draw
            const double damage =
                attack_power * defence_factor * roll * constants.air_combat_scale;
            const double durability =
                constants.air_aircraft_durability + st.air_defence + st.max_strength;
            // Flying over defended ground costs the attacker aircraft in addition to
            // the fighter opposition: the highest hostile anti-air level in the region.
            const double flak_factor =
                1.0 + constants.air_anti_air_combat_loss_factor *
                          static_cast<double>(enemy_anti_air);
            int lost = static_cast<int>(
                std::floor(safe_div(damage, durability) * flak_factor + 0.5));
            if (lost < 0) lost = 0;
            if (lost > x->planes) lost = x->planes;
            loss[id.v] = lost;
            x->experience = clamp01(x->experience + constants.air_experience_per_combat_hour);
        }

        for (AirWingId id : ids) {
            AirWing* x = w.wing(id);
            if (x == nullptr) continue;
            const int lost = loss[id.v];
            if (lost <= 0) continue;
            x->planes -= lost;
            x->losses += lost;
        }

        // CAS: friendly close-air-support wings damage the enemy divisions in this
        // region's battles. The side least friendly to the wing takes the damage.
        for (const RegionCountryAgg& a : aggs) {
            if (!(a.cas_ground > 0.0)) continue;
            for (BattleId bid : battles_by_region[ri]) {
                Battle* b = w.battle(bid);
                if (b == nullptr) continue;
                const bool with_attacker = same_side(w, a.country, b->attacker_lead) &&
                                           !same_side(w, a.country, b->defender_lead);
                const bool with_defender = same_side(w, a.country, b->defender_lead) &&
                                           !same_side(w, a.country, b->attacker_lead);
                if (!with_attacker && !with_defender) continue;
                const std::vector<DivisionId>& targets =
                    with_attacker ? b->defender.divisions : b->attacker.divisions;
                if (targets.empty()) continue;
                const double per_division = a.cas_ground * constants.air_cas_effect /
                                            static_cast<double>(targets.size());
                for (DivisionId did : targets) {
                    Division* d = w.division(did);
                    if (d == nullptr) continue;
                    apply_combat_losses(g, *d,
                                        constants.air_cas_organisation_damage * per_division,
                                        constants.air_cas_strength_damage * per_division);
                }
            }
        }

        // Strategic bombing: the most valuable enemy state in the region loses
        // factories, floors at 0, and logs an event. The target province's anti-air
        // level cuts the damage.
        for (const RegionCountryAgg& a : aggs) {
            if (!(a.strategic_power >= constants.air_bombing_power_unit)) continue;
            State* target = nullptr;
            const Province* target_province = nullptr;
            int best_rank = -1;
            for (ProvinceId pid : reg->provinces) {
                const Province* q = w.province(pid);
                if (q == nullptr || q->is_sea) continue;
                if (!w.at_war(a.country, q->controller)) continue;
                State* st = w.state(q->state);
                if (st == nullptr) continue;
                const int rank = st->total_factories();
                if (rank > best_rank ||
                    (rank == best_rank && target != nullptr && st->id.v < target->id.v)) {
                    target = st;
                    target_province = q;
                    best_rank = rank;
                }
            }
            if (target == nullptr || target_province == nullptr || best_rank <= 0) continue;
            const int anti_air = target_province->anti_air;
            const int hits = static_cast<int>(std::floor(
                a.strategic_power * constants.air_bombing_industry_damage *
                anti_air_defence_factor(constants, anti_air) / constants.air_bombing_power_unit));
            if (hits < 1) continue;  // anti-air absorbed the strike
            int applied = 0;
            for (int i = 0; i < hits; ++i) {
                if (target->civilian_factories > 0) {
                    --target->civilian_factories;
                } else if (target->military_factories > 0) {
                    --target->military_factories;
                } else if (target->dockyards > 0) {
                    --target->dockyards;
                } else {
                    break;
                }
                ++applied;
            }
            if (applied <= 0) continue;
            g.log_event("air", "strategic bombing damaged " + target->name, a.country);
        }

        // Logistics strike: the best enemy railway in the region loses levels, floors
        // at 0. Supply capacity follows from the rail level next supply phase.
        for (const RegionCountryAgg& a : aggs) {
            if (!(a.logistics_power >= constants.air_logistics_power_unit)) continue;
            Province* target = nullptr;
            int best_level = 0;
            for (ProvinceId pid : reg->provinces) {
                Province* q = w.province(pid);
                if (q == nullptr || q->is_sea || q->railway_level <= 0) continue;
                if (!w.at_war(a.country, q->controller)) continue;
                if (q->railway_level > best_level ||
                    (q->railway_level == best_level && target != nullptr &&
                     q->id.v < target->id.v)) {
                    target = q;
                    best_level = q->railway_level;
                }
            }
            if (target == nullptr) continue;
            const int hits = static_cast<int>(std::floor(
                a.logistics_power * constants.air_logistics_strike_damage *
                anti_air_defence_factor(constants, target->anti_air) /
                constants.air_logistics_power_unit));
            if (hits < 1) continue;
            target->railway_level = std::max(0, target->railway_level - hits);
            g.log_event("air", "logistics strike cut railways at " + target->name, a.country);
        }

        // Reconnaissance has no effect while there is no fog-of-war model to reveal
        // into; the mission is still legal and still flies.
    }

    // Displacement: a wing whose base is no longer controlled moves to the nearest
    // friendly air base with spare capacity, or is destroyed (planes back to stock).
    std::vector<int> occupancy;
    std::vector<AirWingId> destroyed;
    for (AirWingId id : all) {
        AirWing* x = w.wing(id);
        if (x == nullptr) continue;
        const Province* base = w.province(x->base);
        if (base != nullptr && !base->is_sea && w.has_access(x->country, x->base)) continue;
        if (occupancy.empty()) {
            occupancy.assign(w.provinces.capacity(), 0);
            w.air_wings.for_each([&](AirWingId, const AirWing& y) {
                if (y.base.valid() && y.base.v < occupancy.size()) occupancy[y.base.v] += y.planes;
            });
        }
        const ProvinceId nb = nearest_displacement_base(
            g, x->country, x->base, x->planes, constants.air_base_capacity_per_level, occupancy);
        if (!nb.valid()) {
            destroyed.push_back(id);
            continue;
        }
        x->base = nb;
        const Province* np = w.province(nb);
        if (np != nullptr) {
            x->region = np->region;
            if (nb.v < occupancy.size()) occupancy[nb.v] += x->planes;
            g.log_event("air", "wing '" + x->name + "' displaced to " + np->name, x->country);
        }
    }
    for (AirWingId id : destroyed) disband_air_wing(g, id);

    // Replacements draw from the country's stockpile; the industry phase, which runs
    // next, refills it from production.
    for (AirWingId id : all) {
        AirWing* x = w.wing(id);
        if (x == nullptr) continue;
        reinforce_air_wing(g, *x);
    }

    // Zero-plane wings are removed (spec edge case). AirWing carries no manpower
    // field, so there is no personnel roster to return to the country pool.
    std::vector<AirWingId> empty_wings;
    w.air_wings.for_each([&](AirWingId id, const AirWing& x) {
        if (x.planes <= 0) empty_wings.push_back(id);
    });
    for (AirWingId id : empty_wings) disband_air_wing(g, id);

    // Air control: one pass over the wing store builds per-region country weights,
    // then each region's shares are written sorted by country id. This runs after
    // displacement so the cache describes where the wings actually are.
    w.regions.for_each([](RegionId, Region& r) { r.air_control.clear(); });
    std::vector<std::vector<std::pair<CountryId, double>>> weights(region_slots);
    w.air_wings.for_each([&](AirWingId, const AirWing& x) {
        if (!wing_flying(g, x) || x.region.v >= region_slots) return;
        const double weight = static_cast<double>(x.planes) * clamp01(x.efficiency) *
                              mission_weight(x.mission, constants);
        if (!(weight > 0.0)) return;
        std::vector<std::pair<CountryId, double>>& list = weights[x.region.v];
        for (auto& entry : list) {
            if (entry.first == x.country) {
                entry.second += weight;
                return;
            }
        }
        list.emplace_back(x.country, weight);
    });
    for (size_t ri = 0; ri < region_slots; ++ri) {
        std::vector<std::pair<CountryId, double>>& list = weights[ri];
        if (list.empty()) continue;
        Region* reg = w.regions.try_get(RegionId(static_cast<uint32_t>(ri)));
        if (reg == nullptr) continue;
        double total = 0.0;
        for (const auto& entry : list) total += entry.second;
        if (!(total > 0.0)) continue;
        std::sort(list.begin(), list.end(), [](const std::pair<CountryId, double>& a,
                                               const std::pair<CountryId, double>& b) {
            return a.first.v < b.first.v;
        });
        for (const auto& entry : list) {
            const double share = clamp01(safe_div(entry.second, total));
            if (share > 0.0) reg->air_control.emplace_back(entry.first, share);
        }
    }

    // Next hour's sortie efficiency: how full the wing is, cut by enemy air control.
    // The one-hour lag (efficiency is read before control is recomputed) is inherent
    // to the spec's circular formula and keeps the phase deterministic and O(wings).
    w.air_wings.for_each([&](AirWingId, AirWing& x) {
        if (!wing_flying(g, x)) return;
        const Region* reg = w.regions.try_get(x.region);
        if (reg == nullptr) return;
        double friendly = 0.0;
        for (const auto& entry : reg->air_control) {
            if (same_side(w, x.country, entry.first)) friendly += entry.second;
        }
        friendly = clamp01(friendly);
        const double filled =
            x.max_planes > 0 ? clamp01(static_cast<double>(x.planes) / x.max_planes) : 0.0;
        x.efficiency = clamp01(filled * (1.0 - 0.5 * (1.0 - friendly)));
    });
}

}  // namespace hoi