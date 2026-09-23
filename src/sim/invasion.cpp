// Naval invasion lifecycle (spec sections 53-55): the army is gathered at a
// friendly port, its divisions are loaded onto shipping, the convoy crosses the
// sea (interception destroys transports and the divisions with them), and the
// survivors land on the hostile coast and establish a beachhead.
//
// The invasion state machine, driven one hour at a time by phase_naval_invasion:
//
//   GATHER   progress == 0, not landed: the army's divisions walk to the origin
//            port (real movement over the land graph, no teleporting). The step
//            does nothing until every alive division of the army stands there.
//   CROSS    progress in (0,1): shipping is committed, the convoy sets out, and
//            hostile offensive task forces in the target sea zone may intercept.
//   LANDED   progress >= 1: the divisions are placed on the target province,
//            control is taken when it is undefended, the beachhead supply source
//            is raised, and the invasion entry is removed (army back to FrontLine).
//
// Shipping is abstract: it is drawn from the country's stockpile. A dedicated
// transport ship model (a Ship-category model whose key starts with "transport")
// is preferred when present; otherwise Convoy-category units (convoy_1) are used.
// Both are the country's stockpile of ship/convoy equipment; see
// hoi::transport_indices below.

#include "sim/navy.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/map.h"
#include "sim/phases.h"
#include "sim/world.h"

namespace hoi {
namespace {

// Equipment indices that can carry this country's invasion, in ascending order.
// A country that fields transport ship models uses those; everyone else uses
// abstract Convoy-category shipping. The choice is per country and consultable at
// both load and refund time, so the accounting stays symmetric: shipping is taken
// from, and returned to, exactly one pool.
bool has_transport_ships(const Game& g, CountryId country) {
    const World& w = g.world;
    bool found = false;
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (found || tf.country != country) return;
        for (ShipId sid : tf.ships) {
            const Ship* s = w.ship(sid);
            if (!s) continue;
            const EquipmentDef* def = g.content.equipment_def(s->equipment);
            if (def && def->category == EquipmentCategory::Ship &&
                def->key.rfind("transport", 0) == 0) {
                found = true;
                return;
            }
        }
    });
    return found;
}

std::vector<size_t> transport_indices(const Game& g, CountryId country) {
    std::vector<size_t> out;
    const EquipmentCategory wanted = has_transport_ships(g, country)
                                         ? EquipmentCategory::Ship
                                         : EquipmentCategory::Convoy;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& def = g.content.equipment[i];
        if (def.category != wanted) continue;
        if (wanted == EquipmentCategory::Ship && def.key.rfind("transport", 0) != 0) continue;
        out.push_back(i);
    }
    return out;
}

double stockpile_at(const Country& c, size_t idx) {
    return idx < c.equipment_stockpile.size() ? c.equipment_stockpile[idx] : 0.0;
}

// Loaded shipping the country can commit right now.
double transports_available(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (!c) return 0.0;
    double sum = 0.0;
    for (size_t idx : transport_indices(g, country)) sum += stockpile_at(*c, idx);
    return sum;
}

// Takes up to `amount` shipping from the country's stockpile; returns what was
// actually taken. Deterministic: indices are visited in ascending order.
double take_transports(Game& g, CountryId country, double amount) {
    Country* c = g.world.country(country);
    if (!c) return 0.0;
    double remaining = amount;
    for (size_t idx : transport_indices(g, country)) {
        if (remaining <= 0.0) break;
        if (idx >= c->equipment_stockpile.size()) continue;
        const double take = std::min(remaining, c->equipment_stockpile[idx]);
        c->equipment_stockpile[idx] -= take;
        remaining -= take;
    }
    const double taken = amount - remaining;
    return taken > 0.0 ? taken : 0.0;
}

// Returns shipping to the country's stockpile. Put back at the most preferred
// carrying index, in the same order take_transports draws from.
void return_transports(Game& g, CountryId country, double amount) {
    if (amount <= 0.0) return;
    Country* c = g.world.country(country);
    if (!c) return;
    const std::vector<size_t> idx = transport_indices(g, country);
    if (idx.empty()) return;
    if (idx.front() >= c->equipment_stockpile.size()) {
        c->equipment_stockpile.resize(idx.front() + 1, 0.0);
    }
    c->equipment_stockpile[idx.front()] += amount;
}

bool hostile(const World& w, CountryId a, CountryId b) {
    if (!b.valid()) return true;  // an unowned province can be taken
    if (a == b) return false;
    return w.at_war(a, b);
}

NavalInvasion* find_invasion(World& w, ArmyId army) {
    for (NavalInvasion& inv : w.invasions) {
        if (inv.army == army) return &inv;
    }
    return nullptr;
}

void erase_invasion(World& w, ArmyId army) {
    w.invasions.erase(std::remove_if(w.invasions.begin(), w.invasions.end(),
                                     [&](const NavalInvasion& inv) { return inv.army == army; }),
                      w.invasions.end());
}

// Alive divisions still attached to the army, in the army's (ascending creation)
// roster order.
std::vector<DivisionId> army_divisions(const World& w, ArmyId army) {
    std::vector<DivisionId> out;
    const Army* a = w.army(army);
    if (!a) return out;
    for (DivisionId did : a->divisions) {
        if (w.divisions.alive(did)) out.push_back(did);
    }
    return out;
}

void clear_movement(Division& d) {
    d.path.clear();
    d.moving = false;
    d.move_from = ProvinceId{};
    d.move_to = ProvinceId{};
    d.move_progress = 0.0;
    d.retreating = false;
}

// Puts an army back into a coherent state after an invasion aborts or is removed:
// every division drops the invasion order and any movement, and the army takes the
// country's front line when it has one, otherwise no order. Shipping refunds are
// the caller's responsibility (only the committed convoy is refundable).
void reset_army_after_invasion(Game& g, ArmyId army) {
    World& w = g.world;
    Army* a = w.army(army);
    if (!a) return;
    for (DivisionId did : a->divisions) {
        Division* d = w.division(did);
        if (!d) continue;
        clear_movement(*d);
        d->order = OrderKind::None;
        d->order_target = ProvinceId{};
    }
    std::vector<ProvinceId> front = compute_front_line(w, a->country);
    a->order.kind = front.empty() ? OrderKind::None : OrderKind::FrontLine;
    a->order.line = std::move(front);
    a->order.target_line.clear();
    a->order.progress = 0.0;
}

// Logs the abort reason with the army named, then resets the army. Returns true so
// the caller erases the invasion entry.
bool abort_invasion(Game& g, NavalInvasion& inv, const std::string& reason) {
    const Army* a = g.world.army(inv.army);
    g.log_event("navy",
                "invasion by '" + (a ? a->name : std::string("#") + std::to_string(inv.army.v)) +
                    "' aborted: " + reason,
                inv.country);
    reset_army_after_invasion(g, inv.army);
    return true;
}

// Removes a division from the world and from every roster it appears on. Named
// losses are real: its equipment and manpower are gone, not returned anywhere.
void remove_division(Game& g, DivisionId id) {
    World& w = g.world;
    Division* d = w.division(id);
    if (!d) return;
    const CountryId owner = d->country;
    const ArmyId army = d->army;
    if (Country* c = w.country(owner)) {
        c->divisions.erase(std::remove(c->divisions.begin(), c->divisions.end(), id),
                           c->divisions.end());
        for (auto it = c->training.begin(); it != c->training.end(); ++it) {
            if (it->division == id) {
                c->training.erase(it);
                break;
            }
        }
    }
    if (Army* a = w.army(army)) {
        a->divisions.erase(std::remove(a->divisions.begin(), a->divisions.end(), id),
                           a->divisions.end());
    }
    w.battles.for_each([&](BattleId, Battle& b) {
        b.attacker.divisions.erase(
            std::remove(b.attacker.divisions.begin(), b.attacker.divisions.end(), id),
            b.attacker.divisions.end());
        b.defender.divisions.erase(
            std::remove(b.defender.divisions.begin(), b.defender.divisions.end(), id),
            b.defender.divisions.end());
    });
    w.divisions.destroy(id);
}

// True when an enemy division stands in `province`, i.e. the landing is contested
// and control cannot simply be taken.
bool province_defended(const World& w, ProvinceId province, CountryId country) {
    bool defended = false;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (defended || d.location != province) return;
        if (!w.divisions.alive(d.id)) return;
        if (d.strength <= 0.0) return;  // already broken: driven off this tick
        if (hostile(w, country, d.country)) defended = true;
    });
    return defended;
}

// Enemy offensive task forces operating in `region`: their summed offensive
// statistics are the interception threat.
double interception_threat(const Game& g, CountryId country, RegionId region) {
    const World& w = g.world;
    double threat = 0.0;
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (tf.country == country || !tf.country.valid()) return;
        if (!tf.at_sea || tf.sea_region != region || tf.ships.empty()) return;
        if (!naval_mission_is_offensive(tf.mission)) return;
        if (!w.at_war(country, tf.country)) return;
        const TaskForceStats st = task_force_stats(g, tf.id);
        threat += st.naval_attack + st.torpedo_attack;
    });
    return threat;
}

// Friendly escort strength in the same zone mitigates interception.
double escort_strength(const Game& g, CountryId country, RegionId region) {
    const World& w = g.world;
    double escort = 0.0;
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (tf.country != country || tf.sea_region != region || tf.ships.empty()) return;
        if (!tf.at_sea) return;
        if (tf.mission != NavalMission::InvasionSupport &&
            tf.mission != NavalMission::ConvoyEscort) {
            return;
        }
        const TaskForceStats st = task_force_stats(g, tf.id);
        escort += st.naval_attack + st.torpedo_attack;
    });
    return escort;
}

// Runs one hour of `inv`; returns true when the invasion entry must be removed.
bool advance_invasion(Game& g, NavalInvasion& inv) {
    World& w = g.world;
    const SimConstants& k = g.content.constants;

    Army* army = w.army(inv.army);
    Country* country = w.country(inv.country);
    if (!army) return abort_invasion(g, inv, "army no longer exists");
    if (!country || !country->alive) return abort_invasion(g, inv, "country left the war");

    const std::vector<DivisionId> divisions = army_divisions(w, inv.army);
    if (divisions.empty()) return abort_invasion(g, inv, "no divisions left");

    // A convoy that has already set out keeps crossing; one still in port both
    // gathers and, once the army is home, sails this same hour.
    bool crossing = inv.progress > 0.0;
    if (!inv.landed && !crossing) {
        // GATHER: every division must actually reach the port.
        for (DivisionId did : divisions) {
            const Division* d = w.division(did);
            if (!d) continue;
            if (d->location != inv.origin || d->moving || !d->path.empty()) return false;
        }

        // LOAD: commit the shipping for every division.
        const double need =
            k.naval_invasion_convoys_per_division * static_cast<double>(divisions.size());
        const double taken = take_transports(g, inv.country, need);
        if (taken + 1e-9 < need) {
            return_transports(g, inv.country, taken);  // give back what we took
            return abort_invasion(g, inv, "shipping lost at the port");
        }
        for (DivisionId did : divisions) {
            Division* d = w.division(did);
            if (!d) continue;
            clear_movement(*d);
            d->order = OrderKind::NavalInvasion;
            d->order_target = inv.target;
        }
        inv.started = w.tick;
        crossing = true;
        g.log_event("navy", "invasion convoy sails for '" +
                                (w.province(inv.target) ? w.province(inv.target)->name
                                                        : std::string("?")) +
                                "'",
                    inv.country);
    }

    // CROSS: advance the convoy and resolve interception.
    if (!inv.landed && crossing) {
        const double threat = interception_threat(g, inv.country, inv.sea_region);
        if (threat > 0.0) {
            const double escort = escort_strength(g, inv.country, inv.sea_region);
            double effective = threat - escort * k.naval_invasion_escort_mitigation;
            if (effective < 0.0) effective = 0.0;
            const double p = clamp01(k.naval_invasion_interception_base * effective /
                                     (effective + k.naval_invasion_interception_threat_scale));
            // Draw 1: whether the enemy finds the convoy this hour.
            const double roll = g.rng.get(RngStream::Combat).next_double();
            if (roll < p && !divisions.empty()) {
                // Draw 2: which transport load (and division) is lost.
                const uint32_t pick =
                    g.rng.get(RngStream::Combat).next_below(static_cast<uint32_t>(divisions.size()));
                const DivisionId lost = divisions[pick];
                remove_division(g, lost);
                g.log_event("navy", "convoy intercepted: a division is lost at sea", inv.country);
                if (army_divisions(w, inv.army).empty()) {
                    return abort_invasion(g, inv, "convoy destroyed at sea");
                }
            }
        }

        RegionId origin_sea = adjacent_sea_region(g, inv.origin);
        RegionId target_sea = inv.sea_region.valid() ? inv.sea_region
                                                     : adjacent_sea_region(g, inv.target);
        int hops = sea_region_distance(g, origin_sea, target_sea);
        if (hops < 0) {
            hops = 0;  // validated at launch; treat an unexpected break as one hop
        }
        const double hours =
            static_cast<double>(std::max(1, hops)) * k.naval_invasion_hours_per_sea_hop;
        inv.progress += 1.0 / hours;
        if (!std::isfinite(inv.progress)) inv.progress = 1.0;
    }

    if (inv.landed || inv.progress < 1.0) return false;

    // LAND: place the survivors, take the undefended coast, raise the beachhead.
    const std::vector<DivisionId> survivors = army_divisions(w, inv.army);
    for (DivisionId did : survivors) {
        Division* d = w.division(did);
        if (!d) continue;
        d->previous_location = inv.origin;
        d->location = inv.target;
        clear_movement(*d);
        d->order = OrderKind::None;
        d->order_target = ProvinceId{};
    }

    Province* target = w.province(inv.target);
    if (target) {
        if (!province_defended(w, inv.target, inv.country) &&
            hostile(w, inv.country, target->controller)) {
            target->controller = inv.country;
            // Beachhead supply source: the landed province becomes a supply hub for
            // whoever controls it, so the landed divisions can draw supply while no
            // captured port is yet connected to the home network (docs/mechanics/
            // naval_warfare.md: "the landing province becomes a supply source").
            target->supply_hub = true;
        }
    }

    // Shipping comes home for the divisions that made it ashore; the losses stay
    // lost (their share was never returned).
    return_transports(g, inv.country,
                      k.naval_invasion_convoys_per_division *
                          static_cast<double>(survivors.size()));

    if (Army* a = w.army(inv.army)) {
        a->order.kind = OrderKind::FrontLine;
        a->order.line.clear();
        a->order.target_line.clear();
        if (target) a->order.line.push_back(inv.target);
        a->order.progress = 0.0;
        a->order.started = w.tick;
    }
    g.log_event("navy", "invasion landings complete at '" +
                            (target ? target->name : std::string("?")) + "'",
                inv.country);
    return true;
}

void set_divisions_embarked_path(Game& g, NavalInvasion& inv, Army& army) {
    for (DivisionId did : army.divisions) {
        Division* d = g.world.division(did);
        if (!d || !d->location.valid()) continue;
        d->order = OrderKind::NavalInvasion;
        d->order_target = inv.origin;
        if (d->location == inv.origin) {
            clear_movement(*d);
            continue;
        }
        PathRequest req;
        req.country = army.country;
        req.allow_hostile = false;
        req.require_controlled = true;
        std::vector<ProvinceId> path = find_path(g.world, d->location, inv.origin, req);
        if (!path.empty()) {
            d->path = std::move(path);
            d->moving = true;
            d->move_from = d->location;
            d->move_to = d->path.empty() ? ProvinceId{} : d->path[0];
            d->move_progress = 0.0;
        }
    }
}

}  // namespace

bool start_naval_invasion(Game& g, ArmyId army, ProvinceId origin, ProvinceId target) {
    World& w = g.world;
    Army* a = w.army(army);
    if (!a) return false;
    Country* country = w.country(a->country);
    if (!country || !country->alive) return false;

    const Province* op = w.province(origin);
    const Province* tp = w.province(target);
    if (!op || !tp) return false;
    if (tp->is_sea || !tp->coastal) return false;
    if (op->is_sea) return false;
    if (!is_usable_port(g, a->country, origin)) return false;
    if (!hostile(w, a->country, tp->controller)) return false;

    const RegionId origin_sea = adjacent_sea_region(g, origin);
    const RegionId target_sea = adjacent_sea_region(g, target);
    if (!origin_sea.valid() || !target_sea.valid()) return false;
    if (sea_region_distance(g, origin_sea, target_sea) < 0) return false;

    const std::vector<DivisionId> divisions = army_divisions(w, army);
    if (divisions.empty()) return false;
    if (find_invasion(w, army) != nullptr) return false;  // already invading

    const double need =
        g.content.constants.naval_invasion_convoys_per_division * static_cast<double>(divisions.size());
    if (transports_available(g, a->country) + 1e-9 < need) return false;

    // Require at least one division that can actually reach the port; otherwise the
    // army can never gather and the invasion would stall forever.
    bool reachable = false;
    for (DivisionId did : divisions) {
        const Division* d = w.division(did);
        if (!d || !d->location.valid()) continue;
        if (d->location == origin) {
            reachable = true;
            break;
        }
        PathRequest req;
        req.country = a->country;
        req.allow_hostile = false;
        req.require_controlled = true;
        if (!find_path(w, d->location, origin, req).empty()) {
            reachable = true;
            break;
        }
    }
    if (!reachable) return false;

    NavalInvasion inv;
    inv.army = army;
    inv.country = a->country;
    inv.origin = origin;
    inv.target = target;
    inv.sea_region = target_sea;
    inv.progress = 0.0;
    inv.started = w.tick;
    inv.landed = false;
    w.invasions.push_back(inv);

    a->order.kind = OrderKind::NavalInvasion;
    a->order.line.clear();
    a->order.line.push_back(origin);
    a->order.target_line.clear();
    a->order.target_line.push_back(target);
    a->order.progress = 0.0;
    a->order.started = w.tick;
    set_divisions_embarked_path(g, w.invasions.back(), *a);

    g.log_event("navy", "naval invasion of '" + tp->name + "' launched", a->country);
    return true;
}

void cancel_naval_invasion(Game& g, ArmyId army) {
    World& w = g.world;
    Army* a = w.army(army);
    NavalInvasion* inv = find_invasion(w, army);
    if (!inv) {
        if (a) {
            a->order.kind = OrderKind::None;
            a->order.line.clear();
            a->order.target_line.clear();
            a->order.progress = 0.0;
        }
        return;
    }

    const ProvinceId origin = inv->origin;
    const ProvinceId target = inv->target;
    const bool committed = inv->progress > 0.0 || inv->landed;
    const CountryId country = inv->country;

    std::vector<DivisionId> divisions;
    if (a) divisions = army_divisions(w, army);
    for (DivisionId did : divisions) {
        Division* d = w.division(did);
        if (!d) continue;
        // Landed divisions return to the port; embarked ones are already there.
        if (d->location == target) {
            d->previous_location = target;
            d->location = origin;
        }
        clear_movement(*d);
        d->order = OrderKind::None;
        d->order_target = ProvinceId{};
    }
    if (committed) {
        return_transports(g, country,
                          g.content.constants.naval_invasion_convoys_per_division *
                              static_cast<double>(divisions.size()));
    }
    if (a) {
        a->order.kind = OrderKind::None;
        a->order.line.clear();
        a->order.target_line.clear();
        a->order.progress = 0.0;
    }
    erase_invasion(w, army);
    g.log_event("navy", "invasion cancelled", country);
}

void phase_naval_invasion(Game& g) {
    World& w = g.world;
    if (w.invasions.empty()) return;

    // Ascending army id keeps the draw sequence and the mutation order fixed.
    std::vector<ArmyId> armies;
    armies.reserve(w.invasions.size());
    for (const NavalInvasion& inv : w.invasions) armies.push_back(inv.army);
    std::sort(armies.begin(), armies.end(),
              [](ArmyId a, ArmyId b) { return a.v < b.v; });

    for (ArmyId aid : armies) {
        NavalInvasion* inv = find_invasion(w, aid);
        if (!inv) continue;
        if (advance_invasion(g, *inv)) erase_invasion(w, aid);
    }
}

}  // namespace hoi