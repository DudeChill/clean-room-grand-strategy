// Naval warfare (spec sections 53-55; docs/mechanics/naval_warfare.md).
//
// Naval control is derived, never stored twice: `naval_control_share` recomputes it
// from the task forces actually at sea in a zone, and `phase_naval` writes the same
// values into Region::naval_control (sorted by country id) so observers and the AI
// can read them without walking the ship store. Ships are individual entities
// (individual damage, individual sinking, individual names); task forces are the
// manoeuvre unit; fleets are administrative rosters only.
//
// Determinism: every store is walked in ascending id, per-zone work is bucketed by
// region id, and the only random draws are one RngStream::Combat roll per hostile
// task-force pair (mutual detection) and one per task force that actually engages
// (the combat roll), both documented at their call sites. No unordered container is
// read in a gameplay decision and no NaN/Inf is left behind.
//
// The invasion lifecycle (loading, crossing, interception, landing) is a separate
// module; phase_naval only sequences it.

#include "sim/navy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"

namespace hoi {
namespace {

// ------------------------------------------------------- model helpers ------

// A ship's equipment definition, or null for a ship whose model is missing from
// content. A null model makes the ship inert (no power, no damage taken) rather than
// a crash or a source of garbage numbers.
const EquipmentDef* ship_def(const Content& content, const Ship& s) {
    return content.equipment_def(s.equipment);
}

// Missions with combat intent: only these task forces take part in detection and
// engagement. None means "no orders"; Training is a safe rear-area mission and
// neither initiates nor receives combat (spec: "training, no combat intent").
bool combat_mission(NavalMission m) {
    switch (m) {
        case NavalMission::Patrol:
        case NavalMission::StrikeForce:
        case NavalMission::ConvoyEscort:
        case NavalMission::ConvoyRaid:
        case NavalMission::InvasionSupport: return true;
        case NavalMission::None:
        case NavalMission::Training:
        case NavalMission::Count: return false;
    }
    return false;
}

// A "large hull" is a capital ship or carrier: big enough that torpedoes get their
// bonus against it and destroyers can screen it. Sized from content stats (the
// spec's hull classes are data), never from a hardcoded class list.
bool large_hull(const EquipmentDef* def, const SimConstants& k) {
    if (def == nullptr) return false;
    return std::max(0.0, def->max_strength) >= k.naval_large_hull_hp;
}

// Presence weight of one ship in a sea zone: the sum of its combat statistics scaled
// by how much of the hull is still afloat. The mission gates participation, so ships
// with no orders contest nothing.
double ship_zone_power(const EquipmentDef* def, double strength) {
    if (def == nullptr) return 0.0;
    const double power = std::max(0.0, def->naval_attack) +
                         std::max(0.0, def->torpedo_attack) +
                         std::max(0.0, def->air_attack) +
                         std::max(0.0, def->ground_attack) +  // carrier air group
                         std::max(0.0, def->sub_detection);
    return clamp01(strength) * power;
}

double ship_detection_power(const EquipmentDef* def, double strength) {
    if (def == nullptr) return 0.0;
    return clamp01(strength) * std::max(0.0, def->detection);
}

double ship_visibility_power(const EquipmentDef* def, double strength) {
    if (def == nullptr) return 0.0;
    return clamp01(strength) * std::max(0.0, def->visibility);
}

// Hull size used to spread incoming damage across a formation and to convert attack
// power into a fraction of the hull.
double hull_size(const EquipmentDef* def) {
    if (def == nullptr) return 1.0;
    return std::max(1.0, def->max_strength);
}

// ------------------------------------------------------- zone presence ------

struct ZoneWeight {
    CountryId country;
    double weight = 0.0;  // total presence weight of this country in the zone
    double raid = 0.0;    // presence weight contributed by its convoy raiders
};

ZoneWeight& zone_weight_for(std::vector<ZoneWeight>& list, CountryId c) {
    for (ZoneWeight& z : list) {
        if (z.country == c) return z;
    }
    list.push_back(ZoneWeight{});
    list.back().country = c;
    return list.back();
}

// Collective presence weights of every country at sea in `region`, walked in
// ascending task-force id. Pure query: nothing is mutated, so naval_control_share
// and the phase's control cache read exactly the same numbers.
void zone_weights(const Game& g, RegionId region, std::vector<ZoneWeight>* out) {
    out->clear();
    const World& w = g.world;
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (!tf.at_sea || tf.sea_region != region || tf.ships.empty()) return;
        if (tf.mission == NavalMission::None || tf.mission == NavalMission::Training) return;
        double power = 0.0;
        for (ShipId sid : tf.ships) {
            const Ship* s = w.ship(sid);
            if (s == nullptr) continue;
            power += ship_zone_power(ship_def(g.content, *s), s->strength);
        }
        if (!(power > 0.0)) return;
        ZoneWeight& z = zone_weight_for(*out, tf.country);
        z.weight += power;
        if (tf.mission == NavalMission::ConvoyRaid) z.raid += power;
    });
    // Raiders cut everyone else's claim on the zone: the pressure a country suffers
    // is the hostile raiding presence relative to its own presence (spec: "convoy
    // raiding ... cuts enemy naval control in the zone").
    for (ZoneWeight& z : *out) {
        if (!z.country.valid()) continue;
        double hostile_raid = 0.0;
        for (const ZoneWeight& o : *out) {
            if (o.country == z.country || !o.country.valid()) continue;
            if (w.at_war(z.country, o.country)) hostile_raid += o.raid;
        }
        if (!(hostile_raid > 0.0)) continue;
        const double pressure = clamp01(safe_div(hostile_raid, hostile_raid + z.weight));
        z.weight *= clamp01(1.0 - g.content.constants.naval_raid_control_cut * pressure);
    }
}

// The lowest-id convoy model in content, the pool raiders sink. INVALID when no
// convoy model exists, in which case raiding damages nothing.
EquipmentId convoy_model(const Content& c) {
    EquipmentId best;
    for (size_t i = 0; i < c.equipment.size(); ++i) {
        const EquipmentDef& d = c.equipment[i];
        if (d.category != EquipmentCategory::Convoy) continue;
        const EquipmentId id(static_cast<uint32_t>(i));
        if (!best.valid() || id.v < best.v) best = id;
    }
    return best;
}

// Nearest friendly port to `from` in land hops, used when a task force's base port
// falls. Ascending province id breaks ties so two runs pick the same harbour.
ProvinceId nearest_friendly_port(const Game& g, CountryId country, ProvinceId from) {
    const World& w = g.world;
    const size_t n = w.provinces.capacity();
    auto viable = [&](const Province& p, ProvinceId pid) {
        return !p.is_sea && p.naval_base > 0 && !p.sea_adj.empty() &&
               w.has_access(country, pid);
    };

    const Province* src = w.province(from);
    if (src == nullptr || src->is_sea || from.v >= n) {
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
        if (best.valid() && d > best_dist) break;
        const Province* cp = w.province(cur);
        if (cp == nullptr) continue;
        if (viable(*cp, cur) &&
            (!best.valid() || d < best_dist || (d == best_dist && cur.v < best.v))) {
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

// Ships currently berthed at a province (they count against its base capacity).
int ships_home_at(const Game& g, ProvinceId port) {
    int n = 0;
    g.world.ships.for_each([&](ShipId, const Ship& s) {
        if (s.port == port) ++n;
    });
    return n;
}

}  // namespace

// --------------------------------------------------------------- queries ----

int naval_base_capacity(const Game& g, ProvinceId province) {
    const Province* p = g.world.province(province);
    if (p == nullptr || p->is_sea || p->naval_base <= 0) return 0;
    const double per_level = g.content.constants.naval_base_capacity_per_level;
    if (!(per_level > 0.0)) return 0;
    return static_cast<int>(p->naval_base * per_level);
}

bool is_usable_port(const Game& g, CountryId country, ProvinceId province) {
    if (!country.valid() || g.world.country(country) == nullptr) return false;
    const Province* p = g.world.province(province);
    if (p == nullptr || p->is_sea) return false;
    if (p->naval_base <= 0 || p->sea_adj.empty()) return false;  // not a naval base
    // Controlled by the country, a co-belligerent, a puppet/overlord or an ally with
    // military access (World::has_access is the single definition of "friendly").
    return g.world.has_access(country, province);
}

RegionId adjacent_sea_region(const Game& g, ProvinceId province) {
    const Province* p = g.world.province(province);
    if (p == nullptr || p->is_sea || p->sea_adj.empty()) return RegionId{};
    // The map lists sea neighbours in ascending id; the lowest one that resolves to a
    // real sea region is the province's sea zone.
    RegionId best;
    for (ProvinceId sid : p->sea_adj) {
        const Province* sea = g.world.province(sid);
        if (sea == nullptr || !sea->is_sea || !sea->region.valid()) continue;
        if (!g.world.regions.alive(sea->region)) continue;
        if (!best.valid() || sea->region.v < best.v) best = sea->region;
    }
    return best;
}

int sea_region_distance(const Game& g, RegionId from, RegionId to) {
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
        if (p == nullptr || !p->is_sea || pid.v >= n || dist[pid.v] >= 0) continue;
        dist[pid.v] = 0;
        queue.push_back(pid);
    }
    // Breadth-first over Province::sea_adj; the first sea province reached inside
    // `to` gives the shortest zone-to-zone distance.
    for (size_t head = 0; head < queue.size(); ++head) {
        const ProvinceId cur = queue[head];
        const int32_t d = dist[cur.v];
        const Province* cp = w.province(cur);
        if (cp == nullptr) continue;
        if (cp->region == to) return d;
        for (ProvinceId nx : cp->sea_adj) {
            if (!nx.valid() || nx.v >= n || dist[nx.v] >= 0) continue;
            const Province* np = w.province(nx);
            if (np == nullptr || !np->is_sea) continue;
            dist[nx.v] = d + 1;
            queue.push_back(nx);
        }
    }
    return -1;
}

double naval_control_share(const Game& g, CountryId country, RegionId region) {
    if (!country.valid() || !region.valid() || !g.world.regions.alive(region)) return 0.0;
    std::vector<ZoneWeight> weights;
    zone_weights(g, region, &weights);
    double total = 0.0;
    double own = 0.0;
    for (const ZoneWeight& z : weights) {
        total += z.weight;
        if (z.country == country) own = z.weight;
    }
    return clamp01(safe_div(own, total));
}

TaskForceStats task_force_stats(const Game& g, TaskForceId id) {
    TaskForceStats out;
    const TaskForce* tf = g.world.task_force(id);
    if (tf == nullptr) return out;
    for (ShipId sid : tf->ships) {
        const Ship* s = g.world.ship(sid);
        if (s == nullptr) continue;
        const EquipmentDef* def = ship_def(g.content, *s);
        if (def == nullptr) continue;
        out.naval_attack += std::max(0.0, def->naval_attack);
        out.torpedo_attack += std::max(0.0, def->torpedo_attack);
        out.sub_detection += std::max(0.0, def->sub_detection);
        out.detection += std::max(0.0, def->detection);
        out.visibility += std::max(0.0, def->visibility);
        out.armour += std::max(0.0, def->armor);
        out.hull += clamp01(s->strength) * hull_size(def);
        ++out.ships;
    }
    return out;
}

// ----------------------------------------------------------- maintenance ----

void destroy_ship(Game& g, ShipId id) {
    World& w = g.world;
    Ship* ship = w.ship(id);
    if (ship == nullptr) return;
    const CountryId country = ship->country;
    const std::string name = ship->name;
    const TaskForceId tff = ship->task_force;

    TaskForce* tf = w.task_force(tff);
    if (tf != nullptr) {
        std::vector<ShipId>& roster = tf->ships;
        roster.erase(std::remove(roster.begin(), roster.end(), id), roster.end());
    }
    w.ships.destroy(id);

    // An empty task force leaves its fleet roster and the store: fleets list task
    // forces, so removing the task force is what keeps the fleet clean. The country
    // learns of ships only through its fleets (Country keeps no ship roster).
    if (tf != nullptr && tf->ships.empty()) {
        Fleet* fleet = w.fleet(tf->fleet);
        if (fleet != nullptr) {
            std::vector<TaskForceId>& list = fleet->task_forces;
            list.erase(std::remove(list.begin(), list.end(), tf->id), list.end());
        }
        const TaskForceId dead = tf->id;
        w.task_forces.destroy(dead);
    }
    g.log_event("navy", "ship '" + name + "' lost", country);
}

TaskForceId form_task_force(Game& g, CountryId country, ProvinceId port, EquipmentId equipment,
                            int count, const std::string& name) {
    if (count <= 0) return TaskForceId{};
    Country* owner = g.world.country(country);
    if (owner == nullptr) return TaskForceId{};
    const EquipmentDef* def = g.content.equipment_def(equipment);
    if (def == nullptr || def->is_archetype) return TaskForceId{};
    if (def->category != EquipmentCategory::Ship) return TaskForceId{};
    if (!is_usable_port(g, country, port)) return TaskForceId{};
    if (equipment.v >= owner->equipment_stockpile.size()) return TaskForceId{};

    // Built ships in the stockpile are the pool: the country must actually have
    // `count` of the model, and the port's naval base levels limit how many ships it
    // can host (same idea as air base capacity).
    const double stock = std::max(0.0, owner->equipment_stockpile[equipment.v]);
    if (stock + 1e-9 < static_cast<double>(count)) return TaskForceId{};
    const int capacity = naval_base_capacity(g, port);
    const int room = capacity - ships_home_at(g, port);
    if (room <= 0) return TaskForceId{};
    const int take = std::min(count, room);
    if (take <= 0) return TaskForceId{};

    // The country's first fleet, created on demand. Stale ids in Country::fleets
    // (from a fallback/removal) are skipped rather than trusted.
    FleetId fleet_id;
    for (FleetId candidate : owner->fleets) {
        if (g.world.fleet(candidate) != nullptr) {
            fleet_id = candidate;
            break;
        }
    }
    if (!fleet_id.valid()) {
        Fleet fleet;
        fleet.country = country;
        fleet.name = "Fleet " + std::to_string(owner->fleets.size() + 1);
        fleet_id = g.world.fleets.create(fleet);
        g.world.fleets[fleet_id].id = fleet_id;
        owner->fleets.push_back(fleet_id);
    }

    TaskForce tf;
    tf.country = country;
    tf.fleet = fleet_id;
    tf.name = name.empty() ? ("Task Force " + std::to_string(g.world.task_forces.size() + 1))
                           : name;
    tf.port = port;
    tf.sea_region = adjacent_sea_region(g, port);
    tf.at_sea = false;
    tf.mission = NavalMission::None;
    const TaskForceId tf_id = g.world.task_forces.create(tf);
    g.world.task_forces[tf_id].id = tf_id;
    g.world.fleets[fleet_id].task_forces.push_back(tf_id);

    owner->equipment_stockpile[equipment.v] -= static_cast<double>(take);
    if (owner->equipment_stockpile[equipment.v] < 0.0) owner->equipment_stockpile[equipment.v] = 0.0;

    for (int i = 0; i < take; ++i) {
        Ship ship;
        ship.country = country;
        ship.equipment = equipment;
        ship.fleet = fleet_id;
        ship.task_force = tf_id;
        ship.port = port;
        ship.at_sea = false;
        const ShipId sid = g.world.ships.create(ship);
        Ship& stored = g.world.ships[sid];
        stored.id = sid;
        stored.name = def->name + " " + std::to_string(sid.v + 1);  // individual hull name
        g.world.task_forces[tf_id].ships.push_back(sid);
    }
    g.log_event("navy", "task force '" + g.world.task_forces[tf_id].name + "' formed at " +
                            g.world.provinces[port].name,
                country);
    return tf_id;
}

// --------------------------------------------------------------- phase ------

void phase_naval(Game& g) {
    World& w = g.world;
    Rng& rng = g.rng.get(RngStream::Combat);
    const SimConstants& k = g.content.constants;
    const Tick now = w.tick;

    // The invasion lifecycle is a separate module: loading, crossing, interception
    // and landing happen before this hour's sea control, so invasion-support task
    // forces protect the landings they cover.
    phase_naval_invasion(g);

    const size_t region_slots = w.regions.capacity();
    const size_t tf_slots = w.task_forces.capacity();
    const size_t ship_slots = w.ships.capacity();

    // Snapshot of every task force with ships, ascending id; the snapshot is stable
    // even though the phase destroys task forces when their last ship sinks.
    std::vector<TaskForceId> tfs;
    w.task_forces.for_each([&](TaskForceId id, const TaskForce& tf) {
        if (!tf.ships.empty()) tfs.push_back(id);
    });

    // --- Port displacement -----------------------------------------------------
    // A task force whose base port was captured (or lost its naval base) moves to
    // the nearest friendly port; with no port left the ships are scuttled. A
    // displaced force lies in its new port for the rest of the hour instead of
    // sailing again immediately.
    std::vector<uint8_t> just_displaced(tf_slots, 0);
    for (TaskForceId id : tfs) {
        TaskForce* tf = w.task_force(id);
        if (tf == nullptr) continue;
        if (is_usable_port(g, tf->country, tf->port)) continue;
        const ProvinceId np = nearest_friendly_port(g, tf->country, tf->port);
        if (!np.valid()) {
            std::vector<ShipId> doomed = tf->ships;
            for (ShipId sid : doomed) destroy_ship(g, sid);
            continue;
        }
        tf->port = np;
        tf->sea_region = adjacent_sea_region(g, np);
        tf->at_sea = false;
        just_displaced[id.v] = 1;
        const Province* dest = w.province(np);
        g.log_event("navy", "task force '" + tf->name + "' displaced to " +
                                (dest != nullptr ? dest->name : std::string("port")),
                    tf->country);
    }

    // --- Fuel at sea -----------------------------------------------------------
    // Each ship burns bunker fuel while at sea; an empty bunker forces the whole
    // task force back to port (spec edge case: "task force with no fuel stays in
    // port"). Ships that are in port are refuelled below.
    for (TaskForceId id : tfs) {
        TaskForce* tf = w.task_force(id);
        if (tf == nullptr || !tf->at_sea) continue;
        bool dry = false;
        for (ShipId sid : tf->ships) {
            Ship* s = w.ship(sid);
            if (s == nullptr) continue;
            const EquipmentDef* def = ship_def(g.content, *s);
            const double use = k.naval_fuel_use_per_hour *
                               (def != nullptr ? std::max(0.0, def->fuel_use) : 0.0);
            s->fuel = clamp01(s->fuel - use);
            if (!(s->fuel > 0.0)) dry = true;
        }
        if (dry) {
            tf->at_sea = false;
            g.log_event("navy", "task force '" + tf->name + "' returns to port: out of fuel",
                        tf->country);
        }
    }

    // --- Bucket the task forces at sea by sea zone -----------------------------
    // Every force's derived sensor state is refreshed here (also for a zone with a
    // single force, which never fights): how much its own sensors contribute of the
    // detection equation against its own visibility.
    std::vector<std::vector<TaskForceId>> by_zone(region_slots);
    for (TaskForceId id : tfs) {
        TaskForce* tf = w.task_force(id);
        if (tf == nullptr) continue;
        if (!tf->at_sea) {
            tf->detection = 0.0;
            continue;
        }
        double detect = 0.0;
        double vis = 0.0;
        for (ShipId sid : tf->ships) {
            const Ship* s = w.ship(sid);
            if (s == nullptr) continue;
            const EquipmentDef* def = ship_def(g.content, *s);
            detect += ship_detection_power(def, s->strength);
            vis += ship_visibility_power(def, s->strength);
        }
        tf->detection = clamp01(safe_div(detect, detect + vis));
        if (!tf->sea_region.valid() || tf->sea_region.v >= region_slots) continue;
        if (tf->mission == NavalMission::None) continue;  // no orders, no sea control
        by_zone[tf->sea_region.v].push_back(id);
    }

    // Per-ship damage of this hour, applied after every zone has fought so the hour
    // is simultaneous: a ship's own damage never changes what it inflicts.
    std::vector<double> strength_hit(ship_slots, 0.0);
    std::vector<double> org_hit(ship_slots, 0.0);
    std::vector<uint8_t> fought(ship_slots, 0);
    std::vector<double> screen_frac(tf_slots, 0.0);
    std::vector<std::vector<TaskForceId>> targets(tf_slots);

    for (size_t ri = 0; ri < region_slots; ++ri) {
        const std::vector<TaskForceId>& ids = by_zone[ri];
        if (ids.size() < 2) continue;

        // Screening share of every task force in the zone (destroyers' presence share).
        for (TaskForceId id : ids) {
            TaskForce* tf = w.task_force(id);
            if (tf == nullptr) continue;
            double screen = 0.0;
            double total = 0.0;
            for (ShipId sid : tf->ships) {
                const Ship* s = w.ship(sid);
                if (s == nullptr) continue;
                const EquipmentDef* def = ship_def(g.content, *s);
                const double power = ship_zone_power(def, s->strength);
                total += power;
                if (!large_hull(def, k)) screen += power;
            }
            screen_frac[id.v] = clamp01(safe_div(screen, total));
            targets[id.v].clear();
        }

        // Detection: one RngStream::Combat draw per hostile task-force pair decides
        // both directions, so mutual detection (and therefore engagement) is
        // r < min(spot_a, spot_b). Nothing else in this pass consumes a Combat draw,
        // which keeps the sequence reproducible.
        for (size_t i = 0; i < ids.size(); ++i) {
            TaskForce* a = w.task_force(ids[i]);
            if (a == nullptr) continue;
            for (size_t j = i + 1; j < ids.size(); ++j) {
                TaskForce* b = w.task_force(ids[j]);
                if (b == nullptr) continue;
                if (a->country == b->country) continue;
                if (!w.at_war(a->country, b->country)) continue;
                if (!combat_mission(a->mission) || !combat_mission(b->mission)) continue;

                double da = 0.0, va = 0.0, db = 0.0, vb = 0.0;
                for (ShipId sid : a->ships) {
                    const Ship* s = w.ship(sid);
                    if (s == nullptr) continue;
                    const EquipmentDef* def = ship_def(g.content, *s);
                    da += ship_detection_power(def, s->strength);
                    va += ship_visibility_power(def, s->strength);
                }
                for (ShipId sid : b->ships) {
                    const Ship* s = w.ship(sid);
                    if (s == nullptr) continue;
                    const EquipmentDef* def = ship_def(g.content, *s);
                    db += ship_detection_power(def, s->strength);
                    vb += ship_visibility_power(def, s->strength);
                }
                const double spot_a = clamp01(k.naval_detection_scale * safe_div(da, da + vb));
                const double spot_b = clamp01(k.naval_detection_scale * safe_div(db, db + va));
                const double roll = rng.next_double();  // documented draw: one per pair
                if (roll < spot_a && roll < spot_b) {
                    targets[ids[i].v].push_back(ids[j]);
                    targets[ids[j].v].push_back(ids[i]);
                    a->last_engagement = now;
                    b->last_engagement = now;
                }
            }
        }

        // Engagement: each engaged task force fires at the ships of the hostile task
        // forces it has detected. Firepower is spread over the enemy formation by
        // hull size, then converted to a fraction of each target's hull by its
        // toughness, so a battleship both soaks and takes more total damage.
        const size_t engage_cap = static_cast<size_t>(
            std::max(1.0, std::floor(k.naval_max_engagement_ships)));
        for (TaskForceId aid : ids) {
            TaskForce* a = w.task_force(aid);
            if (a == nullptr || targets[aid.v].empty()) continue;

            std::vector<ShipId> shooters;
            for (ShipId sid : a->ships) {
                if (shooters.size() >= engage_cap) break;
                shooters.push_back(sid);
            }
            std::vector<ShipId> defenders;
            for (TaskForceId tid : targets[aid.v]) {
                const TaskForce* t = w.task_force(tid);
                if (t == nullptr) continue;
                for (ShipId sid : t->ships) {
                    if (defenders.size() >= engage_cap) break;
                    defenders.push_back(sid);
                }
            }
            if (shooters.empty() || defenders.empty()) continue;

            double formation_size = 0.0;
            for (ShipId sid : defenders) {
                const Ship* s = w.ship(sid);
                if (s == nullptr) continue;
                formation_size += hull_size(ship_def(g.content, *s));
            }
            if (!(formation_size > 0.0)) continue;

            // One RngStream::Combat draw per task force that actually engages: the
            // combat roll scales its firepower for this hour (naval_combat_roll_base
            // + next_double(), i.e. 0.5..1.5 by default).
            const double combat_roll = k.naval_combat_roll_base + rng.next_double();

            for (ShipId sid : shooters) {
                const Ship* s = w.ship(sid);
                if (s == nullptr || !(s->strength > 0.0)) continue;
                const EquipmentDef* es = ship_def(g.content, *s);
                if (es == nullptr) continue;
                const double readiness = clamp01(s->organisation) * (1.0 + clamp01(s->experience));
                const double mult = clamp01(s->strength) * readiness * combat_roll;

                for (ShipId tid : defenders) {
                    const Ship* t = w.ship(tid);
                    if (t == nullptr) continue;
                    const EquipmentDef* et = ship_def(g.content, *t);
                    if (et == nullptr) continue;

                    // Guns vs armour: piercing and gun power against the target's
                    // plating. Armour never floors damage to zero outright, it only
                    // discounts it.
                    const double gun_power = std::max(0.0, es->naval_attack);
                    const double pierce = gun_power + std::max(0.0, es->piercing);
                    const double armour_factor = safe_div(pierce, pierce + std::max(0.0, et->armor));

                    // Torpedoes ignore armour and are deadlier against capital hulls.
                    double torpedo_power = std::max(0.0, es->torpedo_attack);
                    if (large_hull(et, k)) torpedo_power *= k.naval_torpedo_large_hull_bonus;

                    // Carriers strike with their air group (ground_attack is the
                    // air model's anti-ship/ground strike power); the target's
                    // anti-air guns (air_attack) cut the attack.
                    const double air_power =
                        std::max(0.0, es->ground_attack) * k.naval_air_attacks_per_hour *
                        safe_div(1.0, 1.0 + k.naval_aa_carrier_air_factor *
                                              std::max(0.0, et->air_attack));

                    double power = gun_power * armour_factor + torpedo_power + air_power;

                    // Submarines (torpedo boats with no guns) hide from targets whose
                    // anti-submarine detection is weak.
                    if (!(es->naval_attack > 0.0) && es->torpedo_attack > 0.0) {
                        const double stealth = clamp01(
                            1.0 - safe_div(std::max(0.0, et->sub_detection),
                                           std::max(0.0, et->sub_detection) +
                                               std::max(0.0, es->visibility)));
                        power *= clamp01(1.0 - k.naval_sub_detection_penalty * stealth);
                    }
                    if (!(power > 0.0)) continue;

                    // Destroyers screen the capitals of their own formation.
                    double screen_factor = 1.0;
                    if (large_hull(et, k) && t->task_force.valid() &&
                        t->task_force.v < screen_frac.size()) {
                        screen_factor = clamp01(1.0 - k.naval_screen_share_cap *
                                                           screen_frac[t->task_force.v]);
                    }

                    const double size = hull_size(et);
                    const double toughness = size + std::max(0.0, et->armor);
                    const double share = power * mult * screen_factor * size /
                                         (formation_size * toughness);
                    strength_hit[tid.v] += k.naval_combat_scale * share;
                    org_hit[tid.v] += k.naval_org_damage_scale * share;
                    fought[tid.v] = 1;
                }
                fought[sid.v] = 1;
            }
        }

        for (TaskForceId id : ids) targets[id.v].clear();
    }

    // Apply the hour's damage simultaneously, then sink what reached zero.
    w.ships.for_each([&](ShipId id, Ship& s) {
        if (strength_hit[id.v] > 0.0 || org_hit[id.v] > 0.0) {
            s.strength = clamp01(s.strength - strength_hit[id.v]);
            s.organisation = clamp01(s.organisation - std::min(0.5, org_hit[id.v]));
        }
        if (fought[id.v] != 0) {
            s.experience = clamp01(s.experience + k.naval_combat_experience_per_hour);
        }
    });
    std::vector<ShipId> sunk;
    w.ships.for_each([&](ShipId id, const Ship& s) {
        if (!(s.strength > 0.0)) sunk.push_back(id);
    });
    for (ShipId id : sunk) destroy_ship(g, id);

    // --- Convoy raiding --------------------------------------------------------
    // Raiders sink convoy units out of the enemy's convoy pool. The raided country's
    // escorts in the same zone cut the toll.
    const EquipmentId convoys = convoy_model(g.content);
    if (convoys.valid()) {
        std::vector<std::pair<CountryId, double>> raids;   // raider presence per country
        std::vector<std::pair<CountryId, double>> escorts; // escort presence per country
        for (size_t ri = 0; ri < region_slots; ++ri) {
            if (by_zone[ri].empty()) continue;
            raids.clear();
            escorts.clear();
            auto bump = [](std::vector<std::pair<CountryId, double>>& list, CountryId c,
                           double v) {
                for (auto& entry : list) {
                    if (entry.first == c) {
                        entry.second += v;
                        return;
                    }
                }
                list.emplace_back(c, v);
            };
            for (TaskForceId id : by_zone[ri]) {
                const TaskForce* tf = w.task_force(id);
                if (tf == nullptr) continue;
                double power = 0.0;
                for (ShipId sid : tf->ships) {
                    const Ship* s = w.ship(sid);
                    if (s == nullptr) continue;
                    power += ship_zone_power(ship_def(g.content, *s), s->strength);
                }
                if (tf->mission == NavalMission::ConvoyRaid) bump(raids, tf->country, power);
                if (tf->mission == NavalMission::ConvoyEscort) bump(escorts, tf->country, power);
            }
            for (const auto& raider : raids) {
                if (!(raider.second > 0.0)) continue;
                // Every belligerent of the raider takes the toll; escorts it fields in
                // the same zone (possibly none) cut it. Walking the country store in
                // ascending id keeps the sequence deterministic.
                w.countries.for_each([&](CountryId victim_id, Country& victim) {
                    if (!w.at_war(raider.first, victim_id)) return;
                    if (convoys.v >= victim.equipment_stockpile.size()) return;
                    double& stock = victim.equipment_stockpile[convoys.v];
                    if (!(stock > 0.0)) return;
                    double escort_power = 0.0;
                    for (const auto& escort : escorts) {
                        if (escort.first == victim_id) escort_power = escort.second;
                    }
                    const double protection = 1.0 +
                        k.naval_escort_protection * safe_div(escort_power, raider.second);
                    const double sunk_convoys =
                        k.naval_raid_convoy_damage * raider.second / protection;
                    if (!(sunk_convoys > 0.0)) return;
                    const double lost = std::min(stock, sunk_convoys);
                    stock -= lost;
                    if (lost >= 1.0) {
                        g.log_event("navy",
                                    "convoy raiding sunk " + std::to_string(static_cast<int>(lost)) +
                                        " convoys",
                                    raider.first);
                    }
                });
            }
        }
    }

    // --- Retreat, repair, refuel, sortie ---------------------------------------
    for (TaskForceId id : tfs) {
        TaskForce* tf = w.task_force(id);
        if (tf == nullptr) continue;
        double sum_strength = 0.0;
        double sum_org = 0.0;
        int n = 0;
        bool dry = false;
        for (ShipId sid : tf->ships) {
            const Ship* s = w.ship(sid);
            if (s == nullptr) continue;
            sum_strength += clamp01(s->strength);
            sum_org += clamp01(s->organisation);
            ++n;
            if (!(s->fuel > 0.0)) dry = true;
        }
        if (n == 0) continue;
        double avg_strength = sum_strength / static_cast<double>(n);
        const double avg_org = sum_org / static_cast<double>(n);

        // The single transition rule back to port, shared with the AI and the
        // invasion module: a task force returns to port when
        //   * its average strength falls below naval_retreat_strength_threshold, or
        //   * its average organisation falls below naval_retreat_org_threshold, or
        //   * its mission is None ("stand down"), or
        //   * its bunker fuel ran out (handled above).
        // The mission stays set (the AI still sees the orders); the force repairs,
        // refuels and recovers in port, and sorties again once it is fit again and
        // still has a mission other than None.
        const bool stand_down = tf->mission == NavalMission::None;
        if (tf->at_sea && (stand_down || avg_strength < k.naval_retreat_strength_threshold ||
                           avg_org < k.naval_retreat_org_threshold)) {
            tf->at_sea = false;
            g.log_event("navy", "task force '" + tf->name + "' withdraws to port", tf->country);
        }

        if (tf->at_sea) {
            if (tf->mission == NavalMission::Training) {
                for (ShipId sid : tf->ships) {
                    Ship* s = w.ship(sid);
                    if (s == nullptr) continue;
                    s->experience = clamp01(s->experience + k.naval_training_experience_per_hour);
                }
            }
        } else {
            // In port: refuel from the country's fuel pool (one fuel unit per bunker
            // point), recover organisation, and repair the hull. Fuel is the binding
            // cost of repair; spare parts come out of the stockpile of the ship's own
            // model whenever the country still has them. Both are real draws.
            Country* owner = w.country(tf->country);
            const bool dockyard = is_usable_port(g, tf->country, tf->port);
            for (ShipId sid : tf->ships) {
                Ship* s = w.ship(sid);
                if (s == nullptr) continue;
                if (owner != nullptr && s->fuel < 1.0 && owner->fuel > 0.0) {
                    const double taken = std::min(1.0 - s->fuel, owner->fuel);
                    owner->fuel -= taken;
                    s->fuel = clamp01(s->fuel + taken);
                }
                if (s->organisation < 1.0) {
                    s->organisation =
                        clamp01(s->organisation + k.naval_repair_org_per_hour);
                }
                if (!dockyard || s->strength >= 1.0 || owner == nullptr) continue;
                const double delta = std::min(k.naval_repair_per_hour, 1.0 - s->strength);
                if (!(delta > 0.0)) continue;
                const double fuel_cost = k.naval_repair_cost_fuel * delta;
                if (owner->fuel < fuel_cost) continue;  // no fuel, no dockyard work
                owner->fuel -= fuel_cost;
                const EquipmentDef* def = ship_def(g.content, *s);
                if (def != nullptr && s->equipment.valid() &&
                    s->equipment.v < owner->equipment_stockpile.size()) {
                    double& stock = owner->equipment_stockpile[s->equipment.v];
                    stock -= std::min(stock, k.naval_repair_cost_stockpile_share * delta);
                    if (stock < 0.0) stock = 0.0;
                }
                s->strength = clamp01(s->strength + delta);
            }
            double repaired_strength = 0.0;
            double repaired_org = 0.0;
            for (ShipId sid : tf->ships) {
                const Ship* s = w.ship(sid);
                if (s == nullptr) continue;
                repaired_strength += clamp01(s->strength);
                repaired_org += clamp01(s->organisation);
            }
            avg_strength = repaired_strength / static_cast<double>(n);

            // A repaired and fuelled task force with standing orders sorties again; a force
            // displaced this hour waits for the next one.
            if (!just_displaced[id.v] && tf->mission != NavalMission::None && dockyard &&
                tf->sea_region.valid() &&
                avg_strength >= k.naval_retreat_strength_threshold &&
                repaired_org / static_cast<double>(n) >= k.naval_retreat_org_threshold && !dry) {
                tf->at_sea = true;
            }
        }

        // Keep each ship's position consistent with its task force.
        for (ShipId sid : tf->ships) {
            Ship* s = w.ship(sid);
            if (s == nullptr) continue;
            s->at_sea = tf->at_sea;
            s->sea_region = tf->at_sea ? tf->sea_region : RegionId{};
            if (!tf->at_sea && tf->port.valid()) s->port = tf->port;
        }
        if (!tf->at_sea) tf->detection = 0.0;
    }

    // --- Naval control cache ---------------------------------------------------
    // One pass over the task forces per zone builds each country's presence weight;
    // shares are written sorted by country id. Runs last so the cache describes where
    // the ships actually are after retreats and sinkings.
    w.regions.for_each([](RegionId, Region& r) { r.naval_control.clear(); });
    std::vector<ZoneWeight> weights;
    for (size_t ri = 0; ri < region_slots; ++ri) {
        if (by_zone[ri].empty()) continue;
        const RegionId region(static_cast<uint32_t>(ri));
        Region* reg = w.regions.try_get(region);
        if (reg == nullptr) continue;
        zone_weights(g, region, &weights);
        if (weights.empty()) continue;
        double total = 0.0;
        for (const ZoneWeight& z : weights) total += z.weight;
        if (!(total > 0.0)) continue;
        std::sort(weights.begin(), weights.end(),
                  [](const ZoneWeight& a, const ZoneWeight& b) {
                      return a.country.v < b.country.v;
                  });
        for (const ZoneWeight& z : weights) {
            const double share = clamp01(safe_div(z.weight, total));
            if (share > 0.0) reg->naval_control.emplace_back(z.country, share);
        }
    }
}

}  // namespace hoi