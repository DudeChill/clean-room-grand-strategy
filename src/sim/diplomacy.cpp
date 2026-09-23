// Diplomacy, war, occupation (spec sections 60-63, 174-176).
//
// War is modelled as a participant list per side, not as a pairwise flag: declaring
// war pulls in faction members, puppets, overlords and guarantors, and every later
// query (co-belligerence, supply sharing, capitulation, peace) reads the same list.
// All iteration is ascending id or an already ordered container.

#include "sim/diplomacy.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/combat.h"

namespace hoi {
namespace {

// Diplomatic tuning has no SimConstants fields yet (see report); these are the
// engine-side defaults, applied additively like every other rate in the game.
constexpr double kRelationDriftPerDay = 0.5;
constexpr double kWarRelationTarget = -50.0;
constexpr double kWarRelationCeiling = -50.0;
constexpr double kPostWarRelationBaseline = -25.0;
constexpr double kWarUpkeepPoliticalPowerPerDay = 0.25;
constexpr double kIndustryLossThreshold = 0.70;  // factories lost before capitulation
constexpr double kFactionRelationFloor = 25.0;   // joining a faction warms relations

// Occupation model.
constexpr double kResistanceGrowthPerDay = 0.50;
constexpr double kComplianceGrowthPerDay = 0.03;
constexpr double kCoreResistanceDecayPerDay = 0.20;
constexpr double kCoreComplianceDecayPerDay = 0.05;
constexpr double kGarrisonPerResistancePopulation = 0.02;

// 0 = attacker side, 1 = defender side, -1 = not a participant.
int side_of(const War& war, CountryId c) {
    for (const WarParticipant& p : war.attackers)
        if (p.country == c) return 0;
    for (const WarParticipant& p : war.defenders)
        if (p.country == c) return 1;
    return -1;
}

bool country_alive(const World& w, CountryId c) {
    const Country* cc = w.country(c);
    return cc != nullptr && cc->alive;
}

// The faction record a country belongs to (by Country::faction), or nullptr.
const Faction* faction_record(const World& w, CountryId c) {
    const Country* cc = w.country(c);
    if (cc == nullptr || cc->faction == 0) return nullptr;
    for (const Faction& f : w.factions) {
        if (f.id == cc->faction) return &f;
    }
    return nullptr;
}

Faction* faction_record(World& w, uint32_t faction_id) {
    if (faction_id == 0) return nullptr;
    for (Faction& f : w.factions) {
        if (f.id == faction_id) return &f;
    }
    return nullptr;
}

// Faction members are kept in ascending country id so every consumer that walks the
// list (coalitions, co-belligerence, save order) sees a stable order.
void insert_member(Faction& f, CountryId c) {
    const auto it = std::lower_bound(f.members.begin(), f.members.end(), c);
    if (it == f.members.end() || *it != c) f.members.insert(it, c);
}

bool in_war(const World& w, CountryId c) {
    bool found = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (found || !war.active) return;
        if (side_of(war, c) >= 0) found = true;
    });
    return found;
}

// Keeps Country::wars and Country::at_war consistent with the active war list.
void refresh_at_war(World& w, CountryId c) {
    Country* cc = w.country(c);
    if (cc == nullptr || !cc->alive) return;
    std::vector<WarId> live;
    for (WarId wid : cc->wars) {
        const War* war = w.war(wid);
        if (war == nullptr || !war->active) continue;
        if (side_of(*war, c) < 0) continue;
        live.push_back(wid);
    }
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());
    cc->at_war = !live.empty();  // read before the move: a moved-from vector is empty
    cc->wars = std::move(live);
}

// Everyone who joins `root`'s side when it goes to war: itself, its puppets, its
// overlord, its faction and its guarantors. Ascending, deduplicated, alive only.
std::vector<CountryId> coalition(const World& w, CountryId root) {
    std::vector<CountryId> out;
    auto push = [&](CountryId c) {
        if (country_alive(w, c)) out.push_back(c);
    };
    push(root);
    const Country* rc = w.country(root);
    if (rc != nullptr) {
        for (CountryId p : rc->puppets) push(p);
        push(rc->overlord);
        if (const Faction* f = faction_record(w, root)) {
            for (CountryId m : f->members) push(m);
        }
    }
    w.countries.for_each([&](CountryId gid, const Country& g) {
        if (!g.alive) return;
        const Relation* r = w.find_relation(gid, root);
        if (r != nullptr && r->guarantee) out.push_back(gid);
    });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<CountryId> participants(const War& war, bool attacker_side) {
    std::vector<CountryId> out;
    const std::vector<WarParticipant>& list = attacker_side ? war.attackers : war.defenders;
    for (const WarParticipant& p : list) out.push_back(p.country);
    return out;
}

// The capital province: the flagged one, else the first land province of the
// capital state.
ProvinceId capital_province(const World& w, CountryId c) {
    const Country* cc = w.country(c);
    if (cc == nullptr || !cc->capital.valid()) return ProvinceId{};
    const State* st = w.state(cc->capital);
    if (st == nullptr) return ProvinceId{};
    ProvinceId first{};
    for (ProvinceId pid : st->provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr || p->is_sea) continue;
        if (p->is_capital) return pid;
        if (!first.valid()) first = pid;
    }
    return first;
}

int factories_in_states(const World& w, CountryId c, bool by_owner) {
    int total = 0;
    w.states.for_each([&](StateId, const State& s) {
        const CountryId holder = by_owner ? s.owner : s.controller;
        if (holder == c) total += s.total_factories();
    });
    return total;
}

// Every province of `state` is held by a country on `side` of this war.
bool state_held_by_side(const World& w, const War& war, StateId state, int side) {
    const State* st = w.state(state);
    if (st == nullptr || st->provinces.empty()) return false;
    bool any = false;
    for (ProvinceId pid : st->provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr || p->is_sea) continue;
        any = true;
        if (side_of(war, p->controller) != side) return false;
    }
    return any;
}

// At least one settled claim of `side` must be fully held for a decisive verdict.
bool side_holds_all_goals(const World& w, const War& war, int side) {
    bool any = false;
    for (const WarGoal& goal : war.goals) {
        if (side_of(war, goal.claimant) != side) continue;
        if (!goal.state.valid()) continue;
        any = true;
        if (!state_held_by_side(w, war, goal.state, side)) return false;
    }
    return any;
}

bool aggressor_capital_lost(const World& w, const War& war) {
    const int aggressor_side = side_of(war, war.aggressor);
    if (aggressor_side < 0) return false;
    const ProvinceId capital = capital_province(w, war.aggressor);
    if (!capital.valid()) return false;
    const Province* p = w.province(capital);
    if (p == nullptr || !p->controller.valid() || p->controller == war.aggressor) return false;
    const int holder_side = side_of(war, p->controller);
    return holder_side >= 0 && holder_side != aggressor_side;
}

bool all_provinces_held_by_side(const World& w, const War& war, CountryId target, int side) {
    const Country* t = w.country(target);
    if (t == nullptr) return false;
    bool any = false;
    bool held = true;
    w.provinces.for_each([&](ProvinceId, const Province& p) {
        if (p.is_sea || p.owner != target) return;
        any = true;
        if (side_of(war, p.controller) != side) held = false;
    });
    return any && held;
}

double state_population(const World& w, const State& s) {
    double total = 0.0;
    for (ProvinceId pid : s.provinces) {
        const Province* p = w.province(pid);
        if (p != nullptr && std::isfinite(p->population) && p->population > 0.0) {
            total += p->population;
        }
    }
    return total;
}

bool state_is_core(const State& s, CountryId c) {
    if (s.owner == c) return true;
    return std::find(s.core_owners.begin(), s.core_owners.end(), c) != s.core_owners.end();
}

// Occupation burden: the share of `war`, owned by the opposing side, that
// `participant` currently controls.
double occupation_share(const World& w, const War& war, CountryId participant) {
    const int side = side_of(war, participant);
    if (side < 0) return 0.0;
    double total = 0.0;
    double held = 0.0;
    w.provinces.for_each([&](ProvinceId, const Province& p) {
        if (p.is_sea || !p.owner.valid()) return;
        const int owner_side = side_of(war, p.owner);
        if (owner_side < 0 || owner_side == side) return;
        total += 1.0;
        if (p.controller == participant) held += 1.0;
    });
    return clamp01(safe_div(held, total));
}

}  // namespace

WarId declare_war(Game& g, CountryId aggressor, CountryId target,
                  const std::vector<WarGoal>& goals) {
    World& w = g.world;
    if (!aggressor.valid() || !target.valid() || aggressor == target) return WarId{};
    const Country* a = w.country(aggressor);
    const Country* t = w.country(target);
    if (a == nullptr || t == nullptr || !a->alive || !t->alive) return WarId{};

    // Idempotent per pair: an active war already opposing the two is returned as is.
    WarId existing{};
    w.wars.for_each([&](WarId wid, const War& war) {
        if (existing.valid() || !war.active) return;
        const int sa = side_of(war, aggressor);
        const int sb = side_of(war, target);
        if (sa >= 0 && sb >= 0 && sa != sb) existing = wid;
    });
    if (existing.valid()) return existing;

    std::vector<CountryId> attackers = coalition(w, aggressor);
    std::vector<CountryId> defenders = coalition(w, target);
    // Nobody can be on both sides: the aggressor's coalition keeps the country.
    defenders.erase(std::remove_if(defenders.begin(), defenders.end(),
                                   [&](CountryId c) {
                                       return std::find(attackers.begin(), attackers.end(), c) !=
                                              attackers.end();
                                   }),
                    defenders.end());

    // A guaranteed country calls its guarantors, and those guarantees are spent: an
    // invoked pact is cleared on both sides.
    for (CountryId root : {aggressor, target}) {
        for (auto& entry : w.relations) {
            if (!entry.second.guarantee) continue;
            if (entry.first.first != root.v && entry.first.second != root.v) continue;
            entry.second.guarantee = false;
        }
    }

    War war;
    war.aggressor = aggressor;
    war.start_tick = w.tick;
    war.active = true;
    for (CountryId c : attackers) war.attackers.push_back(WarParticipant{c});
    for (CountryId c : defenders) war.defenders.push_back(WarParticipant{c});
    war.goals = goals;
    if (war.goals.empty()) {
        // A war without a stated goal is still settled somewhere: the target's
        // capital is the default war aim so peace terms always have a subject.
        WarGoal goal;
        goal.claimant = aggressor;
        goal.target = target;
        goal.state = t->capital;
        war.goals.push_back(goal);
    }
    const WarId wid = w.wars.create(std::move(war));

    auto enrol = [&](CountryId c) {
        Country* cc = w.country(c);
        if (cc == nullptr || !cc->alive) return;
        if (std::find(cc->wars.begin(), cc->wars.end(), wid) == cc->wars.end()) {
            cc->wars.push_back(wid);
        }
        cc->at_war = true;
    };
    for (CountryId c : attackers) enrol(c);
    for (CountryId c : defenders) enrol(c);

    // Every cross pair is at war; pre-war agreements lapse and relations drop to
    // the wartime ceiling.
    for (CountryId at : attackers) {
        for (CountryId def : defenders) {
            Relation& r = w.relation(at, def);
            r.at_war = true;
            r.guarantee = false;
            r.non_aggression = false;
            r.military_access = false;
            if (r.value > kWarRelationCeiling) r.value = kWarRelationCeiling;
        }
    }

    std::string text = a->tag + " declares war on " + t->tag;
    g.log_event("war", text, aggressor);
    return wid;
}

bool countries_at_war(const World& w, CountryId a, CountryId b) {
    if (!a.valid() || !b.valid() || a == b) return false;
    bool found = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (found || !war.active) return;
        const int sa = side_of(war, a);
        const int sb = side_of(war, b);
        if (sa >= 0 && sb >= 0 && sa != sb) found = true;
    });
    return found;
}

uint32_t faction_of(const World& w, CountryId leader) {
    if (!leader.valid()) return 0;
    for (const Faction& f : w.factions) {
        if (f.leader == leader) return f.id;
    }
    return 0;
}

bool join_faction(Game& g, CountryId who, CountryId faction_leader) {
    World& w = g.world;
    if (!who.valid() || !faction_leader.valid() || who == faction_leader) return false;
    Country* joiner = w.country(who);
    const Country* lead = w.country(faction_leader);
    if (joiner == nullptr || !joiner->alive) return false;
    if (lead == nullptr || !lead->alive) return false;
    if (joiner->faction != 0) return false;              // already in a faction
    if (joiner->ideology != lead->ideology) return false;  // only like-minded join
    if (countries_at_war(w, who, faction_leader)) return false;

    const uint32_t id = faction_of(w, faction_leader);
    Faction* faction = faction_record(w, id);
    if (faction == nullptr) return false;

    insert_member(*faction, who);
    joiner->faction = id;
    // Membership is a warm relationship with the leader at minimum.
    Relation& r = w.relation(who, faction_leader);
    if (r.value < kFactionRelationFloor) r.value = kFactionRelationFloor;

    g.log_event("faction", joiner->tag + " joins faction of " + lead->tag, who);
    return true;
}

bool leave_faction(Game& g, CountryId who) {
    World& w = g.world;
    Country* member = w.country(who);
    if (member == nullptr || !member->alive) return false;
    const uint32_t id = member->faction;
    if (id == 0) return false;
    Faction* faction = faction_record(w, id);
    if (faction == nullptr) {
        member->faction = 0;  // dangling reference: repair rather than fail
        return false;
    }

    faction->members.erase(std::remove(faction->members.begin(), faction->members.end(), who),
                           faction->members.end());
    member->faction = 0;
    std::string text = member->tag + " leaves faction";
    if (faction->leader == who) {
        // The lowest-id remaining member takes over; an empty faction ceases to exist.
        const std::vector<CountryId> ids = faction->members;
        if (ids.empty()) {
            w.factions.erase(std::remove_if(w.factions.begin(), w.factions.end(),
                                            [id](const Faction& f) { return f.id == id; }),
                             w.factions.end());
        } else {
            faction->leader = *std::min_element(ids.begin(), ids.end());
            const Country* successor = w.country(faction->leader);
            text += "; " + std::string(successor != nullptr ? successor->tag : "?") + " leads";
        }
    }
    g.log_event("faction", text, who);
    return true;
}

std::vector<CountryId> co_belligerents(const World& w, CountryId c) {
    std::vector<CountryId> out;
    if (!c.valid()) return out;
    w.wars.for_each([&](WarId, const War& war) {
        if (!war.active) return;
        const int side = side_of(war, c);
        if (side < 0) return;
        const std::vector<WarParticipant>& list = side == 0 ? war.attackers : war.defenders;
        for (const WarParticipant& p : list) {
            if (p.country != c) out.push_back(p.country);
        }
    });
    if (const Faction* f = faction_record(w, c)) {
        for (CountryId m : f->members) {
            if (m != c) out.push_back(m);
        }
    }
    std::vector<CountryId> live;
    for (CountryId other : out) {
        if (country_alive(w, other)) live.push_back(other);
    }
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());
    return live;
}

bool check_capitulation(Game& g, CountryId c) {
    World& w = g.world;
    Country* cc = w.country(c);
    if (cc == nullptr || !cc->alive) return false;
    if (!in_war(w, c)) return false;

    const ProvinceId capital = capital_province(w, c);
    if (!capital.valid()) return false;
    const Province* cp = w.province(capital);
    if (cp == nullptr) return false;
    const CountryId occupier = cp->controller;
    if (!occupier.valid() || occupier == c) return false;
    if (!countries_at_war(w, c, occupier)) return false;

    // Industry threshold: at least 70% of the starting factory base must be in
    // enemy hands (ARCHITECTURE 5.7 note: the baseline is Country::starting_factories,
    // falling back to the states the country owns at the time of the check).
    int baseline = cc->starting_factories;
    if (baseline <= 0) baseline = factories_in_states(w, c, /*by_owner=*/true);
    if (baseline <= 0) return false;
    const int held = factories_in_states(w, c, /*by_owner=*/false);
    if (static_cast<double>(held) > (1.0 - kIndustryLossThreshold) * static_cast<double>(baseline)) {
        return false;
    }

    cc->last_capitulation_check = w.tick;
    capitulate(g, c, occupier);
    return true;
}

void capitulate(Game& g, CountryId loser, CountryId winner) {
    World& w = g.world;
    Country* l = w.country(loser);
    Country* win = w.country(winner);
    if (l == nullptr || !l->alive) return;
    if (win == nullptr || !win->alive || loser == winner) return;

    // Territory: control transfers, and the loser's ownership transfers with it so
    // the winner's states become its own while core_owners keep the occupation model
    // aware of the original population.
    w.provinces.for_each([&](ProvinceId, Province& p) {
        if (p.controller == loser) p.controller = winner;
        if (p.owner == loser) {
            p.owner = winner;
            if (p.is_capital) p.is_capital = false;
        }
    });
    w.states.for_each([&](StateId, State& s) {
        if (s.controller == loser) s.controller = winner;
        if (s.owner == loser) s.owner = winner;
    });

    // Units are destroyed: the country leaves play. Battles holding references to
    // them are reconciled by the combat phase / cleanup (missing entities are
    // skipped there).
    std::vector<DivisionId> doomed_divisions;
    w.divisions.for_each([&](DivisionId did, const Division& d) {
        if (d.country == loser) doomed_divisions.push_back(did);
    });
    for (DivisionId did : doomed_divisions) {
        Division* d = w.division(did);
        if (d != nullptr && d->battle.valid()) detach_from_battle(g, *d);
        w.divisions.destroy(did);
    }
    std::vector<ArmyId> doomed_armies;
    w.armies.for_each([&](ArmyId aid, const Army& a) {
        if (a.country == loser) doomed_armies.push_back(aid);
    });
    for (ArmyId aid : doomed_armies) w.armies.destroy(aid);
    std::vector<CharacterId> doomed_characters;
    w.characters.for_each([&](CharacterId cid, const Character& ch) {
        if (ch.country == loser) doomed_characters.push_back(cid);
    });
    for (CharacterId cid : doomed_characters) w.characters.destroy(cid);

    // Wars: the loser stops fighting. Its roster entry is kept as the historical
    // record of the war (the auditor requires both sides to stay populated, and it
    // inspects ended wars too), but a war with no living participant left on either
    // side is over for everyone.
    std::vector<CountryId> to_refresh;
    auto side_has_living_participant = [&](const std::vector<WarParticipant>& side) {
        for (const WarParticipant& p : side) {
            if (p.country == loser) continue;  // leaving play in this call
            if (country_alive(w, p.country)) return true;
        }
        return false;
    };
    const std::vector<WarId> loser_wars = l->wars;
    for (WarId wid : loser_wars) {
        War* war = w.war(wid);
        if (war == nullptr) continue;
        for (const WarParticipant& p : war->attackers) {
            if (p.country != loser) to_refresh.push_back(p.country);
        }
        for (const WarParticipant& p : war->defenders) {
            if (p.country != loser) to_refresh.push_back(p.country);
        }
        if (!side_has_living_participant(war->attackers) ||
            !side_has_living_participant(war->defenders)) {
            war->active = false;
        }
    }
    l->wars.clear();
    l->at_war = false;
    std::sort(to_refresh.begin(), to_refresh.end());
    to_refresh.erase(std::unique(to_refresh.begin(), to_refresh.end()), to_refresh.end());
    for (CountryId c : to_refresh) refresh_at_war(w, c);

    // Industry stands down: a defeated country must not hold production assignments or
    // a construction queue, or the "assigned factories <= controlled factories"
    // invariant breaks for a country that controls nothing.
    l->lines.clear();
    l->construction.queue.clear();

    // Puppets are released: a state without an overlord may be picked up by whoever
    // wins the peace, but no puppet is inherited silently. A released puppet that was
    // already a belligerent in its own right keeps fighting; only the overlord's own
    // participation is removed above.
    for (CountryId p : l->puppets) {
        Country* pc = w.country(p);
        if (pc != nullptr) pc->overlord = CountryId{};
    }
    l->puppets.clear();
    if (l->overlord.valid()) {
        Country* ov = w.country(l->overlord);
        if (ov != nullptr) {
            ov->puppets.erase(std::remove(ov->puppets.begin(), ov->puppets.end(), loser),
                              ov->puppets.end());
        }
        l->overlord = CountryId{};
    }

    // Faction: a dead country leaves its faction; an empty faction loses its leader.
    for (Faction& f : w.factions) {
        f.members.erase(std::remove(f.members.begin(), f.members.end(), loser), f.members.end());
        if (f.leader == loser) {
            f.leader = f.members.empty() ? CountryId{} : *std::min_element(f.members.begin(), f.members.end());
        }
    }
    l->faction = 0;

    l->divisions.clear();
    l->armies.clear();
    l->generals.clear();
    l->training.clear();
    l->alive = false;

    std::string text = win->tag + " annexes " + l->tag + "; industry stands down";
    g.log_event("capitulation", text, loser);
}

bool offer_peace(Game& g, WarId war_id, CountryId proposer) {
    World& w = g.world;
    War* war = w.war(war_id);
    if (war == nullptr || !war->active) return false;
    const int proposer_side = side_of(*war, proposer);
    if (proposer_side < 0) return false;
    const int aggressor_side = side_of(*war, war->aggressor);

    // A war ends only when it is decided: the proposing side holds every claim it
    // made, or the aggressor has lost its own capital.
    bool decisive = side_holds_all_goals(w, *war, proposer_side);
    if (!decisive && aggressor_side >= 0 && proposer_side != aggressor_side &&
        aggressor_capital_lost(w, *war)) {
        decisive = true;
    }
    if (!decisive) return false;

    const std::vector<WarGoal> goals = war->goals;
    const std::vector<CountryId> attacker_ids = participants(*war, true);
    const std::vector<CountryId> defender_ids = participants(*war, false);

    // Settle the claims the winner actually holds: occupied ground changes hands,
    // contested claims are dropped.
    for (const WarGoal& goal : goals) {
        if (side_of(*war, goal.claimant) != proposer_side) continue;
        if (!goal.state.valid()) continue;
        if (!state_held_by_side(w, *war, goal.state, proposer_side)) continue;
        const State* st = w.state(goal.state);
        if (st == nullptr) continue;
        for (ProvinceId pid : st->provinces) {
            Province* p = w.province(pid);
            if (p == nullptr || p->is_sea) continue;
            if (!p->controller.valid() || p->controller == goal.claimant) continue;
            if (!countries_at_war(w, goal.claimant, p->controller)) continue;
            p->controller = goal.claimant;
        }
    }

    // Annexation and puppet clauses, applied after the territorial settlement.
    for (const WarGoal& goal : goals) {
        if (side_of(*war, goal.claimant) != proposer_side) continue;
        if (!goal.annex_country && !goal.puppet) continue;
        Country* target = w.country(goal.target);
        if (target == nullptr || !target->alive) continue;
        if (goal.annex_country && all_provinces_held_by_side(w, *war, goal.target, proposer_side)) {
            capitulate(g, goal.target, goal.claimant);
            break;  // the war may be closed by the annexation itself
        }
        if (goal.puppet) {
            if (target->overlord.valid()) {
                Country* old = w.country(target->overlord);
                if (old != nullptr) {
                    old->puppets.erase(std::remove(old->puppets.begin(), old->puppets.end(), goal.target),
                                       old->puppets.end());
                }
            }
            target->overlord = goal.claimant;
            Country* claimant = w.country(goal.claimant);
            if (claimant != nullptr &&
                std::find(claimant->puppets.begin(), claimant->puppets.end(), goal.target) ==
                    claimant->puppets.end()) {
                claimant->puppets.push_back(goal.target);
            }
        }
    }

    war = w.war(war_id);  // re-resolve: the annexation path can close the war
    if (war != nullptr && war->active) {
        war->active = false;
    }
    for (CountryId c : attacker_ids) refresh_at_war(w, c);
    for (CountryId c : defender_ids) refresh_at_war(w, c);

    // Post-war relations: peace is cold, not friendly, and any other war between the
    // same pair still keeps them formally at war.
    for (CountryId at : attacker_ids) {
        if (!country_alive(w, at)) continue;
        for (CountryId def : defender_ids) {
            if (!country_alive(w, def)) continue;
            Relation& r = w.relation(at, def);
            r.at_war = countries_at_war(w, at, def);
            r.value = clamp(r.value, kPostWarRelationBaseline, 25.0);
        }
    }

    const Country* pc = w.country(proposer);
    g.log_event("peace", std::string("peace offered by ") + (pc != nullptr ? pc->tag : "?"),
                proposer);
    return true;
}

void phase_diplomacy(Game& g) {
    World& w = g.world;
    const double hour_fraction = 1.0 / static_cast<double>(TICKS_PER_DAY);
    const double drift_step = kRelationDriftPerDay * hour_fraction;

    // 1. Relation drift: ordinary relations relax toward neutral, wartime relations
    //    harden toward the wartime ceiling. std::map iteration is ordered, so this is
    //    deterministic.
    for (auto& entry : w.relations) {
        Relation& r = entry.second;
        if (r.at_war) {
            const CountryId a(entry.first.first);
            const CountryId b(entry.first.second);
            if (!countries_at_war(w, a, b)) r.at_war = false;  // war ended elsewhere
        }
        const double target = r.at_war ? kWarRelationTarget : 0.0;
        if (r.value < target) {
            r.value = std::min(target, r.value + drift_step);
        } else if (r.value > target) {
            r.value = std::max(target, r.value - drift_step);
        }
        r.value = clamp(r.value, -100.0, 100.0);
    }

    std::vector<WarId> active_wars;
    w.wars.for_each([&](WarId wid, const War& war) {
        if (war.active) active_wars.push_back(wid);
    });

    // 2. War upkeep and occupation bookkeeping for every participant.
    for (WarId wid : active_wars) {
        War* war = w.war(wid);
        if (war == nullptr || !war->active) continue;
        for (std::vector<WarParticipant>* side : {&war->attackers, &war->defenders}) {
            for (WarParticipant& part : *side) {
                Country* c = w.country(part.country);
                if (c == nullptr) continue;
                part.occupation_share = occupation_share(w, *war, part.country);
                if (c->alive) {
                    c->political_power = c->political_power > 0.0
                                             ? std::max(0.0, c->political_power -
                                                                 kWarUpkeepPoliticalPowerPerDay *
                                                                     hour_fraction)
                                             : 0.0;
                }
            }
        }
    }

    // 3. No war may stay active without a living participant on both sides: a war
    //    whose side has been wiped out or removed from play is closed here, dropped
    //    from every participant's war list and logged. (The participant rosters are
    //    kept as the historical record of the war; nothing empties them, so an ended
    //    war still names both sides. An empty roster would be a bug in whoever created
    //    the war and is reported by the auditor.)
    for (WarId wid : active_wars) {
        War* war = w.war(wid);
        if (war == nullptr || !war->active) continue;
        auto has_living = [&](const std::vector<WarParticipant>& side) {
            for (const WarParticipant& p : side) {
                if (country_alive(w, p.country)) return true;
            }
            return false;
        };
        if (has_living(war->attackers) && has_living(war->defenders)) continue;
        war->active = false;
        const std::vector<CountryId> members = participants(*war, true);
        const std::vector<CountryId> others = participants(*war, false);
        for (CountryId c : members) refresh_at_war(w, c);
        for (CountryId c : others) refresh_at_war(w, c);
        g.log_event("war", "war ended: no belligerents left on one side", war->aggressor);
    }

    // 4. Capitulation, once per day. Occupation is owned by phase_territory (phase 5)
    //    and must not be applied twice per tick (see phase_occupation).
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) == 0) {
        std::vector<CountryId> fighting;
        w.countries.for_each([&](CountryId cid, const Country& c) {
            if (c.alive && in_war(w, cid)) fighting.push_back(cid);
        });
        for (CountryId cid : fighting) {
            // check_capitulation re-validates: an earlier capitulation in this loop
            // may already have ended this country's wars.
            check_capitulation(g, cid);
        }
    }

    // 5. AI peace offers: only from a side that has decisively won, and only for a
    //    country the AI actually plays.
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) == 0) {
        for (WarId wid : active_wars) {
            const War* war = w.war(wid);
            if (war == nullptr || !war->active) continue;
            CountryId lead;
            const int aggressor_side = side_of(*war, war->aggressor);
            if (side_holds_all_goals(w, *war, aggressor_side)) {
                lead = war->aggressor;
            } else if (aggressor_side >= 0 && aggressor_capital_lost(w, *war) &&
                       !war->defenders.empty()) {
                lead = war->defenders.front().country;
            }
            if (!lead.valid() || !g.is_ai(lead)) continue;
            offer_peace(g, wid, lead);
        }
    }
}

void phase_occupation(Game& g, CountryId country) {
    // Called once per country per tick by phase_territory (phase 5). phase_diplomacy
    // deliberately does not call it: a second caller would double the daily rates.
    World& w = g.world;
    const Country* holder = w.country(country);
    if (holder == nullptr || !holder->alive) return;  // nobody occupies for a dead state
    const double hour_fraction = 1.0 / static_cast<double>(TICKS_PER_DAY);
    w.states.for_each([&](StateId, State& s) {
        if (s.controller != country) return;
        if (!s.owner.valid()) return;  // unowned/neutral ground: nobody to occupy for
        const double population = state_population(w, s);
        if (state_is_core(s, country)) {
            // Own ground: the occupation model decays back to zero and asks for no
            // garrison.
            s.resistance = std::max(0.0, s.resistance - kCoreResistanceDecayPerDay * hour_fraction);
            s.compliance = std::max(0.0, s.compliance - kCoreComplianceDecayPerDay * hour_fraction);
            s.garrison_required = 0.0;
            return;
        }
        // Foreign ground: resistance grows while compliance is low, and slow
        // collaboration accumulates as resistance is kept down.
        s.resistance = clamp01(s.resistance +
                               kResistanceGrowthPerDay * (1.0 - s.compliance) * hour_fraction);
        s.compliance = clamp01(s.compliance +
                               kComplianceGrowthPerDay * (1.0 - s.resistance) * hour_fraction);
        s.garrison_required = s.resistance * population * kGarrisonPerResistancePopulation;
        if (!std::isfinite(s.garrison_required) || s.garrison_required < 0.0) {
            s.garrison_required = 0.0;
        }
    });
}

void update_province_control(Game& g, ProvinceId p) {
    World& w = g.world;
    Province* province = w.province(p);
    if (province == nullptr || province->is_sea) return;

    // The strongest occupier holds the province: total division strength, ties to the
    // lowest country id. std::map keeps the scan ordered, so the result is stable.
    std::map<uint32_t, double> strength;
    double best = 0.0;
    uint32_t best_country = INVALID_ID;
    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (d.location != p) return;
        if (!d.country.valid() || !country_alive(w, d.country)) return;
        const double weight = std::isfinite(d.strength) && d.strength > 0.0 ? d.strength : 0.0;
        const double total = (strength[d.country.v] += weight);
        const uint32_t v = d.country.v;
        if (best_country == INVALID_ID || total > best) {
            best = total;
            best_country = v;
        }
    });
    if (best_country == INVALID_ID) return;  // unoccupied: control stays as it is
    province->controller = CountryId(best_country);
    // Ownership never changes here: that is a peace-settlement decision.
}

}  // namespace hoi
