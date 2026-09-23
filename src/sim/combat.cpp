// Incremental land combat (ARCHITECTURE section 5.5).
//
// A battle is per province and resolves once per hour: reinforcement into the
// terrain combat width, simultaneous damage in both directions, losses, retreats
// and battle end. A division that reaches zero organisation leaves the battle -
// defenders retreat (or are destroyed when surrounded) and attackers break off.
//
// Only the armour-advantage decision draws from RNG_COMBAT: exactly one chance()
// call per side per battle. Everything else is a deterministic function of state.

#include "sim/combat.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "core/math.h"
#include "core/rng.h"
#include "game/game.h"
#include "sim/politics.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

// ------------------------------------------------------------ war-side scan ---
//
// Shared by movement/combat/territory (declared in movement.cpp/territory.cpp).
// Reads only World state, so the military phases stay self-contained and
// deterministic and do not depend on the diplomacy phase's implementation.

bool mil_at_war(const World& w, CountryId a, CountryId b) {
    if (!a.valid() || !b.valid() || a == b) return false;
    bool result = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (!war.active || result) return;
        bool a_att = false, a_def = false, b_att = false, b_def = false;
        for (const WarParticipant& p : war.attackers) {
            if (p.country == a) a_att = true;
            if (p.country == b) b_att = true;
        }
        for (const WarParticipant& p : war.defenders) {
            if (p.country == a) a_def = true;
            if (p.country == b) b_def = true;
        }
        if ((a_att && b_def) || (a_def && b_att)) result = true;
    });
    return result;
}

bool mil_same_side(const World& w, CountryId a, CountryId b) {
    if (!a.valid() || !b.valid()) return false;
    if (a == b) return true;
    if (mil_at_war(w, a, b)) return false;
    bool result = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (!war.active || result) return;
        bool a_att = false, a_def = false, b_att = false, b_def = false;
        for (const WarParticipant& p : war.attackers) {
            if (p.country == a) a_att = true;
            if (p.country == b) b_att = true;
        }
        for (const WarParticipant& p : war.defenders) {
            if (p.country == a) a_def = true;
            if (p.country == b) b_def = true;
        }
        if ((a_att && b_att) || (a_def && b_def)) result = true;
    });
    return result;
}

namespace {

constexpr size_t kMaxBattleDebugLines = 64;

// Combat width per terrain (ARCHITECTURE 5.5).
double terrain_combat_width(Terrain t) {
    switch (t) {
        case Terrain::Plains: return 80.0;
        case Terrain::Forest: return 70.0;
        case Terrain::Hills: return 60.0;
        case Terrain::Mountain: return 50.0;
        case Terrain::Urban: return 80.0;
        case Terrain::Marsh: return 60.0;
        case Terrain::Desert: return 80.0;
        case Terrain::Jungle: return 50.0;
        default: return 80.0;
    }
}

// Terrain attack multiplier for the attacking side. ARCHITECTURE gives no table;
// these follow the HOI4 ordering of rough-terrain penalties.
double terrain_attack_modifier(Terrain t) {
    switch (t) {
        case Terrain::Plains: return 1.00;
        case Terrain::Forest: return 0.90;
        case Terrain::Hills: return 0.85;
        case Terrain::Mountain: return 0.70;
        case Terrain::Urban: return 0.80;
        case Terrain::Marsh: return 0.70;
        case Terrain::Desert: return 0.95;
        case Terrain::Jungle: return 0.60;
        default: return 1.00;
    }
}

// Removes a division from the world and from every list that references it.
void military_destroy_division(Game& g, DivisionId id) {
    World& w = g.world;
    Division* d = w.division(id);
    if (!d) return;
    Country* c = w.country(d->country);
    if (c) {
        c->divisions.erase(std::remove(c->divisions.begin(), c->divisions.end(), id),
                           c->divisions.end());
    }
    if (d->army.valid()) {
        Army* a = w.army(d->army);
        if (a) {
            a->divisions.erase(std::remove(a->divisions.begin(), a->divisions.end(), id),
                               a->divisions.end());
        }
    }
    w.divisions.destroy(id);
}

double commander_bonus(const Game& g, const Division& d, bool attacker) {
    if (!d.army.valid()) return 0.0;
    const Army* a = g.world.army(d.army);
    if (!a || !a->general.valid()) return 0.0;
    const Character* ch = g.world.characters.try_get(a->general);
    if (!ch) return 0.0;
    const double skill = static_cast<double>(attacker ? ch->attack : ch->defense);
    return clamp(skill * 0.02, 0.0, 0.30);
}

double side_hardness(const Game& g, const std::vector<DivisionId>& ids) {
    double num = 0.0;
    double den = 0.0;
    for (DivisionId did : ids) {
        const Division* d = g.world.division(did);
        if (!d) continue;
        const DivisionStats s = compute_division_stats(g, *d);
        const double weight = std::max(1.0, s.combat_width);
        num += s.hardness * weight;
        den += weight;
    }
    return clamp01(safe_div(num, den));
}

// Probability that this side's armour is decisive this hour. Deterministic at the
// extremes (no armour -> never; overwhelming armour -> always).
double armour_advantage_chance(double side_armor, double enemy_piercing) {
    if (!(side_armor > 0.0)) return 0.0;
    const double pierce = std::max(0.0, enemy_piercing);
    const double denom = std::max(1e-6, side_armor + pierce);
    return clamp01(0.5 + 0.5 * (side_armor - pierce) / denom);
}

struct EngagedSide {
    std::vector<DivisionId> ids;
    std::vector<double> widths;
    double total_width = 0.0;
};

EngagedSide collect_engaged(const Game& g, const BattleSideState& side) {
    EngagedSide out;
    for (size_t i = 0; i < side.divisions.size(); ++i) {
        const Division* d = g.world.division(side.divisions[i]);
        if (!d) continue;
        const double weight = i < side.width_used.size() ? side.width_used[i] : 0.0;
        if (!(weight > 0.0)) continue;
        out.ids.push_back(side.divisions[i]);
        out.widths.push_back(weight);
        out.total_width += weight;
    }
    return out;
}

// Keeps divisions in ascending id order so reinforcement is deterministic even
// after detaches and reinforcements reordered the vectors.
void sort_side(BattleSideState& side) {
    const size_t n = side.divisions.size();
    if (side.width_used.size() != n) side.width_used.resize(n, 0.0);
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return side.divisions[a] < side.divisions[b];
    });
    std::vector<DivisionId> ids(n);
    std::vector<double> widths(n);
    for (size_t i = 0; i < n; ++i) {
        ids[i] = side.divisions[order[i]];
        widths[i] = side.width_used[order[i]];
    }
    side.divisions.swap(ids);
    side.width_used.swap(widths);
}

void reinforce_side(Game& g, BattleSideState& side, double cap) {
    if (side.width_used.size() < side.divisions.size()) {
        side.width_used.resize(side.divisions.size(), 0.0);
    }
    double used = 0.0;
    for (double u : side.width_used) {
        if (u > 0.0 && std::isfinite(u)) used += u;
    }
    for (size_t i = 0; i < side.divisions.size(); ++i) {
        Division* d = g.world.division(side.divisions[i]);
        if (!d) {
            side.width_used[i] = 0.0;
            continue;
        }
        if (side.width_used[i] > 0.0) continue;  // already engaged
        double width = compute_division_stats(g, *d).combat_width;
        if (!(width > 0.0) || !std::isfinite(width)) width = 2.0;
        // At least one division always fights, even if it is wider than the cap.
        if (used <= 0.0 || used + width <= cap) {
            side.width_used[i] = width;
            used += width;
        } else {
            side.width_used[i] = 0.0;
        }
    }
}

void apply_side_damage(Game& g, const EngagedSide& target, double raw_total,
                       double org_total, double str_total,
                       std::vector<BattleDebugLine>& lines, double enemy_defense) {
    for (size_t i = 0; i < target.ids.size(); ++i) {
        Division* d = g.world.division(target.ids[i]);
        if (!d) continue;
        const double share =
            target.total_width > 0.0
                ? target.widths[i] / target.total_width
                : (target.ids.empty() ? 0.0 : 1.0 / static_cast<double>(target.ids.size()));
        const double raw = raw_total * share;
        const double org = org_total * share;
        const double str = str_total * share;
        apply_combat_losses(g, *d, org, str);
        for (BattleDebugLine& line : lines) {
            if (line.division == target.ids[i]) {
                line.enemy_defense = enemy_defense;
                line.damage = raw;
                line.org_damage = org;
                line.strength_damage = str;
                break;
            }
        }
    }
}

double side_organization_ratio(const Game& g, const EngagedSide& side) {
    if (side.ids.empty()) return 1.0;
    double sum = 0.0;
    int n = 0;
    for (DivisionId did : side.ids) {
        const Division* d = g.world.division(did);
        if (!d) continue;
        sum += clamp01(safe_div(d->organization, std::max(1.0, d->max_organization)));
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 1.0;
}

double side_strength_ratio(const Game& g, const EngagedSide& side) {
    if (side.ids.empty()) return 1.0;
    double sum = 0.0;
    int n = 0;
    for (DivisionId did : side.ids) {
        const Division* d = g.world.division(did);
        if (!d) continue;
        sum += clamp01(d->strength);
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 1.0;
}

double side_supply_ratio(const Game& g, const EngagedSide& side) {
    if (side.ids.empty()) return 1.0;
    double sum = 0.0;
    int n = 0;
    for (DivisionId did : side.ids) {
        const Division* d = g.world.division(did);
        if (!d) continue;
        sum += clamp01(d->supply);
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 1.0;
}

// Attacker progress 0..1 from the defenders' remaining organisation and strength.
double attacker_progress(const Game& g, const EngagedSide& defenders) {
    if (defenders.ids.empty()) return 1.0;
    const double org = side_organization_ratio(g, defenders);
    const double str = side_strength_ratio(g, defenders);
    return clamp01(1.0 - 0.5 * (org + str));
}

void handle_retreats(Game& g, const std::vector<DivisionId>& attackers,
                     const std::vector<DivisionId>& defenders) {
    World& w = g.world;
    const double threshold = g.content.constants.battle_retreat_org_threshold;

    for (DivisionId did : attackers) {
        Division* d = w.division(did);
        if (!d) continue;
        if (d->organization <= threshold) {
            detach_from_battle(g, *d);
            d->planning = 0.0;  // the attack failed; the plan has to be rebuilt
        }
    }

    for (DivisionId did : defenders) {
        Division* d = w.division(did);
        if (!d) continue;
        if (d->organization > threshold) continue;
        const ProvinceId retreat = choose_retreat_province(g, *d);
        detach_from_battle(g, *d);
        if (!retreat.valid()) {
            military_destroy_division(g, did);  // surrounded: destroyed
            continue;
        }
        d->previous_location = d->location;
        d->location = retreat;
        d->retreating = true;
        d->moving = false;
        d->path.clear();
        d->move_progress = 0.0;
        d->move_from = ProvinceId{};
        d->move_to = ProvinceId{};
        d->entrenchment = 0.0;
    }
}

// The attackers took the province: they advance into it, which is what makes the
// territory phase transfer control.
void attacker_wins(Game& g, Battle& b) {
    World& w = g.world;
    for (DivisionId did : b.attacker.divisions) {
        Division* d = w.division(did);
        if (!d) continue;
        d->battle = BattleId{};
        if (d->location == b.province) continue;  // already inside
        d->previous_location = d->location;
        d->location = b.province;
        d->moving = false;
        d->path.clear();
        d->move_progress = 0.0;
        d->move_from = ProvinceId{};
        d->move_to = ProvinceId{};
        d->retreating = false;
        if (d->order_target == b.province) d->order_target = ProvinceId{};
    }
    w.battles.destroy(b.id);
}

void defender_wins(Game& g, Battle& b) {
    World& w = g.world;
    for (DivisionId did : b.defender.divisions) {
        Division* d = w.division(did);
        if (d) d->battle = BattleId{};
    }
    w.battles.destroy(b.id);
}

}  // namespace

// ------------------------------------------------------------ statistics -----

DivisionStats compute_division_stats(const Game& g, const Division& d) {
    DivisionStats out;
    const DivisionTemplate* t = g.content.template_def(d.template_id);
    if (!t) return out;

    // Equipment required per EquipmentId, so a shared equipment id is counted once
    // when scaling (a division may mount the same model in several battalions).
    std::vector<double> required(g.content.equipment.size(), 0.0);
    for (const BattalionSlot& slot : t->battalions) {
        if (slot.count <= 0 || !slot.equipment.valid()) continue;
        if (slot.equipment.v >= required.size()) continue;
        required[slot.equipment.v] += static_cast<double>(slot.count);
    }

    auto present_of = [&](EquipmentId e) -> double {
        if (!e.valid() || e.v >= d.equipment.size()) return 0.0;
        const double v = d.equipment[e.v];
        return (std::isfinite(v) && v > 0.0) ? v : 0.0;
    };
    auto fill_ratio = [&](EquipmentId e) -> double {
        if (!e.valid() || e.v >= required.size() || required[e.v] <= 0.0) return 0.0;
        return clamp01(safe_div(present_of(e), required[e.v]));
    };

    double soft = 0.0, hard = 0.0, defense = 0.0, breakthrough = 0.0;
    double armor = 0.0, piercing = 0.0;
    double hardness_num = 0.0, hardness_den = 0.0;

    for (const BattalionSlot& slot : t->battalions) {
        if (slot.count <= 0) continue;
        const EquipmentDef* e = g.content.equipment_def(slot.equipment);
        const double ratio = fill_ratio(slot.equipment);
        if (!e) continue;
        const double n = static_cast<double>(slot.count);
        soft += e->soft_attack * n * ratio;
        hard += e->hard_attack * n * ratio;
        defense += e->defense * n * ratio;
        breakthrough += e->breakthrough * n * ratio;
        if (e->armor * ratio > armor) armor = e->armor * ratio;
        if (e->piercing * ratio > piercing) piercing = e->piercing * ratio;
        hardness_num += e->hardness * n * ratio;
        hardness_den += n * ratio;
    }

    double fill_num = 0.0, fill_den = 0.0;
    for (size_t v = 0; v < required.size(); ++v) {
        if (required[v] <= 0.0) continue;
        const double present =
            v < d.equipment.size() && std::isfinite(d.equipment[v]) && d.equipment[v] > 0.0
                ? d.equipment[v]
                : 0.0;
        fill_num += std::min(present, required[v]);
        fill_den += required[v];
    }
    const double fill = clamp01(safe_div(fill_num, fill_den));

    Modifiers mods;
    if (const Country* c = g.world.country(d.country)) mods = c->total_modifiers();

    out.soft_attack = soft;
    out.hard_attack = hard;
    out.defense = defense;
    out.breakthrough = breakthrough;
    out.armor = armor;
    out.piercing = piercing;
    out.hardness = clamp01(safe_div(hardness_num, hardness_den));
    out.combat_width = t->combat_width * mods.factor(ModifierKind::CombatWidth);
    out.max_organization = t->max_organization * mods.factor(ModifierKind::DivisionOrganization);
    out.max_strength = t->max_strength * clamp01(d.strength);
    out.speed = t->speed;
    out.supply_use = t->supply_use * fill * mods.factor(ModifierKind::SupplyConsumption);
    out.fuel_use = t->fuel_use * fill;
    out.max_manpower = t->manpower * clamp01(d.strength);
    return out;
}

SideCombatValues compute_side_values(const Game& g, const Battle& b,
                                     const std::vector<DivisionId>& divisions,
                                     bool attacker, std::vector<BattleDebugLine>* debug) {
    SideCombatValues out;
    if (debug) debug->clear();

    // Enemy hardness is the width-weighted average of the opposing side's troops.
    const std::vector<DivisionId>& enemy_list =
        attacker ? b.defender.divisions : b.attacker.divisions;
    const double enemy_hardness = side_hardness(g, enemy_list);

    double terrain_mod = attacker ? terrain_attack_modifier(b.terrain) : 1.0;
    if (attacker && b.river_crossing) terrain_mod *= 0.8;

    // Attacking into bad weather costs attack value (politics.h / weather.cpp).
    double weather_penalty = 0.0;
    if (attacker) {
        const Province* bp = g.world.province(b.province);
        const Region* reg = bp ? g.world.regions.try_get(bp->region) : nullptr;
        if (reg) weather_penalty = std::max(0.0, weather_attack_penalty(*reg));
    }

    double armor_sum = 0.0;
    double piercing_sum = 0.0;

    for (DivisionId did : divisions) {
        const Division* dp = g.world.division(did);
        if (!dp) continue;
        const DivisionStats s = compute_division_stats(g, *dp);
        Modifiers mods;
        if (const Country* c = g.world.country(dp->country)) mods = c->total_modifiers();

        const double planning_mod =
            attacker ? clamp01(dp->planning) * g.content.constants.planning_max_attack_bonus : 0.0;
        const double supply_mod = clamp01(dp->supply);
        const double experience_mod = clamp01(dp->experience);
        const double commander = commander_bonus(g, *dp, attacker);
        const double attack_mod = mods.get(ModifierKind::DivisionAttack);
        const double org_factor =
            clamp01(safe_div(dp->organization, std::max(1e-9, s.max_organization)));
        const double mult = (1.0 + attack_mod + planning_mod + experience_mod + commander -
                             weather_penalty) *
                            terrain_mod * supply_mod * org_factor;

        const double base_attack =
            s.soft_attack * (1.0 - enemy_hardness) + s.hard_attack * enemy_hardness;
        out.soft_attack += s.soft_attack * mult;
        out.hard_attack += s.hard_attack * mult;

        const double defense_skill =
            attacker ? mods.get(ModifierKind::DivisionBreakthrough) : mods.get(ModifierKind::DivisionDefense);
        const double defense_value =
            (attacker ? s.breakthrough : s.defense) * (1.0 + defense_skill) * supply_mod;
        if (attacker) {
            out.breakthrough += defense_value;
        } else {
            out.defense += defense_value;
        }

        const double weight = std::max(0.0, s.combat_width);
        armor_sum += s.armor * weight;
        piercing_sum += s.piercing * weight;
        out.width += weight;
        out.hp += s.max_strength;

        if (debug && debug->size() < kMaxBattleDebugLines) {
            BattleDebugLine line;
            line.division = did;
            line.base_attack = base_attack;
            line.planning_mod = planning_mod;
            line.terrain_mod = terrain_mod;
            line.supply_mod = supply_mod;
            line.commander_mod = commander;
            line.experience_mod = experience_mod;
            line.final_attack = base_attack * (1.0 + attack_mod + planning_mod + experience_mod +
                                               commander - weather_penalty) *
                                terrain_mod * supply_mod * org_factor;
            debug->push_back(line);
        }
    }

    out.armor = safe_div(armor_sum, out.width);
    out.piercing = safe_div(piercing_sum, out.width);
    return out;
}

// --------------------------------------------------------------- battles -----

BattleId start_battle(Game& g, ProvinceId province, CountryId attacker_lead,
                      CountryId defender_lead, const std::vector<DivisionId>& attackers,
                      const std::vector<DivisionId>& defenders) {
    World& w = g.world;

    BattleId id;
    w.battles.for_each([&](BattleId bid, const Battle& bb) {
        if (!id.valid() && bb.province == province) id = bid;
    });

    if (!id.valid()) {
        id = w.battles.create();
        Battle* nb = w.battles.try_get(id);
        nb->id = id;
        nb->province = province;
        nb->start_tick = w.tick;
        nb->last_tick = w.tick;
        const Province* p = w.province(province);
        nb->terrain = p ? p->terrain : Terrain::Plains;
        nb->river_crossing = p && p->terrain == Terrain::Marsh;
        nb->attacker_lead = attacker_lead;
        nb->defender_lead = defender_lead;
    } else {
        Battle* eb = w.battles.try_get(id);
        if (!eb->attacker_lead.valid()) eb->attacker_lead = attacker_lead;
        if (!eb->defender_lead.valid()) eb->defender_lead = defender_lead;
    }

    Battle* b = w.battles.try_get(id);
    if (!b) return BattleId{};

    auto add = [&](BattleSideState& side, const std::vector<DivisionId>& ids, bool is_attacker) {
        for (DivisionId did : ids) {
            Division* d = w.division(did);
            if (!d) continue;
            if (d->battle.valid() && d->battle != id) continue;  // already fighting elsewhere
            bool present = false;
            for (DivisionId x : side.divisions) {
                if (x == did) {
                    present = true;
                    break;
                }
            }
            if (present) continue;
            side.divisions.push_back(did);
            side.width_used.push_back(0.0);
            d->battle = id;
            // An attacker joins from an adjacent province and commits: it stops
            // marching so the two phases cannot fight over the same division.
            if (is_attacker && d->location != province) {
                d->moving = false;
                d->path.clear();
                d->move_progress = 0.0;
                d->move_from = ProvinceId{};
                d->move_to = ProvinceId{};
            }
        }
    };
    add(b->attacker, attackers, true);
    add(b->defender, defenders, false);
    return id;
}

void detach_from_battle(Game& g, Division& d) {
    if (!d.battle.valid()) return;
    if (Battle* b = g.world.battle(d.battle)) {
        for (BattleSideState* side : {&b->attacker, &b->defender}) {
            for (size_t i = 0; i < side->divisions.size(); ++i) {
                if (side->divisions[i] == d.id) {
                    side->divisions.erase(side->divisions.begin() + static_cast<long>(i));
                    if (i < side->width_used.size()) {
                        side->width_used.erase(side->width_used.begin() + static_cast<long>(i));
                    }
                    break;
                }
            }
        }
    }
    d.battle = BattleId{};
}

ProvinceId choose_retreat_province(const Game& g, const Division& d) {
    const World& w = g.world;
    const Province* here = w.province(d.location);
    if (!here) return ProvinceId{};

    // `adj` is documented as sorted by province id, so the first legal candidate
    // is deterministic.
    for (ProvinceId p : here->adj) {
        const Province* cand = w.province(p);
        if (!cand || cand->is_sea) continue;
        const State* st = w.state(cand->state);
        if (st && st->impassable) continue;
        if (!mil_same_side(w, d.country, cand->controller)) continue;
        bool hostile_present = false;
        w.divisions.for_each([&](DivisionId, const Division& other) {
            if (hostile_present || !other.location.valid()) return;
            if (other.location == p && mil_at_war(w, other.country, d.country)) {
                hostile_present = true;
            }
        });
        if (hostile_present) continue;
        return p;
    }
    return ProvinceId{};
}

void apply_combat_losses(Game& g, Division& d, double org_damage, double strength_damage) {
    if (!std::isfinite(org_damage) || org_damage < 0.0) org_damage = 0.0;
    if (!std::isfinite(strength_damage) || strength_damage < 0.0) strength_damage = 0.0;

    d.organization = std::max(0.0, d.organization - org_damage);

    const DivisionTemplate* t = g.content.template_def(d.template_id);
    double max_hp = t ? t->max_strength : 0.0;
    if (!(max_hp > 0.0) || !std::isfinite(max_hp)) max_hp = 1.0;

    const double old_strength = clamp01(d.strength);
    const double new_strength = clamp01(old_strength - safe_div(strength_damage, max_hp));
    const double keep = old_strength > 0.0 ? clamp01(safe_div(new_strength, old_strength)) : 0.0;

    double equipment_before = 0.0;
    double equipment_after = 0.0;
    for (double& q : d.equipment) {
        if (!std::isfinite(q) || q < 0.0) q = 0.0;
        equipment_before += q;
        q *= keep;
        equipment_after += q;
    }

    double manpower_lost = 0.0;
    if (t) manpower_lost = std::max(0.0, t->manpower) * (old_strength - new_strength);
    d.manpower = std::max(0.0, d.manpower - manpower_lost);

    d.losses_manpower += manpower_lost;
    d.losses_equipment += std::max(0.0, equipment_before - equipment_after);
    d.strength = new_strength;
}

// ------------------------------------------------------------ phase ----------

namespace {

// Starts battles for every province where hostile forces meet: divisions in the
// province defend it, divisions ordered onto it from an adjacent province attack.
void start_new_battles(Game& g) {
    World& w = g.world;

    std::map<ProvinceId, std::vector<CountryId>> occupants;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (!d.location.valid()) return;
        if (!w.province(d.location)) return;
        occupants[d.location].push_back(d.country);
    });

    auto has_hostile = [&](ProvinceId p, CountryId c) -> bool {
        auto it = occupants.find(p);
        if (it == occupants.end()) return false;
        for (CountryId oc : it->second) {
            if (mil_at_war(w, oc, c)) return true;
        }
        return false;
    };

    std::vector<ProvinceId> candidates;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (!d.location.valid() || !d.order_target.valid()) return;
        if (has_hostile(d.order_target, d.country)) candidates.push_back(d.order_target);
    });
    for (auto& entry : occupants) {
        const std::vector<CountryId>& countries = entry.second;
        bool hostile_pair = false;
        for (size_t i = 0; i < countries.size() && !hostile_pair; ++i) {
            for (size_t j = i + 1; j < countries.size(); ++j) {
                if (mil_at_war(w, countries[i], countries[j])) {
                    hostile_pair = true;
                    break;
                }
            }
        }
        if (hostile_pair) candidates.push_back(entry.first);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (ProvinceId p : candidates) {
        const Province* prov = w.province(p);
        if (!prov) continue;

        // The holding side is whoever controls the province and has troops there;
        // otherwise the lowest-id country standing in it defends.
        CountryId holder = prov->controller;
        bool holder_present = false;
        w.divisions.for_each([&](DivisionId, const Division& d) {
            if (holder_present || !d.location.valid()) return;
            if (d.location == p && mil_same_side(w, d.country, holder)) holder_present = true;
        });
        if (!holder_present) {
            bool found = false;
            w.divisions.for_each([&](DivisionId, const Division& d) {
                if (!d.location.valid() || d.location != p) return;
                if (!found || d.country.v < holder.v) {
                    holder = d.country;
                    found = true;
                }
            });
        }

        std::vector<DivisionId> attackers;
        std::vector<DivisionId> defenders;
        w.divisions.for_each([&](DivisionId did, const Division& d) {
            if (!d.location.valid()) return;
            if (d.location == p) {
                if (mil_same_side(w, d.country, holder)) {
                    defenders.push_back(did);
                } else {
                    attackers.push_back(did);
                }
            } else if (d.order_target == p && mil_at_war(w, d.country, holder)) {
                attackers.push_back(did);
            }
        });
        if (defenders.empty() || attackers.empty()) continue;

        const Division* first = w.division(attackers.front());
        if (!first) continue;
        start_battle(g, p, first->country, holder, attackers, defenders);
    }
}

void reinforce_battles(Game& g) {
    g.world.battles.for_each([&](BattleId, Battle& b) {
        sort_side(b.attacker);
        sort_side(b.defender);
        const double cap = terrain_combat_width(b.terrain);
        reinforce_side(g, b.attacker, cap);
        reinforce_side(g, b.defender, cap);
    });
}

void resolve_battles(Game& g) {
    World& w = g.world;
    const SimConstants& k = g.content.constants;

    w.battles.for_each([&](BattleId, Battle& b) {
        EngagedSide att = collect_engaged(g, b.attacker);
        EngagedSide def = collect_engaged(g, b.defender);

        if (!att.ids.empty() && !def.ids.empty()) {
            const double att_hardness = side_hardness(g, att.ids);
            const double def_hardness = side_hardness(g, def.ids);

            std::vector<BattleDebugLine> att_lines;
            std::vector<BattleDebugLine> def_lines;
            const SideCombatValues a_vals = compute_side_values(g, b, att.ids, true, &att_lines);
            const SideCombatValues d_vals = compute_side_values(g, b, def.ids, false, &def_lines);

            const double attack_att =
                a_vals.soft_attack * (1.0 - def_hardness) + a_vals.hard_attack * def_hardness;
            const double attack_def =
                d_vals.soft_attack * (1.0 - att_hardness) + d_vals.hard_attack * att_hardness;
            const double def_att = a_vals.breakthrough;  // attackers defend with breakthrough
            const double def_def = d_vals.defense;

            const double mit_def = safe_div(def_def, def_def + attack_att);
            const double mit_att = safe_div(def_att, def_att + attack_def);
            double raw_to_def = attack_att * k.damage_scale * (1.0 - mit_def);
            double raw_to_att = attack_def * k.damage_scale * (1.0 - mit_att);

            // Exactly one RNG_COMBAT draw per side per battle (see file header).
            Rng& rng = g.rng.get(RngStream::Combat);
            const bool att_armour = rng.chance(armour_advantage_chance(a_vals.armor, d_vals.piercing));
            const bool def_armour = rng.chance(armour_advantage_chance(d_vals.armor, a_vals.piercing));
            if (att_armour) {
                raw_to_def *= k.armor_advantage_multiplier;
                raw_to_att *= k.armor_disadvantage_multiplier;
            }
            if (def_armour) {
                raw_to_att *= k.armor_advantage_multiplier;
                raw_to_def *= k.armor_disadvantage_multiplier;
            }

            const double org_to_def = raw_to_def * k.org_damage_share;
            const double org_to_att = raw_to_att * k.org_damage_share;
            const double str_to_def = raw_to_def * k.strength_damage_share;
            const double str_to_att = raw_to_att * k.strength_damage_share;

            apply_side_damage(g, def, raw_to_def, org_to_def, str_to_def, def_lines, def_att);
            apply_side_damage(g, att, raw_to_att, org_to_att, str_to_att, att_lines, def_def);

            b.attacker.total_soft_attack = a_vals.soft_attack;
            b.attacker.total_hard_attack = a_vals.hard_attack;
            b.attacker.total_breakthrough = a_vals.breakthrough;
            b.attacker.total_armor = a_vals.armor;
            b.attacker.total_piercing = a_vals.piercing;
            b.defender.total_soft_attack = d_vals.soft_attack;
            b.defender.total_hard_attack = d_vals.hard_attack;
            b.defender.total_defense = d_vals.defense;
            b.defender.total_armor = d_vals.armor;
            b.defender.total_piercing = d_vals.piercing;

            b.progress = attacker_progress(g, def);
            b.encirclement = side_supply_ratio(g, def) < 0.1;
            b.last_tick = w.tick;

            b.debug.clear();
            for (const BattleDebugLine& line : att_lines) {
                if (b.debug.size() >= kMaxBattleDebugLines) break;
                b.debug.push_back(line);
            }
            for (const BattleDebugLine& line : def_lines) {
                if (b.debug.size() >= kMaxBattleDebugLines) break;
                b.debug.push_back(line);
            }

            // Planning is consumed once the offensive is executed.
            for (DivisionId did : att.ids) {
                Division* d = w.division(did);
                if (d) d->planning = 0.0;
            }
        }

        handle_retreats(g, att.ids, def.ids);

        if (b.attacker.divisions.empty() && b.defender.divisions.empty()) {
            w.battles.destroy(b.id);
            return;
        }
        if (b.defender.divisions.empty()) {
            attacker_wins(g, b);
            return;
        }
        if (b.attacker.divisions.empty()) {
            defender_wins(g, b);
            return;
        }
    });
}

}  // namespace

void phase_combat(Game& g) {
    World& w = g.world;
    start_new_battles(g);
    reinforce_battles(g);
    resolve_battles(g);

    // Organisation recovery for everything not currently fighting.
    w.divisions.for_each([&](DivisionId, Division& d) {
        if (!d.location.valid()) return;  // off-map (training)
        recover_organization(g, d);
    });

    // Weather attrition: on-map divisions in a region whose weather is
    // attritional lose strength and manpower every hour. Applied here (combat) so
    // the loss flows through equipment and manpower consistently.
    w.divisions.for_each([&](DivisionId, Division& d) {
        if (!d.location.valid()) return;
        const Province* p = w.province(d.location);
        const Region* r = p ? w.regions.try_get(p->region) : nullptr;
        if (!r) return;
        const double per_day = weather_attrition_per_day(*r);
        if (!(per_day > 0.0) || !std::isfinite(per_day)) return;
        const DivisionTemplate* t = g.content.template_def(d.template_id);
        const double max_hp = (t && t->max_strength > 0.0) ? t->max_strength : 1.0;
        apply_combat_losses(g, d, 0.0,
                            max_hp * per_day / static_cast<double>(TICKS_PER_DAY));
    });
}

void recover_organization(Game& g, Division& d) {
    if (d.in_combat()) return;
    if (!d.location.valid()) return;

    Modifiers mods;
    if (const Country* c = g.world.country(d.country)) mods = c->total_modifiers();

    const double supply = clamp01(d.supply);
    const double gain = g.content.constants.org_recovery_base *
                        (1.0 + mods.get(ModifierKind::DivisionRecoveryRate)) * supply;
    const double max_org = std::max(1.0, d.max_organization);
    d.organization = clamp(finite_or(d.organization, 0.0) + gain, 0.0, max_org);

    if (!d.moving && !d.retreating) {
        const double entrench =
            g.content.constants.entrenchment_per_day / TICKS_PER_DAY *
            (1.0 + mods.get(ModifierKind::EntrenchmentSpeed));
        d.entrenchment = clamp01(d.entrenchment + entrench);

        const Army* a = g.world.army(d.army);
        if (a && a->order.kind == OrderKind::Offensive) {
            const double plan = g.content.constants.planning_per_day / TICKS_PER_DAY *
                                (1.0 + mods.get(ModifierKind::PlanningSpeed));
            const double cap = 1.0 + mods.get(ModifierKind::MaxPlanning);
            d.planning = clamp(d.planning + plan, 0.0, cap);
        }
    }
}

}  // namespace hoi
