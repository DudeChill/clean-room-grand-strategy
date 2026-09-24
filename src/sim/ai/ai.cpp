// AI: layered planners for AI-controlled countries (spec sections 66-70, 163-166).
//
// The AI is a first-class player. Each layer reads only the planning country's own
// state plus world-level public state, never writes world state, and acts
// exclusively by pushing Commands into Game::queue. Those commands are validated
// and applied by the command system on the next tick, exactly like a human
// player's - so an AI country cannot do anything a player could not.
//
// Two invariants keep that promise checkable:
//
//  1. Every planned command is checked with validate_command before it is pushed,
//     so a command the AI "knows" is illegal is never even queued.
//  2. Legality is decided in one place. The layers score *desirability* only and
//     never re-implement the rules; the command system stays the single authority
//     on what a country may do.
//
// Because the queue is only applied at the start of the next tick, a layer sees
// pre-command state. Where a plan needs several commands that all change the same
// limited resource (construction slots, factory assignment, research slots,
// divisions in training) the layer keeps its own accounting for the commands it
// issued during this run, so the whole batch stays feasible when it is applied in
// order. Where that accounting and validate_command disagree the layer defers the
// command to the next run rather than risking a rejected command.
//
// Determinism: countries, states, provinces, divisions and wars are walked in
// ascending id order (Store::for_each / sorted vectors), no unordered container is
// iterated, ties are broken on ascending id, and every ratio goes through
// safe_div so no NaN/Inf can reach a score or a command payload.

#include "sim/ai/ai.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/air.h"
#include "sim/diplomacy.h"
#include "sim/design.h"
#include "sim/events.h"
#include "sim/focus.h"
#include "sim/spirits.h"
#include "sim/trade.h"
#include "sim/industry.h"
#include "sim/map.h"
#include <chrono>

#include "sim/navy.h"
#include "sim/research.h"
#include "sim/supply.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

const char* ai_layer_name(AiLayer l) {
    switch (l) {
        case AiLayer::Industry:
            return "industry";
        case AiLayer::Research:
            return "research";
        case AiLayer::Production:
            return "production";
        case AiLayer::Military:
            return "military";
        case AiLayer::Politics:
            return "politics";
        case AiLayer::Diplomacy:
            return "diplomacy";
        case AiLayer::Count:
            break;
    }
    return "unknown";
}

namespace {

// --------------------------------------------------------------- constants --
//
// Every weight below is a named part of the documented scoring model: the AI
// debugger shows the same names in AiReason::factors, so reading `last_reasons`
// is enough to explain a plan without stepping through the code.

// Scheduling.
constexpr uint32_t kDefaultInterval = 24;   // daily planning
constexpr uint32_t kMilitaryInterval = 12;  // the military layer reacts twice a day
constexpr size_t kMaxReasons = 16;

// Industry: construction.
constexpr double kIndustryBaseCivilianFactory = 45.0;
constexpr double kIndustryBaseMilitaryFactory = 40.0;
constexpr double kIndustryBaseInfrastructure = 25.0;
constexpr double kIndustryBaseSupplyHub = 15.0;
constexpr double kIndustryBaseFort = 12.0;
constexpr double kIndustryAtWar = 25.0;
constexpr double kIndustryPeace = 15.0;
constexpr double kIndustryEquipmentDeficit = 30.0;  // per unit of deficit ratio
constexpr double kIndustryMilitaryShare = 15.0;
constexpr double kIndustryCivilianCapacity = 10.0;
constexpr double kIndustryConsumerGoods = 10.0;
constexpr double kIndustryInfrastructureGap = 12.0;
constexpr double kIndustryInfrastructureDensity = 1.0;  // per existing factory
constexpr double kIndustrySupplyHubDistance = 25.0;
constexpr double kIndustryFortDefensive = 30.0;
constexpr double kIndustryFortOffensive = 10.0;
constexpr double kIndustryFortBorder = 15.0;
constexpr double kIndustryFortExisting = -8.0;  // per fort level already built
constexpr double kIndustryFortCapital = 25.0;
constexpr double kIndustryMinScore = 20.0;
constexpr int kIndustryMaxProjectsPerRun = 3;

// Research.
constexpr double kResearchUnlockNeeded = 25.0;
constexpr double kResearchUnlockTemplate = 15.0;
constexpr double kResearchWeaponPriority = 10.0;
constexpr double kResearchAvailableNow = 8.0;
constexpr double kResearchAheadOfTime = -20.0;  // per year ahead of the calendar
constexpr double kResearchCostWeight = -0.05;   // per research day
constexpr double kResearchCombatWarMultiplier = 1.5;
constexpr int kResearchMaxPerRun = 3;

// Production.
constexpr double kProdTemplateBaseline = 0.10;  // stock per template battalion
constexpr double kProdSwitchMinGain = 0.05;     // >=5% better stats to consider
constexpr double kProdSwitchRetentionLoss = 0.30;
constexpr double kProdSwitchMargin = 0.5;  // gain must beat half the efficiency loss
constexpr int kProdMinLines = 1;
constexpr int kProdMaxLines = 3;

// Military.
constexpr int kMilRecruitPerRunWar = 2;
constexpr int kMilRecruitPerRunPeace = 1;
constexpr int kMilDivisionsPerArmy = 12;
constexpr int kMilMaxArmies = 24;
constexpr int kMilMinUnassignedForArmy = 4;
constexpr int kMilMaxMovesPerRun = 8;
constexpr double kMilLocalAttackRatio = 1.0;  // attack when locally at least equal
constexpr int kMilMaxAttacksPerRun = 6;       // the rest of the budget reinforces the line
constexpr double kMilAttackUndefended = 60.0;  // free occupation
constexpr double kMilAttackBase = 20.0;
constexpr double kMilAttackRatioWeight = 40.0;
constexpr double kMilEquipmentReady = 0.5;  // stockpile share needed to train
constexpr double kMilOffensiveThreshold = 45.0;
constexpr double kMilOffensiveRatio = 1.15;
constexpr double kMilFallbackRatio = 0.75;
constexpr double kOrderForceWeight = 40.0;
constexpr double kOrderSupplyWeight = 20.0;
constexpr double kOrderFrontWeight = 10.0;
constexpr double kOrderCapitalThreatWeight = 40.0;

// Air (planned from the military layer, which owns the front line and the posture
// the air plan depends on). Aircraft are production planning too: kProdAirReserve is
// the stockpile a country with an air base keeps on hand so a wing can be formed.
constexpr int kAirMinWingSize = 50;       // smallest wing worth forming
constexpr int kAirWingEstablishment = 100;  // preferred wing size
constexpr int kAirMaxWings = 6;           // wings a country forms without more demand
constexpr int kAirReservePlanes = 60;     // aircraft kept in the stockpile
constexpr double kAirContestedThreshold = 1.0;  // enemy planes that make a region contested
constexpr double kAirModelAirWeight = 1.0;   // air-to-air value of a model's stats
constexpr double kAirModelGroundWeight = 0.5;  // ground-attack value of a model's stats

// Naval (planned from the military layer, which owns the posture and front line the
// naval plan depends on - exactly like the air layer above). Ships and convoys are
// production planning too, but they draw on dockyards rather than military
// factories, so the production layer keeps a separate dockyard budget for them.
constexpr int kNavalMinTaskForceShips = 4;     // smallest task force worth forming
constexpr int kNavalShipsPerTaskForce = 20;    // preferred task force size
constexpr int kNavalMaxTaskForces = 8;         // task forces a country forms
constexpr int kNavalEscortsPerForce = 8;       // escort hulls wanted per planned force
constexpr int kNavalCapitalsPerForce = 2;      // capital hulls wanted per planned force
constexpr double kNavalConvoyBaseline = 100.0;  // convoy stock kept for sea supply
constexpr double kNavalConvoyWarMultiplier = 2.0;  // wartime sea-supply demand
constexpr double kNavalInvasionControlThreshold = 0.5;  // control share needed to land
constexpr int kNavalRangeHops = 3;             // sea zones a task force may work from port
constexpr double kNavalDamagedStrength = 0.6;  // average strength that sends a force home
constexpr int kNavalStageMovesPerRun = 4;      // divisions ordered to a port per run

// A landing needs troops that can board now: an army whose divisions are locked in a
// battle or already retreating cannot gather at the port and would stall the crossing.
bool army_is_available_for_landing(const World& w, ArmyId army) {
    const Army* a = w.army(army);
    if (!a || a->divisions.empty()) return false;
    for (DivisionId did : a->divisions) {
        const Division* d = w.division(did);
        if (!d) continue;
        if (!d->location.valid()) return false;  // still training
        if (d->in_combat() || d->retreating || d->moving) return false;
    }
    return true;
}

bool army_is_invading(const World& w, ArmyId army) {
    if (!army.valid()) return false;
    for (const NavalInvasion& inv : w.invasions) {
        if (inv.army == army) return true;
    }
    return false;
}

// An army is committed to a landing when the crossing exists OR it has been given the
// invasion order while its divisions march to the staging port. While committed it must
// not be re-tasked: the front logic would otherwise pull staged divisions back out.
bool army_is_committed(const World& w, ArmyId army) {
    if (army_is_invading(w, army)) return true;
    const Army* a = w.army(army);
    return a != nullptr && a->order.kind == OrderKind::NavalInvasion;
}

// A division committed to a naval invasion must not be re-tasked by the army layer:
// the crossing waits until the whole army has gathered at the staging port, so any
// competing movement or army-level order would stall the operation forever.
bool division_committed_to_invasion(const World& w, const Division& d) {
    if (d.order == OrderKind::NavalInvasion) return true;
    return army_is_committed(w, d.army);
}


constexpr double kNavalContestedControl = 0.01;  // enemy share that makes a zone contested
constexpr double kNavalModelEscortSubWeight = 2.0;   // escort: anti-submarine weight
constexpr double kNavalModelEscortDetectWeight = 1.0;  // escort: spotting weight
constexpr double kNavalModelEscortGunWeight = 0.25;  // escort: light guns
constexpr double kNavalModelCapitalGunWeight = 1.0;  // capital: main battery
constexpr double kNavalModelCapitalTorpWeight = 0.5;  // capital: torpedoes
constexpr double kNavalModelCapitalArmorWeight = 0.5;  // capital: protection

// Diplomacy.
constexpr double kDipFactionThreat = 35.0;
constexpr double kDipFactionIdeology = 20.0;
constexpr double kDipFactionWeakness = 10.0;
constexpr double kDipFactionPatron = 10.0;
constexpr double kDipFactionThreshold = 40.0;
constexpr double kDipWarForce = 40.0;
constexpr double kDipWarWeakness = 20.0;
constexpr double kDipWarPosture = 20.0;
constexpr double kDipWarOwnThreat = 25.0;
constexpr double kDipWarCooldown = -100.0;
// Force ratio the country needs *against the specific target* (and its faction)
// before it will start a war; the declaration score is also reachable at 1.5.
constexpr double kDipWarRatioGate = 1.5;
constexpr double kDipWarThreshold = 45.0;
constexpr double kDipPeaceOccupation = 60.0;  // reason score only; the gate is decisiveness
constexpr uint64_t kWarDeclarationCooldown = 30ull * static_cast<uint64_t>(TICKS_PER_DAY);

// ------------------------------------------------------------------ helpers --

void ensure_posture_capacity(AiState& ai, size_t country_capacity) {
    if (ai.posture.size() < country_capacity) ai.posture.resize(country_capacity, 0);
}

uint8_t posture_of(const AiState& ai, CountryId c) {
    return c.v < ai.posture.size() ? ai.posture[c.v] : 0;
}

void set_posture(Game& g, CountryId c, uint8_t p) {
    ensure_posture_capacity(g.ai, c.v + 1);
    g.ai.posture[c.v] = p;
}

// Records one scored decision. The log is capped: once full, a new decision
// replaces the weakest one, so a debugger session always shows the decisions that
// actually drove the country rather than the first sixteen it happened to make.
void record_reason(Game& g, AiLayer l, AiReason r) {
    AiLayerState& st = g.ai.layer(l);
    r.score = finite_or(r.score, 0.0);
    for (auto& f : r.factors) f.second = finite_or(f.second, 0.0);
    if (st.last_reasons.size() < kMaxReasons) {
        st.last_reasons.push_back(std::move(r));
    } else {
        size_t weakest = 0;
        for (size_t i = 1; i < st.last_reasons.size(); ++i) {
            if (st.last_reasons[i].score < st.last_reasons[weakest].score) weakest = i;
        }
        if (r.score > st.last_reasons[weakest].score) st.last_reasons[weakest] = std::move(r);
    }
    ++g.ai.decisions_made;
}

void record_reason(Game& g, AiLayer l, const char* what, double score,
                   std::initializer_list<std::pair<const char*, double>> factors) {
    AiReason r;
    r.what = what;
    r.score = score;
    r.factors.reserve(factors.size());
    for (const auto& f : factors) r.factors.emplace_back(f.first, f.second);
    record_reason(g, l, std::move(r));
}

// Pushes `cmd` when the command system accepts it against the current state.
// Returns false when the command would be rejected, in which case the caller
// falls back to its next-best option instead of queueing a doomed order.
bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

Command make_command(CommandType type, CountryId country) {
    Command cmd;
    cmd.type = type;
    cmd.country = country;
    return cmd;
}

// How many commands of this kind the issuing country already has waiting in the
// queue. Layers subtract these from their budget so that planning twice before the
// queue is applied cannot double-spend a slot.
int pending_commands(const Game& g, CommandType type, CountryId country) {
    int n = 0;
    for (const Command& cmd : g.queue.pending) {
        if (cmd.type == type && cmd.country == country) ++n;
    }
    return n;
}

const BuildingDef* building_def(const Content& content, BuildingKind kind) {
    for (const BuildingDef& b : content.buildings) {
        if (b.kind == kind) return &b;
    }
    return nullptr;
}

int building_max_level(const Content& content, BuildingKind kind, int fallback) {
    const BuildingDef* def = building_def(content, kind);
    return def && def->max_level > 0 ? def->max_level : fallback;
}

// The province a country's capital sits in: the province flagged `is_capital`
// inside CapitalState, or the lowest-id land province of that state when the map
// does not flag one. Takes a country id so callers that only hold ids (war
// participants) can use it.
ProvinceId capital_province(const World& w, CountryId id) {
    const Country* c = w.country(id);
    if (!c) return ProvinceId{};
    const State* s = w.state(c->capital);
    if (!s) return ProvinceId{};
    ProvinceId best;
    for (ProvinceId p : s->provinces) {
        const Province* pr = w.province(p);
        if (!pr || pr->is_sea) continue;
        if (pr->is_capital) return p;
        if (!best.valid() || p < best) best = p;
    }
    return best;
}

bool hostile_pair(const World& w, CountryId a, CountryId b) {
    if (!a.valid() || !b.valid() || a == b) return false;
    return countries_at_war(w, a, b);
}

double mean_infrastructure(const World& w, const State& s) {
    double sum = 0.0;
    int n = 0;
    for (ProvinceId p : s.provinces) {
        const Province* pr = w.province(p);
        if (!pr || pr->is_sea || terrain_is_water(pr->terrain)) continue;
        sum += static_cast<double>(pr->infrastructure);
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

// Land distance from the nearest supply source of `country` to `target`, in
// province hops. Returns -1 when no source reaches the province.
int hops_from_supply(const World& w, CountryId country, ProvinceId target) {
    int best = -1;
    PathRequest req;
    req.country = country;
    req.require_controlled = true;
    req.for_supply = true;
    for (const SupplySource& src : supply_sources(w, country)) {
        if (!src.province.valid()) continue;
        std::vector<int32_t> hops;
        compute_hop_distances(w, src.province, req, &hops);
        if (target.v < hops.size() && hops[target.v] >= 0) {
            const int h = static_cast<int>(hops[target.v]);
            if (best < 0 || h < best) best = h;
        }
    }
    return best;
}

// Controllers of provinces adjacent to `p` (public map information).
std::vector<CountryId> hostile_neighbours(const World& w, CountryId self, ProvinceId p) {
    std::vector<CountryId> out;
    const Province* prov = w.province(p);
    if (!prov) return out;
    for (ProvinceId a : prov->adj) {
        const CountryId ctrl = w.province_controller(a);
        if (!ctrl.valid() || ctrl == self) continue;
        if (hostile_pair(w, self, ctrl)) {
            bool seen = false;
            for (CountryId s : out) {
                if (s == ctrl) seen = true;
            }
            if (!seen) out.push_back(ctrl);
        }
    }
    return out;
}

// Equipment models that at least one of the country's templates fields. Precomputed
// once per layer run so scoring stays linear in content size instead of scanning
// every template for every technology and every candidate line.
std::vector<char> template_equipment_mask(const Content& content, const Country& c,
                                          size_t equipment_count) {
    // A slot draws from a family, not from one model (MIL-021): every model of a family
    // the templates use counts, so a newer variant or a design is recognised as "this is
    // what my divisions are made of" instead of being invisible to production scoring.
    std::vector<std::string> families;
    for (TemplateId t : c.templates) {
        const DivisionTemplate* td = content.template_def(t);
        if (!td) continue;
        for (const BattalionSlot& b : td->battalions) {
            if (!b.equipment.valid()) continue;
            const std::string family = slot_family(content, b.equipment);
            if (family.empty()) continue;
            if (std::find(families.begin(), families.end(), family) == families.end()) {
                families.push_back(family);
            }
        }
    }
    std::vector<char> mask(equipment_count, 0);
    for (size_t i = 0; i < content.equipment.size() && i < equipment_count; ++i) {
        const EquipmentDef& def = content.equipment[i];
        if (def.is_archetype) continue;
        if (std::find(families.begin(), families.end(), def.archetype) != families.end()) {
            mask[i] = 1;
        }
    }
    return mask;
}

// Strength a country can put in the field: division strength is the public measure
// of an army, and it is what a player reads off the map before picking a fight.
double divisions_strength(const World& w, CountryId c) {
    const Country* cc = w.country(c);
    if (!cc) return 0.0;
    double s = 0.0;
    for (DivisionId d : cc->divisions) {
        const Division* dd = w.division(d);
        if (dd) s += clamp01(dd->strength);
    }
    return s;
}

// Strength a war against `id` would actually face: its own divisions plus everything
// its faction would pull into the war (declare_war brings faction members in).
double side_strength(const World& w, CountryId id, uint32_t own_faction) {
    const Country* target = w.country(id);
    if (!target) return 0.0;
    double s = divisions_strength(w, id);
    if (target->faction == 0 || target->faction == own_faction) return s;
    for (const Faction& f : w.factions) {
        if (f.id != target->faction) continue;
        for (CountryId m : f.members) {
            if (m != id) s += divisions_strength(w, m);
        }
    }
    return s;
}

// Rate limit on starting wars: one declaration per country per 30 days. Read from
// the command log (public, saved and deterministic) rather than from the live wars,
// because a war that has already ended must still hold the ceasefire period.
bool declared_recently(const Game& g, CountryId country, Tick now) {
    for (auto it = g.log.records.rbegin(); it != g.log.records.rend(); ++it) {
        if (it->command.type != CommandType::DeclareWar || it->command.country != country) continue;
        if (it->result != CommandResult::Applied) continue;
        return now < it->tick + kWarDeclarationCooldown;
    }
    return false;
}

// The state a war is declared for: the first (lowest id) state of the target that
// borders us. A war without a claim can only ever end in capitulation, so the AI
// always names one - the claim is also what its peace offers are settled against.
StateId border_claim_state(const World& w, CountryId self, CountryId target) {
    StateId best;
    w.states.for_each([&](StateId sid, const State& s) {
        if (best.valid() || s.impassable || s.owner != target) return;
        for (ProvinceId p : s.provinces) {
            const Province* pr = w.province(p);
            if (!pr || pr->is_sea || pr->controller != target) continue;
            for (ProvinceId a : pr->adj) {
                if (w.province_controller(a) == self) {
                    best = sid;
                    return;
                }
            }
        }
    });
    return best;
}

// Mirrors the diplomacy phase's decisive-settlement rule: a side may only end a war
// once it holds every state it claims, or - as the defender - once the aggressor has
// lost its own capital. Public state only; used as a gate so the AI never queues a
// peace offer the diplomacy phase would refuse.
bool war_decided_for(const World& w, const War& war, CountryId proposer) {
    auto side_of = [&war](CountryId country) -> int {
        for (const WarParticipant& p : war.attackers) {
            if (p.country == country) return 1;
        }
        for (const WarParticipant& p : war.defenders) {
            if (p.country == country) return 0;
        }
        return -1;
    };
    const int mine = side_of(proposer);
    if (mine < 0) return false;

    bool any_goal = false, all_held = true;
    for (const WarGoal& goal : war.goals) {
        if (side_of(goal.claimant) != mine || !goal.state.valid()) continue;
        any_goal = true;
        const State* st = w.state(goal.state);
        if (!st) {
            all_held = false;
            continue;
        }
        for (ProvinceId pid : st->provinces) {
            const Province* pr = w.province(pid);
            if (!pr || pr->is_sea) continue;
            if (side_of(pr->controller) != mine) {
                all_held = false;
                break;
            }
        }
    }
    if (any_goal && all_held) return true;

    const int aggressor_side = side_of(war.aggressor);
    if (aggressor_side >= 0 && mine != aggressor_side) {
        const ProvinceId cap = capital_province(w, war.aggressor);
        const Province* p = w.province(cap);
        if (p && p->controller.valid() && p->controller != war.aggressor &&
            side_of(p->controller) == mine) {
            return true;
        }
    }
    return false;
}

struct IndustryCandidate {
    BuildingKind kind = BuildingKind::CivilianFactory;
    StateId state;
    ProvinceId province;
    double score = 0.0;
    AiReason reason;
};

// ------------------------------------------------------------------ posture --

// True when the country is a belligerent of any live (or only just concluded) war.
// `Country::wars` is authoritative for the AI: it is public state and it survives a
// save, unlike anything the AI could cache for itself.
bool at_war_state(const Country& c) { return c.at_war || !c.wars.empty(); }

// Strategic assessment shared by the military and diplomacy layers: force ratio
// against everyone the country is at war with, supply health, how close the enemy is
// to the capital, and which neighbour (if any) the country could actually beat. All
// of it is information a player could read off the map and the ledger.
struct StrategicAssessment {
    double own_strength = 0.0;
    double enemy_strength = 0.0;
    double force_ratio = 0.0;
    double avg_supply = 1.0;
    double capital_threat = 0.0;   // 0..1
    double occupied_share = 0.0;   // own provinces controlled by a hostile
    double own_threat = 0.0;       // 0..1 combined danger to the country itself
    double offensive_score = 0.0;
    // Countries that border us (ascending id) and the best ratio we could bring
    // against one of them. Makes an offensive posture meaningful before any war
    // exists, and spares every later layer from re-scanning the map for borders.
    std::vector<CountryId> neighbours;
    double neighbour_ratio = 0.0;
    CountryId best_neighbour;
    std::vector<std::pair<std::string, double>> factors;
};

StrategicAssessment assess_strategy(const Game& g, const Country& c,
                                    const std::vector<ProvinceId>& front) {
    const World& w = g.world;
    StrategicAssessment a;

    for (DivisionId did : c.divisions) {
        const Division* d = w.division(did);
        if (!d) continue;
        a.own_strength += clamp01(d->strength);
    }

    double supply_sum = 0.0;
    int divisions = 0;
    for (DivisionId did : c.divisions) {
        const Division* d = w.division(did);
        if (!d) continue;
        supply_sum += clamp01(d->supply);
        ++divisions;
    }
    a.avg_supply = divisions > 0 ? supply_sum / static_cast<double>(divisions) : 1.0;

    w.divisions.for_each([&](DivisionId, const Division& d) {
        if (d.country == c.id) return;
        if (hostile_pair(w, c.id, d.country)) a.enemy_strength += clamp01(d.strength);
    });

    a.force_ratio = safe_div(a.own_strength, a.enemy_strength + 0.5);

    // Capital threat: enemy boots on the capital, or on its doorstep.
    const ProvinceId cap = capital_province(w, c.id);
    const Province* capital = w.province(cap);
    if (!capital) {
        a.capital_threat = 0.5;
    } else if (capital->controller != c.id) {
        a.capital_threat = 1.0;
    } else {
        const size_t n = hostile_neighbours(w, c.id, cap).size();
        a.capital_threat = clamp01(0.5 * static_cast<double>(n));
    }

    double owned = 0.0, occupied = 0.0;
    w.provinces.for_each([&](ProvinceId, const Province& p) {
        if (p.is_sea || p.owner != c.id) return;
        owned += 1.0;
        if (p.controller != c.id && hostile_pair(w, c.id, p.controller)) occupied += 1.0;
    });
    a.occupied_share = clamp01(safe_div(occupied, owned));

    // How much danger the country itself is in: pressure on the capital, lost
    // territory, and being outmatched in a war it is already fighting.
    const double war_pressure = at_war_state(c) ? clamp01(1.0 - a.force_ratio) : 0.0;
    a.own_threat = clamp01(a.capital_threat * 0.5 + a.occupied_share * 2.0 + war_pressure);

    // Reachable neighbours and the best force ratio available against one of them.
    // This is what gives a country an offensive posture before any war exists: a
    // large army alone is not a plan, a beatable neighbour is.
    std::vector<CountryId> neighbours;
    w.states.for_each([&](StateId, const State& s) {
        if (s.controller != c.id) return;
        for (ProvinceId p : s.provinces) {
            const Province* pr = w.province(p);
            if (!pr || pr->is_sea || terrain_is_water(pr->terrain)) continue;
            for (ProvinceId adj : pr->adj) {
                const CountryId ctrl = w.province_controller(adj);
                if (!ctrl.valid() || ctrl == c.id) continue;
                bool seen = false;
                for (CountryId known : neighbours) {
                    if (known == ctrl) seen = true;
                }
                if (!seen) neighbours.push_back(ctrl);
            }
        }
    });
    std::sort(neighbours.begin(), neighbours.end());
    a.neighbours = neighbours;
    for (CountryId n : a.neighbours) {
        const Country* nc = w.country(n);
        if (!nc || !nc->alive) continue;
        if (nc->faction != 0 && nc->faction == c.faction) continue;  // already allied
        const double ratio = safe_div(a.own_strength, side_strength(w, n, c.faction) + 0.5);
        if (ratio > a.neighbour_ratio) {
            a.neighbour_ratio = ratio;
            a.best_neighbour = n;
        }
    }

    a.offensive_score = kOrderForceWeight * clamp(a.force_ratio - 0.5, 0.0, 1.5) +
                        kOrderSupplyWeight * clamp01(a.avg_supply) +
                        kOrderFrontWeight * (front.empty() ? 0.0 : 1.0) -
                        kOrderCapitalThreatWeight * a.capital_threat;

    a.factors.emplace_back("force_ratio", a.force_ratio);
    a.factors.emplace_back("own_strength", a.own_strength);
    a.factors.emplace_back("enemy_strength", a.enemy_strength);
    a.factors.emplace_back("supply", a.avg_supply);
    a.factors.emplace_back("capital_threat", a.capital_threat);
    a.factors.emplace_back("occupied_share", a.occupied_share);
    a.factors.emplace_back("own_threat", a.own_threat);
    a.factors.emplace_back("neighbour_ratio", a.neighbour_ratio);
    a.factors.emplace_back("offensive_score", a.offensive_score);
    return a;
}

}  // namespace

// ============================================================== industry =====

void ai_industry_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive || !c.capital.valid()) return;
    const uint8_t posture = posture_of(g.ai, c.id);
    const bool war = at_war_state(c);

    int civ = 0, mil = 0, dock = 0;
    count_factories(w, c.id, &civ, &mil, &dock);

    // Equipment deficit: what divisions and the training queue are short of,
    // measured against the stockpile that should cover it.
    std::vector<double> demand;
    compute_equipment_demand(g, c.id, &demand);
    double demand_total = 0.0, stock_total = 0.0;
    for (double d : demand) {
        if (std::isfinite(d) && d > 0.0) demand_total += d;
    }
    for (double s : c.equipment_stockpile) {
        if (std::isfinite(s) && s > 0.0) stock_total += s;
    }
    const double deficit_ratio = clamp(safe_div(demand_total, stock_total + 1.0), 0.0, 2.0);

    const std::vector<ProvinceId> front = compute_front_line(w, c.id);

    std::vector<IndustryCandidate> candidates;

    // --- factories and infrastructure per controlled state -------------------
    w.states.for_each([&](StateId sid, const State& s) {
        if (s.controller != c.id || s.impassable) return;
        const int used = s.civilian_factories + s.military_factories + s.dockyards;
        const bool has_slot = used < s.building_slots;
        const double avg_infra = mean_infrastructure(w, s);

        if (has_slot) {
            const double share = safe_div(static_cast<double>(mil),
                                          static_cast<double>(mil + civ));
            const double mil_share_factor = kIndustryMilitaryShare * clamp01((0.5 - share) * 2.0);
            const double deficit_factor = kIndustryEquipmentDeficit * deficit_ratio;
            IndustryCandidate m;
            m.kind = BuildingKind::MilitaryFactory;
            m.state = sid;
            m.score = kIndustryBaseMilitaryFactory + (war ? kIndustryAtWar : 0.0) +
                      deficit_factor + mil_share_factor;
            m.reason.what = "build_military_factory";
            m.reason.score = m.score;
            m.reason.factors = {{"base", kIndustryBaseMilitaryFactory},
                                {"at_war", war ? kIndustryAtWar : 0.0},
                                {"equipment_deficit", deficit_factor},
                                {"military_share", mil_share_factor},
                                {"states_factories", static_cast<double>(used)}};
            candidates.push_back(std::move(m));

            const double peace_factor = war ? 0.0 : kIndustryPeace;
            const double capacity_factor =
                kIndustryCivilianCapacity * clamp01(1.0 - static_cast<double>(civ) / 50.0);
            const double consumer_factor = kIndustryConsumerGoods * clamp01(c.consumer_goods_ratio);
            IndustryCandidate c2;
            c2.kind = BuildingKind::CivilianFactory;
            c2.state = sid;
            c2.score = kIndustryBaseCivilianFactory + peace_factor + capacity_factor + consumer_factor;
            c2.reason.what = "build_civilian_factory";
            c2.reason.score = c2.score;
            c2.reason.factors = {{"base", kIndustryBaseCivilianFactory},
                                 {"peace", peace_factor},
                                 {"civilian_capacity", capacity_factor},
                                 {"consumer_goods", consumer_factor}};
            candidates.push_back(std::move(c2));
        }

        if (avg_infra < 8.0 && !s.provinces.empty()) {
            // Infrastructure is a per-province building, so the plan targets the
            // worst-served province of the state that also needs the capacity.
            ProvinceId worst;
            int worst_infra = 11;
            for (ProvinceId p : s.provinces) {
                const Province* pr = w.province(p);
                if (!pr || pr->is_sea || terrain_is_water(pr->terrain)) continue;
                if (pr->controller != c.id) continue;
                if (pr->infrastructure < worst_infra) {
                    worst_infra = pr->infrastructure;
                    worst = p;
                }
            }
            if (!worst.valid()) return;
            const double gap = kIndustryInfrastructureGap * clamp01((8.0 - avg_infra) / 8.0);
            const double density =
                kIndustryInfrastructureDensity * std::min<double>(static_cast<double>(used), 10.0);
            IndustryCandidate i;
            i.kind = BuildingKind::Infrastructure;
            i.state = sid;
            i.province = worst;
            i.score = kIndustryBaseInfrastructure + gap + density;
            i.reason.what = "build_infrastructure";
            i.reason.score = i.score;
            i.reason.factors = {{"base", kIndustryBaseInfrastructure},
                                {"infrastructure_gap", gap},
                                {"factory_density", density},
                                {"avg_infrastructure", avg_infra}};
            candidates.push_back(std::move(i));
        }
    });

    // --- supply hub on the front, when the front is out of logistics reach ---
    if (!front.empty()) {
        const int hops = hops_from_supply(w, c.id, front[0]);
        const double radius = g.content.constants.supply_hub_radius;
        if (hops >= 0 && static_cast<double>(hops) > radius * 0.5) {
            const double distance_factor =
                kIndustrySupplyHubDistance * clamp01((static_cast<double>(hops) - radius * 0.5) / radius);
            const Province* p = w.province(front[0]);
            if (p && !p->is_sea) {
                IndustryCandidate hub;
                hub.kind = BuildingKind::SupplyHub;
                hub.state = p->state;
                hub.province = front[0];
                hub.score = kIndustryBaseSupplyHub + distance_factor;
                hub.reason.what = "build_supply_hub";
                hub.reason.score = hub.score;
                hub.reason.factors = {{"base", kIndustryBaseSupplyHub},
                                      {"front_distance", static_cast<double>(hops)},
                                      {"front_supply_distance", distance_factor}};
                candidates.push_back(std::move(hub));
            }
        }
    }

    // --- forts on the border when the posture is defensive --------------------
    if (posture != 0) {
        const int fort_max = building_max_level(g.content, BuildingKind::Fort, 10);
        w.provinces.for_each([&](ProvinceId pid, const Province& p) {
            if (p.is_sea || p.controller != c.id || p.fort_level >= fort_max) return;
            const size_t hostiles = hostile_neighbours(w, c.id, pid).size();
            if (hostiles == 0) return;
            const double posture_factor = posture == 1 ? kIndustryFortDefensive : kIndustryFortOffensive;
            const double border_factor =
                kIndustryFortBorder * std::min<double>(static_cast<double>(hostiles), 2.0);
            const double existing_factor = kIndustryFortExisting * static_cast<double>(p.fort_level);
            const double capital_factor = p.is_capital ? kIndustryFortCapital : 0.0;
            IndustryCandidate f;
            f.kind = BuildingKind::Fort;
            f.state = p.state;
            f.province = pid;
            f.score = kIndustryBaseFort + posture_factor + border_factor + existing_factor + capital_factor;
            f.reason.what = "build_fort";
            f.reason.score = f.score;
            f.reason.factors = {{"base", kIndustryBaseFort},
                                {"defensive_posture", posture_factor},
                                {"border_pressure", border_factor},
                                {"existing_forts", existing_factor},
                                {"capital_risk", capital_factor}};
            candidates.push_back(std::move(f));
        });
    }

    // Best plan first; ties break on the building kind then on the target id so
    // the same state always produces the same queue.
    std::sort(candidates.begin(), candidates.end(),
              [](const IndustryCandidate& a, const IndustryCandidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  if (a.kind != b.kind) return a.kind < b.kind;
                  if (a.state != b.state) return a.state < b.state;
                  return a.province < b.province;
              });

    const int queue_free = std::max(0, c.construction.max_queue_size -
                                           static_cast<int>(c.construction.queue.size()) -
                                           pending_commands(g, CommandType::StartConstruction, c.id));
    int budget = std::min(queue_free, kIndustryMaxProjectsPerRun);
    int issued = 0;

    // Per-state slot accounting for the projects planned in this run: validate
    // only sees pre-command state, so the layer tracks what it already spent.
    std::vector<int> extra_slots(w.states.capacity() + 1, 0);

    for (const IndustryCandidate& cand : candidates) {
        if (budget <= 0) break;
        if (cand.score < kIndustryMinScore) break;

        const State* s = w.state(cand.state);
        if (!s) continue;
        if (cand.kind == BuildingKind::CivilianFactory || cand.kind == BuildingKind::MilitaryFactory ||
            cand.kind == BuildingKind::Dockyard) {
            const int used = s->civilian_factories + s->military_factories + s->dockyards +
                             extra_slots[cand.state.v];
            if (used >= s->building_slots) continue;
        }

        Command cmd = make_command(CommandType::StartConstruction, c.id);
        cmd.value = static_cast<int32_t>(cand.kind);
        // State-wide kinds identify their target by state; province kinds by
        // province. Sending both for a state-wide kind would be redundant payload.
        if (cand.kind == BuildingKind::CivilianFactory || cand.kind == BuildingKind::MilitaryFactory ||
            cand.kind == BuildingKind::Dockyard || cand.kind == BuildingKind::SyntheticRefinery) {
            cmd.state = cand.state;
        } else {
            cmd.province = cand.province;
            cmd.state = cand.state;
        }
        if (!push_if_valid(g, std::move(cmd))) continue;

        if (cand.kind == BuildingKind::CivilianFactory || cand.kind == BuildingKind::MilitaryFactory ||
            cand.kind == BuildingKind::Dockyard) {
            ++extra_slots[cand.state.v];
        }
        --budget;
        ++issued;
        record_reason(g, AiLayer::Industry, cand.reason);
    }

    if (issued == 0) {
        // Nothing was built this run: record why, so the debugger can tell "the
        // queue is full" from "nothing is worth building".
        const double best = candidates.empty() ? 0.0 : candidates.front().score;
        record_reason(g, AiLayer::Industry, "no_construction", 0.0,
                      {{"queue_free", static_cast<double>(queue_free)},
                       {"best_candidate", best},
                       {"min_score", kIndustryMinScore}});
    }
}

// ============================================================== research =====

namespace {

// Strategic value of one modifier point, by kind. Combat modifiers are worth more
// when the country is fighting; everything else is worth what it earns per day.
double modifier_weight(ModifierKind k) {
    switch (k) {
        case ModifierKind::FactoryOutput: return 400.0;
        case ModifierKind::DockyardOutput: return 120.0;
        case ModifierKind::ConstructionSpeed: return 350.0;
        case ModifierKind::ResearchSpeed: return 300.0;
        case ModifierKind::DivisionOrganization: return 200.0;
        case ModifierKind::DivisionAttack: return 200.0;
        case ModifierKind::DivisionDefense: return 150.0;
        case ModifierKind::DivisionBreakthrough: return 120.0;
        case ModifierKind::DivisionRecoveryRate: return 80.0;
        case ModifierKind::SupplyConsumption: return 100.0;
        case ModifierKind::MaxPlanning: return 120.0;
        case ModifierKind::PlanningSpeed: return 80.0;
        case ModifierKind::ManpowerGrowth: return 150.0;
        case ModifierKind::PoliticalPowerGain: return 100.0;
        case ModifierKind::FuelGain: return 80.0;
        case ModifierKind::TrainingTime: return 150.0;
        case ModifierKind::EquipmentCostFactor: return 300.0;
        case ModifierKind::RecruitablePopulation: return 150.0;
        case ModifierKind::Stability: return 100.0;
        case ModifierKind::WarSupport: return 100.0;
        case ModifierKind::EntrenchmentSpeed: return 60.0;
        case ModifierKind::CombatWidth: return 100.0;
        case ModifierKind::ConvoyDefense: return 40.0;
        case ModifierKind::Count: break;
    }
    return 50.0;
}

bool combat_modifier(ModifierKind k) {
    return k == ModifierKind::DivisionOrganization || k == ModifierKind::DivisionAttack ||
           k == ModifierKind::DivisionDefense || k == ModifierKind::DivisionBreakthrough ||
           k == ModifierKind::MaxPlanning || k == ModifierKind::PlanningSpeed ||
           k == ModifierKind::DivisionRecoveryRate || k == ModifierKind::EntrenchmentSpeed;
}

double research_category_weight(const std::string& category) {
    if (category == "industry") return 35.0;
    if (category == "infantry") return 30.0;
    if (category == "armor") return 30.0;
    if (category == "artillery") return 22.0;
    if (category == "motorized") return 20.0;
    if (category == "land_doctrine") return 15.0;
    if (category == "support") return 12.0;
    return 10.0;
}

struct TechCandidate {
    TechId tech;
    double score = 0.0;
    AiReason reason;
};

}  // namespace

void ai_research_layer(Game& g, Country& c) {
    if (!c.alive) return;
    const int year = g.world.date.year;
    const bool war = at_war_state(c);

    // Occupied slots: active, or holding a technology the engine has not finished
    // clearing. The engine may pre-size `slots` to `slots_unlocked`, so counting
    // only `active` would over-estimate the free capacity.
    int used_slots = 0;
    for (const ResearchSlot& s : c.research.slots) {
        if (s.active || s.tech.valid()) ++used_slots;
    }
    int free_slots = c.research.slots_unlocked - used_slots -
                     pending_commands(g, CommandType::StartResearch, c.id);
    free_slots = std::min(free_slots, kResearchMaxPerRun);
    if (free_slots <= 0) return;

    // Which equipment the country is short of, and in which categories.
    std::vector<double> demand;
    compute_equipment_demand(g, c.id, &demand);
    double demand_by_category[static_cast<int>(EquipmentCategory::Count)] = {};
    for (size_t i = 0; i < demand.size() && i < g.content.equipment.size(); ++i) {
        if (!std::isfinite(demand[i]) || demand[i] <= 0.0) continue;
        const EquipmentDef& def = g.content.equipment[i];
        demand_by_category[static_cast<int>(def.category)] += demand[i];
    }
    int top_category = -1;
    double top_demand = 0.0;
    for (int i = 0; i < static_cast<int>(EquipmentCategory::Count); ++i) {
        if (demand_by_category[i] > top_demand) {
            top_demand = demand_by_category[i];
            top_category = i;
        }
    }

    std::vector<TechCandidate> candidates;
    const std::vector<char> template_mask =
        template_equipment_mask(g.content, c, g.content.equipment.size());
    for (size_t i = 0; i < g.content.techs.size(); ++i) {
        const TechDef& tech = g.content.techs[i];
        const TechId id(i);
        if (c.research.has_tech(id)) continue;
        bool in_slot = false;
        for (const ResearchSlot& s : c.research.slots) {
            if (s.active && s.tech == id) in_slot = true;
        }
        if (in_slot) continue;
        if (!tech_available(g, c.id, id)) continue;

        const double category_factor = research_category_weight(tech.category);

        // Does it unlock equipment the army is short of, or that we field?
        double unlock_factor = 0.0;
        double priority_factor = 0.0;
        bool template_unlock = false;
        for (const std::string& key : tech.unlock_equipment) {
            const EquipmentId eq = g.content.equipment_id(key);
            const EquipmentDef* def = g.content.equipment_def(eq);
            if (!def) continue;
            if (eq.v < demand.size() && demand[eq.v] > 0.0) {
                unlock_factor = kResearchUnlockNeeded;
                if (static_cast<int>(def->category) == top_category) {
                    priority_factor = kResearchWeaponPriority;
                }
            }
            if (eq.v < template_mask.size() && template_mask[eq.v] != 0) template_unlock = true;
        }
        if (template_unlock) unlock_factor = std::max(unlock_factor, kResearchUnlockTemplate);

        double economy_value = 0.0, combat_value = 0.0;
        for (int k = 0; k < static_cast<int>(ModifierKind::Count); ++k) {
            const double v = tech.modifiers.v[k];
            if (!std::isfinite(v) || v == 0.0) continue;
            const double contribution = modifier_weight(static_cast<ModifierKind>(k)) * v;
            if (combat_modifier(static_cast<ModifierKind>(k))) {
                combat_value += contribution;
            } else {
                economy_value += contribution;
            }
        }
        const double combat_factor =
            combat_value * (war ? kResearchCombatWarMultiplier : 1.0);

        const double ahead_years = static_cast<double>(std::max(0, tech.year - year));
        const double ahead_factor = kResearchAheadOfTime * ahead_years;
        const double available_factor = tech.year <= year ? kResearchAvailableNow : 0.0;
        const double cost_factor = kResearchCostWeight * tech_cost_days(g, c.id, id);

        TechCandidate cand;
        cand.tech = id;
        cand.score = category_factor + unlock_factor + priority_factor + economy_value +
                     combat_factor + ahead_factor + available_factor + cost_factor;
        cand.reason.what = "research_" + tech.key;
        cand.reason.score = cand.score;
        cand.reason.factors = {{"category", category_factor},
                               {"unlock_needed_equipment", unlock_factor},
                               {"weapon_priority", priority_factor},
                               {"economy_value", economy_value},
                               {"combat_value", combat_factor},
                               {"ahead_of_time", ahead_factor},
                               {"available_now", available_factor},
                               {"research_cost", cost_factor}};
        candidates.push_back(std::move(cand));
    }

    std::sort(candidates.begin(), candidates.end(), [](const TechCandidate& a, const TechCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.tech < b.tech;
    });

    for (const TechCandidate& cand : candidates) {
        if (free_slots <= 0) break;
        Command cmd = make_command(CommandType::StartResearch, c.id);
        cmd.tech = cand.tech;
        if (!push_if_valid(g, std::move(cmd))) continue;
        --free_slots;
        record_reason(g, AiLayer::Research, cand.reason);
    }
}

// ============================================================ production =====

namespace {

struct ProdTarget {
    EquipmentId equipment;
    double need = 0.0;
    double score = 0.0;
};

double equipment_stat_score(const EquipmentDef& def) {
    return def.soft_attack + def.hard_attack * 0.5 + def.defense + def.breakthrough * 0.5 +
           def.armor * 0.5 + def.piercing * 0.25;
}

double production_category_weight(EquipmentCategory c) {
    switch (c) {
        case EquipmentCategory::Infantry: return 8.0;
        case EquipmentCategory::Armor: return 6.0;
        case EquipmentCategory::Artillery: return 5.0;
        case EquipmentCategory::Aircraft: return 4.0;
        case EquipmentCategory::Support: return 4.0;
        case EquipmentCategory::Motorized: return 3.0;
        case EquipmentCategory::Mechanized: return 3.0;
        case EquipmentCategory::AntiTank: return 3.0;
        case EquipmentCategory::AntiAir: return 3.0;
        default: return 0.0;
    }
}

// ------------------------------------------------------------------ navy ------
//
// Ship and convoy lines draw on dockyards; everything else on military factories
// (`equipment_factory_pool`). The production layer therefore plans two pools, and
// the naval layer below works from the same picture of what the country owns.

enum class NavalRole : uint8_t { Escort, Capital };

// Value of a hull for a role: escorts live on detection and anti-submarine work,
// capitals on guns and protection.
double naval_model_score(const EquipmentDef& d, NavalRole role) {
    if (role == NavalRole::Escort) {
        return d.sub_detection * kNavalModelEscortSubWeight +
               d.detection * kNavalModelEscortDetectWeight + d.naval_attack * kNavalModelEscortGunWeight;
    }
    return d.naval_attack * kNavalModelCapitalGunWeight +
           d.torpedo_attack * kNavalModelCapitalTorpWeight + d.armor * kNavalModelCapitalArmorWeight;
}

// Best ship model the country may actually build for a role (unlocked, not an
// archetype). Ties keep the lower index.
EquipmentId best_naval_model(const Game& g, CountryId country, NavalRole role) {
    EquipmentId best;
    double best_score = 0.0;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& d = g.content.equipment[i];
        if (d.is_archetype || d.category != EquipmentCategory::Ship) continue;
        if (!equipment_unlocked(g, country, EquipmentId(i))) continue;
        const double s = naval_model_score(d, role);
        if (!best.valid() || s > best_score) {
            best = EquipmentId(i);
            best_score = s;
        }
    }
    return best;
}

// Newest unlocked convoy model (the transport hull convoys are built from).
EquipmentId best_convoy_model(const Game& g, CountryId country) {
    EquipmentId best;
    int best_year = -1;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& d = g.content.equipment[i];
        if (d.is_archetype || d.category != EquipmentCategory::Convoy) continue;
        if (!equipment_unlocked(g, country, EquipmentId(i))) continue;
        if (!best.valid() || d.year > best_year) {
            best = EquipmentId(i);
            best_year = d.year;
        }
    }
    return best;
}

int ships_of_model(const World& w, CountryId country, EquipmentId equipment) {
    int n = 0;
    w.ships.for_each([&](ShipId, const Ship& s) {
        if (s.country == country && s.equipment == equipment) ++n;
    });
    return n;
}

// Total Convoy-category stockpile, in convoy units. Convoys cover overseas supply
// and are what a naval invasion embarks divisions on.
double convoy_stock(const Country& c, const Content& content) {
    double n = 0.0;
    for (size_t i = 0; i < c.equipment_stockpile.size(); ++i) {
        const EquipmentDef* def = content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
        if (def && def->category == EquipmentCategory::Convoy) n += c.equipment_stockpile[i];
    }
    return n;
}

int usable_port_count(const Game& g, CountryId country) {
    int n = 0;
    g.world.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (p.is_sea || !p.coastal) return;
        if (is_usable_port(g, country, pid)) ++n;
    });
    return n;
}

// Ships the country bases at a port, so a new task force can be sized against the
// base's free capacity.
int ships_in_port(const World& w, CountryId country, ProvinceId port) {
    int n = 0;
    w.ships.for_each([&](ShipId, const Ship& s) {
        if (s.country == country && s.port == port) ++n;
    });
    return n;
}

// Naval control of everyone hostile to `country` in a sea zone.
double hostile_naval_control(const Game& g, CountryId country, RegionId region) {
    double sum = 0.0;
    g.world.countries.for_each([&](CountryId id, const Country& other) {
        if (!other.alive || id == country) return;
        if (!hostile_pair(g.world, country, id)) return;
        const double share = naval_control_share(g, id, region);
        if (std::isfinite(share) && share > 0.0) sum += share;
    });
    return sum;
}

// Best in-range sea zone holding hostile shipping, or `home` when none is. A task
// force never works further than kNavalRangeHops sea zones from its port.
RegionId pick_enemy_zone(const Game& g, CountryId country, RegionId home) {
    if (!home.valid()) return RegionId{};
    RegionId best = home;
    double best_control = 0.0;
    g.world.regions.for_each([&](RegionId rid, const Region& r) {
        if (!r.is_sea) return;
        const int hops = rid == home ? 0 : sea_region_distance(g, home, rid);
        if (hops < 0 || hops > kNavalRangeHops) return;
        const double control = hostile_naval_control(g, country, rid);
        if (control <= best_control || control <= kNavalContestedControl) return;
        best = rid;
        best_control = control;
    });
    return best;
}

// An army that can actually embark: it owns divisions and one of them stands in a
// usable port of the country. `origin` is that port, so the landing starts where the
// troops are. Deterministic (ascending army and division ids).
struct InvasionForce {
    ArmyId army;
    ProvinceId origin;
    int divisions = 0;
};

InvasionForce pick_invasion_force(const Game& g, CountryId country) {
    InvasionForce force;
    std::vector<ArmyId> armies;
    const Country* c = g.world.country(country);
    if (!c) return force;
    armies = c->armies;
    std::sort(armies.begin(), armies.end());
    for (ArmyId aid : armies) {
        const Army* a = g.world.army(aid);
        if (!a || a->divisions.empty()) continue;
        std::vector<DivisionId> divs = a->divisions;
        std::sort(divs.begin(), divs.end());
        for (DivisionId did : divs) {
            const Division* d = g.world.division(did);
            if (!d || !d->location.valid()) continue;
            if (!is_usable_port(g, country, d->location)) continue;
            force.army = aid;
            force.origin = d->location;
            force.divisions = static_cast<int>(a->divisions.size());
            return force;
        }
    }
    return force;
}

// First (lowest-id) hostile coastal province reachable by sea from `origin`'s zone,
// within task-force range. Writes the crossing zone and its distance; invalid when
// there is no such coast.
ProvinceId hostile_coast_from(const Game& g, CountryId country, ProvinceId origin,
                              RegionId* out_crossing, int* out_hops) {
    const RegionId home = adjacent_sea_region(g, origin);
    if (!home.valid()) return ProvinceId{};
    ProvinceId target;
    g.world.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (target.valid() || p.is_sea || !p.coastal) return;
        if (!hostile_pair(g.world, country, p.controller)) return;
        const RegionId zone = adjacent_sea_region(g, pid);
        if (!zone.valid()) return;
        const int hops = zone == home ? 0 : sea_region_distance(g, home, zone);
        if (hops < 0 || hops > kNavalRangeHops) return;
        target = pid;
        if (out_crossing) *out_crossing = zone;
        if (out_hops) *out_hops = hops;
    });
    return target;
}

// The army and port an invasion would use. `at_port` is false when the army still
// has to march to `port`; the caller then stages it with MoveDivision orders. Two
// global passes: an army already standing in a usable port always wins over one that
// merely could reach a port, and both walks are in ascending id order.
struct InvasionSetup {
    ArmyId army;
    ProvinceId port;
    RegionId crossing;
    int hops = 0;        // sea hops from the port to the target coast
    int divisions = 0;
    bool at_port = false;
};

InvasionSetup plan_invasion(const Game& g, CountryId country) {
    InvasionSetup setup;
    const World& w = g.world;
    const Country* c = w.country(country);
    if (!c) return setup;
    std::vector<ArmyId> armies = c->armies;
    std::sort(armies.begin(), armies.end());

    auto army_invading = [&](ArmyId aid) {
        for (const NavalInvasion& inv : w.invasions) {
            if (inv.army == aid) return true;
        }
        return false;
    };
    auto living_divisions = [&](const Army& a) {
        std::vector<DivisionId> divs;
        for (DivisionId did : a.divisions) {
            const Division* d = w.division(did);
            if (d && d->location.valid()) divs.push_back(did);
        }
        std::sort(divs.begin(), divs.end());
        return divs;
    };

    // Pass A: a division already in a usable port with a hostile coast across.
    for (ArmyId aid : armies) {
        const Army* a = w.army(aid);
        if (!a || a->divisions.empty() || army_invading(aid)) continue;
        if (!army_is_available_for_landing(w, aid)) continue;
        const std::vector<DivisionId> divs = living_divisions(*a);
        for (DivisionId did : divs) {
            const Division* d = w.division(did);
            if (!d || !is_usable_port(g, country, d->location)) continue;
            RegionId crossing;
            int hops = 0;
            if (!hostile_coast_from(g, country, d->location, &crossing, &hops).valid()) continue;
            setup.army = aid;
            setup.port = d->location;
            setup.crossing = crossing;
            setup.hops = hops;
            setup.divisions = static_cast<int>(divs.size());
            setup.at_port = true;
            return setup;
        }
    }

    // Pass B: no army is at a port, so pick the first army that can march to one. The
    // port with a hostile coast and a controlled land route from every division wins;
    // ties keep the lowest port id, and the route length only breaks ties.
    for (ArmyId aid : armies) {
        const Army* a = w.army(aid);
        if (!a || a->divisions.empty()) continue;
        if (army_invading(aid)) continue;  // already crossing: not available for a new one
        if (!army_is_available_for_landing(w, aid)) continue;
        const std::vector<DivisionId> divs = living_divisions(*a);
        if (divs.empty()) continue;
        ProvinceId stage_port;
        RegionId stage_crossing;
        int stage_hops = 0;
        int best_route = -1;
        w.provinces.for_each([&](ProvinceId pid, const Province& p) {
            if (stage_port.valid() || p.is_sea || !p.coastal) return;
            if (!is_usable_port(g, country, pid)) return;
            RegionId crossing;
            int hops = 0;
            if (!hostile_coast_from(g, country, pid, &crossing, &hops).valid()) return;
            int route = 0;
            for (DivisionId did : divs) {
                const Division* d = w.division(did);
                if (!d || !d->location.valid()) return;
                if (d->location == pid) continue;
                PathRequest req;
                req.country = country;
                req.allow_hostile = false;
                req.require_controlled = true;
                const std::vector<ProvinceId> path = find_path(w, d->location, pid, req);
                if (path.empty()) return;
                route += static_cast<int>(path.size());
            }
            if (best_route >= 0 && route >= best_route) return;
            best_route = route;
            stage_port = pid;
            stage_crossing = crossing;
            stage_hops = hops;
        });
        if (stage_port.valid()) {
            setup.army = aid;
            setup.port = stage_port;
            setup.crossing = stage_crossing;
            setup.hops = stage_hops;
            setup.divisions = static_cast<int>(divs.size());
            setup.at_port = false;
            return setup;
        }
    }
    return setup;
}

// Combat value of an airframe. Fighters live on air attack and agility, CAS on
// ground attack; both are worth something, so one blended score picks the model the
// country should standardise on.
double aircraft_combat_score(const EquipmentDef& d) {
    return (d.air_attack + d.agility * 0.5) * kAirModelAirWeight +
           (d.ground_attack + d.air_attack * 0.5) * kAirModelGroundWeight;
}

// Best aircraft model the country may actually build (unlocked, not an archetype).
EquipmentId best_aircraft_model(const Game& g, CountryId country) {
    EquipmentId best;
    double best_score = 0.0;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        const EquipmentDef& d = g.content.equipment[i];
        if (d.is_archetype || d.category != EquipmentCategory::Aircraft) continue;
        if (!equipment_unlocked(g, country, EquipmentId(i))) continue;
        const double s = aircraft_combat_score(d);
        if (!best.valid() || s > best_score) {
            best = EquipmentId(i);
            best_score = s;
        }
    }
    return best;
}

double efficiency_of_line(const Country& c, EquipmentId eq) {
    for (const ProductionLine& line : c.lines) {
        if (line.equipment == eq) return clamp01(line.efficiency);
    }
    return 0.0;
}

int factories_of_line(const Country& c, EquipmentId eq) {
    for (const ProductionLine& line : c.lines) {
        if (line.equipment == eq) return line.factories;
    }
    return 0;
}

// Assigns one factory pool's production targets: trims the target list to the line
// budget, splits the factories with the largest-remainder method, then emits the
// removals, reductions and raises so every intermediate state stays inside the
// budget the command system enforces. `budget` is the controlled factory count of
// `pool` (military factories for land/air, dockyards for ships and convoys); a
// non-positive budget releases every line of that pool.
void emit_production_pool(Game& g, Country& c, std::vector<ProdTarget> targets, int budget,
                          FactoryPool pool) {
    if (budget <= 0) {
        // No factories of this pool left (occupied or destroyed industry): release
        // the pool's assignments rather than keep factories the country no longer
        // controls.
        for (const ProductionLine& line : c.lines) {
            if (line.factories <= 0) continue;
            if (line_factory_pool(g.content, line) != pool) continue;
            Command cmd = make_command(CommandType::RemoveProductionLine, c.id);
            cmd.equipment = line.equipment;
            if (!push_if_valid(g, std::move(cmd))) continue;
            record_reason(g, AiLayer::Production, "release_line", 5.0,
                          {{"factories_freed", static_cast<double>(line.factories)},
                           {"controlled_factories", static_cast<double>(budget)}});
        }
        return;
    }
    if (targets.empty()) return;

    // One line per distinct need, but never more lines than the pool has factories
    // to fill them.
    std::sort(targets.begin(), targets.end(), [](const ProdTarget& a, const ProdTarget& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.equipment < b.equipment;
    });
    const int line_count = std::max(
        kProdMinLines, std::min({kProdMaxLines, static_cast<int>(targets.size()), budget}));
    targets.resize(static_cast<size_t>(line_count));

    // Every chosen model gets one factory first (line_count <= budget guarantees
    // they all fit), then the remainder is split by need with the largest-remainder
    // method. The split is deterministic (ties fall to the lower index) and, unlike
    // a plain floor with a one-factory minimum, never asks for more factories than
    // the pool owns.
    double need_sum = 0.0;
    for (const ProdTarget& t : targets) need_sum += t.need;
    std::vector<int> want(targets.size(), 0);
    for (size_t i = 0; i < targets.size(); ++i) want[i] = 1;
    const int leftover = budget - static_cast<int>(targets.size());
    if (leftover > 0) {
        std::vector<double> frac(targets.size(), 0.0);
        int given = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            const double exact =
                static_cast<double>(leftover) * safe_div(targets[i].need, need_sum);
            const int base = static_cast<int>(std::floor(exact));
            want[i] += base;
            given += base;
            frac[i] = exact - static_cast<double>(base);
        }
        for (int rem = leftover - given; rem > 0; --rem) {
            size_t pick = 0;
            for (size_t i = 1; i < frac.size(); ++i) {
                if (frac[i] > frac[pick]) pick = i;
            }
            want[pick] += 1;
            frac[pick] = -1.0;  // never picked twice in one pass
        }
    }

    // Removals first, then reductions, then raises. Only this pool's lines are
    // touched, so the two pools never spend each other's factories.
    int assigned_live = 0;
    for (const ProductionLine& line : c.lines) {
        if (line.factories > 0 && line_factory_pool(g.content, line) == pool) {
            assigned_live += line.factories;
        }
    }

    struct Op {
        bool remove = false;
        EquipmentId equipment;
        int factories = 0;
        double score = 0.0;
        AiReason reason;
    };
    std::vector<Op> ops;

    for (const ProductionLine& line : c.lines) {
        if (line.factories <= 0) continue;  // retired line: nothing left to free
        if (line_factory_pool(g.content, line) != pool) continue;
        bool kept = false;
        for (const ProdTarget& t : targets) {
            if (t.equipment == line.equipment) kept = true;
        }
        if (kept) continue;
        Op op;
        op.remove = true;
        op.equipment = line.equipment;
        op.score = 10.0;
        const EquipmentDef* def = g.content.equipment_def(line.equipment);
        op.reason.what = "drop_line_" + (def ? def->key : std::string("unknown"));
        op.reason.score = op.score;
        op.reason.factors = {{"factories_freed", static_cast<double>(line.factories)}};
        ops.push_back(std::move(op));
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const int current = factories_of_line(c, targets[i].equipment);
        if (current == want[i]) continue;
        Op op;
        op.remove = false;
        op.equipment = targets[i].equipment;
        op.factories = want[i];
        op.score = targets[i].score;
        const EquipmentDef* def = g.content.equipment_def(targets[i].equipment);
        op.reason.what = std::string(current == 0 ? "new_line_" : "rebalance_line_") +
                         (def ? def->key : std::string("unknown"));
        op.reason.score = op.score;
        op.reason.factors = {{"need", targets[i].need},
                             {"factories_before", static_cast<double>(current)},
                             {"factories_after", static_cast<double>(want[i])}};
        ops.push_back(std::move(op));
    }

    std::sort(ops.begin(), ops.end(), [&](const Op& a, const Op& b) {
        const int a_delta = a.remove ? -1 : (a.factories - factories_of_line(c, a.equipment));
        const int b_delta = b.remove ? -1 : (b.factories - factories_of_line(c, b.equipment));
        if (a_delta != b_delta) return a_delta < b_delta;
        return a.equipment < b.equipment;
    });

    for (const Op& op : ops) {
        if (op.remove) {
            Command cmd = make_command(CommandType::RemoveProductionLine, c.id);
            cmd.equipment = op.equipment;
            if (!push_if_valid(g, std::move(cmd))) continue;
            assigned_live -= factories_of_line(c, op.equipment);
            record_reason(g, AiLayer::Production, op.reason);
            continue;
        }
        const int current = factories_of_line(c, op.equipment);
        const int next = assigned_live - current + op.factories;
        if (next > budget) continue;  // keep the batch inside the pool's budget
        Command cmd = make_command(CommandType::SetProductionLine, c.id);
        cmd.equipment = op.equipment;
        cmd.value = op.factories;
        if (!push_if_valid(g, std::move(cmd))) continue;
        assigned_live = next;
        record_reason(g, AiLayer::Production, op.reason);
    }
}

}  // namespace

void ai_production_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;
    int civ = 0, mil = 0, dock = 0;
    count_factories(w, c.id, &civ, &mil, &dock);
    if (mil <= 0 && dock <= 0) {
        // No factories of either pool left (occupied or destroyed industry): every
        // line has to be released, otherwise the country keeps factories assigned
        // that it no longer controls.
        for (const ProductionLine& line : c.lines) {
            if (line.factories <= 0) continue;
            Command cmd = make_command(CommandType::RemoveProductionLine, c.id);
            cmd.equipment = line.equipment;
            if (!push_if_valid(g, std::move(cmd))) continue;
            record_reason(g, AiLayer::Production, "release_line", 5.0,
                          {{"factories_freed", static_cast<double>(line.factories)},
                           {"controlled_factories", 0.0}});
        }
        return;
    }

    // ---- need per equipment model -------------------------------------------
    std::vector<double> need(g.content.equipment.size(), 0.0);
    {
        std::vector<double> demand;
        compute_equipment_demand(g, c.id, &demand);
        const size_t n = std::min(need.size(), demand.size());
        for (size_t i = 0; i < n; ++i) {
            if (std::isfinite(demand[i]) && demand[i] > 0.0) need[i] = demand[i];
        }
    }
    // Baseline stock-building for the divisions the country fields: without it a
    // country whose army is fully equipped would stop producing entirely.
    for (TemplateId t : c.templates) {
        const DivisionTemplate* td = g.content.template_def(t);
        if (!td) continue;
        for (const BattalionSlot& b : td->battalions) {
            if (!b.equipment.valid() || b.equipment.v >= need.size()) continue;
            need[b.equipment.v] += static_cast<double>(b.count) * kProdTemplateBaseline;
        }
    }

    // Air demand. Division templates never mention aircraft and
    // compute_equipment_demand only walks divisions, so without an explicit term an
    // aircraft model can never reach a production line: wings draw replacements from
    // the stockpile, but nothing would build them. Wings ask for replacements, and a
    // country with an air base keeps a reserve so a wing can actually be formed.
    {
        for (AirWingId wid : c.wings) {
            const AirWing* wing = w.wing(wid);
            if (!wing || !wing->equipment.valid() || wing->equipment.v >= need.size()) continue;
            const int missing = wing->max_planes - wing->planes;
            if (missing > 0) need[wing->equipment.v] += static_cast<double>(missing);
        }
        bool has_air_base = false;
        w.provinces.for_each([&](ProvinceId, const Province& p) {
            if (has_air_base) return;
            if (!p.is_sea && p.air_base > 0 && p.controller == c.id) has_air_base = true;
        });
        if (has_air_base) {
            const EquipmentId model = best_aircraft_model(g, c.id);
            if (model.valid() && model.v < need.size()) {
                need[model.v] += static_cast<double>(kAirReservePlanes);
            }
        }
    }

    // Naval demand. Division templates never mention ships or convoys and
    // compute_equipment_demand only walks divisions, so without an explicit term no
    // hull could ever reach a dockyard line. The desired task force composition sets
    // the escort and capital need; convoys cover overseas supply, and only a planned
    // invasion asks for the extra lift its divisions will need.
    {
        bool has_port = false;
        w.provinces.for_each([&](ProvinceId pid, const Province& p) {
            if (has_port || p.is_sea || !p.coastal) return;
            if (is_usable_port(g, c.id, pid)) has_port = true;
        });
        if (has_port) {
            int task_forces = 0;
            w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
                if (tf.country == c.id) ++task_forces;
            });
            int planned =
                clamp(std::max(usable_port_count(g, c.id), task_forces + 1), 1, kNavalMaxTaskForces);
            if (at_war_state(c)) planned = std::min(kNavalMaxTaskForces, planned + 1);

            auto hull_need = [&](NavalRole role, int per_force) {
                const EquipmentId model = best_naval_model(g, c.id, role);
                if (!model.valid() || model.v >= need.size()) return;
                const double have =
                    model.v < c.equipment_stockpile.size() ? c.equipment_stockpile[model.v] : 0.0;
                const double want = static_cast<double>(planned) * static_cast<double>(per_force);
                const double missing =
                    want - static_cast<double>(ships_of_model(w, c.id, model)) - have;
                if (missing > 0.0) need[model.v] += missing;
            };
            hull_need(NavalRole::Escort, kNavalEscortsPerForce);
            hull_need(NavalRole::Capital, kNavalCapitalsPerForce);

            const EquipmentId convoy = best_convoy_model(g, c.id);
            if (convoy.valid() && convoy.v < need.size()) {
                const double have =
                    convoy.v < c.equipment_stockpile.size() ? c.equipment_stockpile[convoy.v] : 0.0;
                double want =
                    kNavalConvoyBaseline * (at_war_state(c) ? kNavalConvoyWarMultiplier : 1.0);
                const InvasionForce force = pick_invasion_force(g, c.id);
                if (at_war_state(c) && force.army.valid() &&
                    hostile_coast_from(g, c.id, force.origin, nullptr, nullptr).valid()) {
                    want += g.content.constants.naval_invasion_convoys_per_division *
                            static_cast<double>(force.divisions);
                }
                const double missing = want - have;
                if (missing > 0.0) need[convoy.v] += missing;
            }
        }
    }

    // ---- pick the models worth a line, upgrading to the best known model ----
    const int year = g.world.date.year;
    // Tech availability is a per-country, per-run fact: resolve it once per model
    // instead of re-scanning the technology table inside the nested model loops below
    // (that scan is string-heavy and dominated the whole AI phase when it ran per
    // candidate pair).
    std::vector<char> unlocked(g.content.equipment.size(), 0);
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        unlocked[i] = equipment_unlocked(g, c.id, g.content.equipment[i].id) ? 1 : 0;
    }
    // A division template may name a bare archetype (the generic model) instead of a
    // concrete model, and a country may not have unlocked an authored model of that
    // family at all. Redirect that demand to the best model of the family the country
    // can actually build - an authored model or one of its own designs - otherwise the
    // demand is silently dropped (`is_archetype` defs never become production targets).
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        if (need[i] <= 0.0) continue;
        const EquipmentDef& def = g.content.equipment[i];
        if (!def.is_archetype) continue;
        const EquipmentDef* best = nullptr;
        size_t best_index = 0;
        for (size_t j = 0; j < g.content.equipment.size(); ++j) {
            const EquipmentDef& other = g.content.equipment[j];
            if (other.is_archetype || other.archetype != def.key) continue;
            if (other.year > year) continue;
            if (other.id.v >= unlocked.size() || unlocked[other.id.v] == 0) continue;
            if (!best || equipment_stat_score(other) > equipment_stat_score(*best)) {
                best = &other;
                best_index = j;
            }
        }
        if (!best) continue;
        need.at(best_index) += need[i];
        need[i] = 0.0;
        AiReason r;
        r.what = "build_family_model_" + def.key + "_as_" + best->key;
        r.score = need[best_index];
        r.factors = {{"family_need", need[best_index]},
                     {"family", static_cast<double>(def.key.size())},
                     {"model_is_design", best->archetype == def.key && !best->is_archetype ? 1.0 : 1.0}};
        record_reason(g, AiLayer::Production, std::move(r));
    }

    const std::vector<char> template_mask =
        template_equipment_mask(g.content, c, g.content.equipment.size());
    std::vector<ProdTarget> targets;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        if (need[i] <= 0.0) continue;
        const EquipmentDef& def = g.content.equipment[i];
        if (def.is_archetype) continue;
        // A model the country has not unlocked cannot be built: this is what a
        // research-gated model would look like from the production layer, and
        // targeting it only wastes commands (SetProductionLine would be rejected).
        if (def.id.v >= unlocked.size() || unlocked[def.id.v] == 0) continue;

        // Upgrade within the archetype when the new model is measurably better and
        // the efficiency thrown away by switching is smaller than the gain. The
        // upgrade target is tracked by *index*: `EquipmentDef::id` is metadata, and a
        // definition whose id does not match its slot must not be able to index the
        // need vector out of bounds.
        const EquipmentDef* best = &def;
        size_t best_index = i;
        for (size_t j = 0; j < g.content.equipment.size(); ++j) {
            const EquipmentDef& other = g.content.equipment[j];
            if (other.is_archetype || other.category != def.category ||
                other.archetype != def.archetype) {
                continue;
            }
            if (other.year > year) continue;
            if (other.id.v >= unlocked.size() || unlocked[other.id.v] == 0) continue;
            if (equipment_stat_score(other) > equipment_stat_score(*best)) {
                best = &other;
                best_index = j;
            }
        }
        if (best != &def) {
            const double gain =
                safe_div(equipment_stat_score(*best), equipment_stat_score(def) + 1e-9) - 1.0;
            const double loss = kProdSwitchRetentionLoss * efficiency_of_line(c, EquipmentId(i));
            const double margin = gain - kProdSwitchMargin * loss;
            if (gain >= kProdSwitchMinGain && margin > 0.0) {
                AiReason r;
                r.what = "switch_model_" + def.key + "_to_" + best->key;
                r.score = margin * 100.0;
                r.factors = {{"stat_gain", gain},
                             {"efficiency_loss", loss},
                             {"switch_margin", margin}};
                record_reason(g, AiLayer::Production, std::move(r));
                need.at(best_index) += need[i];
                need[i] = 0.0;
                continue;
            }
        }

        ProdTarget t;
        t.equipment = EquipmentId(i);
        t.need = need[i];
        t.score = need[i] + production_category_weight(def.category) +
                  (template_mask.size() > i && template_mask[i] != 0 ? 12.0 : 0.0);
        targets.push_back(t);
    }

    // Split the plan across the two factory pools: ships and convoys draw on
    // dockyards, land and air equipment on military factories. Each pool is planned
    // and budgeted independently so neither can spend the other's factories.
    std::vector<ProdTarget> military_targets;
    std::vector<ProdTarget> dockyard_targets;
    for (const ProdTarget& t : targets) {
        if (equipment_factory_pool(g.content, t.equipment) == FactoryPool::Dockyard) {
            dockyard_targets.push_back(t);
        } else {
            military_targets.push_back(t);
        }
    }

    // No demand at all (fresh country, empty templates): still keep one line on the
    // best unlocked infantry model so the country is not caught without a rifle.
    if (military_targets.empty()) {
        EquipmentId fallback;
        double fallback_score = -1.0;
        for (size_t i = 0; i < g.content.equipment.size(); ++i) {
            const EquipmentDef& def = g.content.equipment[i];
            if (def.is_archetype || def.category != EquipmentCategory::Infantry) continue;
            const double s = equipment_stat_score(def);
            if (s > fallback_score) {
                fallback_score = s;
                fallback = EquipmentId(i);
            }
        }
        if (fallback.valid()) military_targets.push_back(ProdTarget{fallback, 1.0, 1.0});
    }

    emit_production_pool(g, c, std::move(military_targets), mil, FactoryPool::Military);
    emit_production_pool(g, c, std::move(dockyard_targets), dock, FactoryPool::Dockyard);
}

// ============================================================== military =====

namespace {

// Divisions the country wants to field: a small standing army, scaled by the
// industry that has to equip it and by how many fronts it has to hold.
int division_target(const Country& c, int military_factories, size_t front_size) {
    int target = 4 + military_factories * 2 + static_cast<int>(front_size) * 2;
    if (at_war_state(c)) target = target * 3 / 2;
    return clamp(target, 4, 80);
}

double template_power(const DivisionTemplate& td) {
    return td.soft_attack + td.defense + td.breakthrough * 0.5 + td.hard_attack * 0.5 +
           td.armor * 0.5;
}

// Share of one division's equipment the stockpile already covers (min over
// battalions). A country trains only what it can actually equip.
double equipment_readiness(const Game& g, const Country& c, const DivisionTemplate& td) {
    // Gear of any model in the slot's family counts: a country holding 200 of a newly
    // designed rifle is equipped, not "short of the 1936 model".
    double worst = 1.0;
    for (const BattalionSlot& b : td.battalions) {
        if (b.count <= 0 || !b.equipment.valid()) continue;
        double have = 0.0;
        if (b.equipment.v < c.equipment_stockpile.size()) {
            have = c.equipment_stockpile[b.equipment.v];
        }
        const std::string family = slot_family(g.content, b.equipment);
        if (!family.empty()) {
            const double named = have;
            have = 0.0;
            for (size_t i = 0; i < g.content.equipment.size() && i < c.equipment_stockpile.size();
                 ++i) {
                const EquipmentDef& def = g.content.equipment[i];
                if (def.is_archetype || def.archetype != family) continue;
                if (!equipment_unlocked(g, c.id, def.id)) continue;
                have += c.equipment_stockpile[i];
            }
            if (have <= 0.0) have = named;  // locked family: fall back to the named model
        }
        const double ratio = clamp01(safe_div(have, static_cast<double>(b.count)));
        if (ratio < worst) worst = ratio;
    }
    return worst;
}

bool captain_of_side(const War& war, bool attacker_side, CountryId c) {
    const std::vector<WarParticipant>& side = attacker_side ? war.attackers : war.defenders;
    for (const WarParticipant& p : side) {
        if (p.country == c) return true;
    }
    return false;
}

bool side_has(const std::vector<WarParticipant>& side, CountryId c) {
    for (const WarParticipant& p : side) {
        if (p.country == c) return true;
    }
    return false;
}

// Best controlled air base to fly from. `needed` planes must fit in the base's free
// capacity, given `stationed` planes already committed to each province (live wings
// plus the commands this run has already queued). The base whose region is closest
// to `target` wins, ties falling to the lowest province id; a base with a land path
// to the target beats one without. Invalid id when the country controls no usable
// air base. Linear in provinces and wings: the caller precomputes `stationed` once.
ProvinceId pick_air_base(const Game& g, const Country& c, RegionId target, int needed,
                         const std::vector<int>& stationed) {
    const World& w = g.world;
    if (needed < 1) needed = 1;
    ProvinceId best;
    int best_hops = 0;
    w.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (p.is_sea || p.air_base <= 0 || p.controller != c.id) return;
        const int committed = pid.v < stationed.size() ? stationed[pid.v] : 0;
        if (air_base_capacity(g, pid) - committed < needed) return;
        const int hops = target.valid() ? region_distance_hops(g, p.region, target) : 0;
        if (!best.valid()) {
            best = pid;
            best_hops = hops;
            return;
        }
        const bool reachable = hops >= 0;
        const bool best_reachable = best_hops >= 0;
        if (reachable && !best_reachable) {
            best = pid;
            best_hops = hops;
        } else if (reachable == best_reachable && reachable && hops < best_hops) {
            best = pid;
            best_hops = hops;
        }
    });
    return best;
}

}  // namespace

// Air plan, run from the military layer (the same schedule and the same front line
// the army uses). Every decision goes through the command system, and each one
// leaves an AiReason in the military layer's log with its numeric factors.
void ai_air_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;

    const bool war = at_war_state(c);
    const uint8_t posture = posture_of(g.ai, c.id);
    const std::vector<ProvinceId> front = compute_front_line(w, c.id);
    const ProvinceId capital = capital_province(w, c.id);

    // The region the air force operates in: the front while at war, the home region
    // otherwise (and while a war has no front to speak of).
    RegionId target_region;
    const Province* anchor = (war && !front.empty()) ? w.province(front.front()) : nullptr;
    if (!anchor) anchor = w.province(capital);
    if (anchor) target_region = anchor->region;

    // The region an offensive army is pushing into: the first (lowest-id) hostile
    // province across the front. CAS flies over the enemy side of the line, not just
    // over our own.
    RegionId offensive_region;
    if (war && !front.empty()) {
        const Province* fp = w.province(front.front());
        if (fp) {
            for (ProvinceId n : fp->adj) {
                const Province* np = w.province(n);
                if (!np || np->is_sea || terrain_is_water(np->terrain)) continue;
                if (!hostile_pair(w, c.id, np->controller)) continue;
                offensive_region = np->region;
                break;
            }
        }
    }

    // Public wing store: enemy planes in the target region and our own tally.
    int enemy_planes_here = 0;
    int own_wings = 0;
    for (AirWingId wid : c.wings) {
        const AirWing* wing = w.wing(wid);
        if (!wing) continue;
        ++own_wings;
    }

    // Planes committed per province: live wings now, plus the moves and formations
    // this run decides below, so a batch of orders stays feasible when it is applied
    // in order (two wings must not be told they fit in the same free space).
    std::vector<int> stationed(w.provinces.capacity() + 1, 0);
    w.air_wings.for_each([&](AirWingId, const AirWing& x) {
        if (x.base.valid() && x.base.v < stationed.size()) stationed[x.base.v] += x.planes;
    });
    if (target_region.valid()) {
        w.air_wings.for_each([&](AirWingId, const AirWing& wing) {
            if (wing.country == c.id) return;
            if (!hostile_pair(w, c.id, wing.country)) return;
            if (wing.region != target_region) return;
            enemy_planes_here += wing.planes;
        });
    }

    // (c) The mission the force should fly. A contested sky is settled first; with
    // the sky ours an offensive army gets close air support and a defensive one
    // intercepts whatever crosses the line.
    AirMission desired = AirMission::AirSuperiority;
    const char* desired_name = "air_superiority";
    if (war) {
        if (enemy_planes_here > 0) {
            desired = AirMission::AirSuperiority;
            desired_name = "air_superiority_contested";
        } else if (posture == 2) {
            desired = AirMission::CloseAirSupport;
            desired_name = "close_air_support";
        } else {
            desired = AirMission::Interception;
            desired_name = "interception";
        }
    }

    // CAS flies where the army is attacking; every other mission flies the front
    // region (or home, at peace).
    RegionId mission_region = target_region;
    if (desired == AirMission::CloseAirSupport && offensive_region.valid()) {
        mission_region = offensive_region;
    }

    // (d) Move or remove a wing whose base is gone, then set its mission.
    for (AirWingId wid : c.wings) {
        const AirWing* wing = w.wing(wid);
        if (!wing) continue;
        const Province* base = w.province(wing->base);
        const bool base_lost = !base || base->controller != c.id;
        if (base_lost) {
            const ProvinceId rebase =
                pick_air_base(g, c, wing->region, wing->max_planes, stationed);
            Command move = make_command(CommandType::DeployAirWing, c.id);
            move.wing = wid;
            move.province = rebase;
            if (rebase.valid() && rebase != wing->base && push_if_valid(g, std::move(move))) {
                if (wing->base.v < stationed.size()) stationed[wing->base.v] -= wing->planes;
                if (rebase.v < stationed.size()) stationed[rebase.v] += wing->planes;
                record_reason(g, AiLayer::Military, "rebase_wing", 30.0,
                              {{"planes", static_cast<double>(wing->planes)},
                               {"wing_max", static_cast<double>(wing->max_planes)}});
                continue;
            }
            Command drop = make_command(CommandType::DisbandAirWing, c.id);
            drop.wing = wid;
            if (push_if_valid(g, std::move(drop))) {
                record_reason(g, AiLayer::Military, "disband_wing_base_lost", 40.0,
                              {{"planes", static_cast<double>(wing->planes)}, {"no_base", 1.0}});
            }
            continue;
        }

        const double stock = wing->equipment.valid() &&
                                     wing->equipment.v < c.equipment_stockpile.size()
                                 ? c.equipment_stockpile[wing->equipment.v]
                                 : 0.0;
        if (wing->planes <= 0 && stock < static_cast<double>(kAirMinWingSize)) {
            Command drop = make_command(CommandType::DisbandAirWing, c.id);
            drop.wing = wid;
            if (push_if_valid(g, std::move(drop))) {
                record_reason(g, AiLayer::Military, "disband_wing_depleted", 20.0,
                              {{"planes", 0.0}, {"stockpile", stock}});
            }
            continue;
        }

        if (wing->mission == desired && wing->region == mission_region) continue;
        Command cmd = make_command(CommandType::SetAirMission, c.id);
        cmd.wing = wid;
        cmd.region = mission_region;
        cmd.value = static_cast<int32_t>(desired);
        if (!push_if_valid(g, std::move(cmd))) continue;
        record_reason(g, AiLayer::Military, desired_name, 25.0,
                      {{"planes", static_cast<double>(wing->planes)},
                       {"enemy_planes", static_cast<double>(enemy_planes_here)},
                       {"posture", static_cast<double>(posture)},
                       {"war", war ? 1.0 : 0.0}});
    }

    // (b) Form a wing when the country has aircraft to fill one and an air base with
    // room. The mission is set on the next run, once the command has applied.
    if (own_wings + pending_commands(g, CommandType::CreateAirWing, c.id) >= kAirMaxWings) return;
    const EquipmentId model = best_aircraft_model(g, c.id);
    if (!model.valid()) return;
    const double stock =
        model.v < c.equipment_stockpile.size() ? c.equipment_stockpile[model.v] : 0.0;
    if (stock < static_cast<double>(kAirMinWingSize)) return;

    const ProvinceId base = pick_air_base(g, c, mission_region, kAirMinWingSize, stationed);
    if (!base.valid()) return;
    const int committed = base.v < stationed.size() ? stationed[base.v] : 0;
    const int free = air_base_capacity(g, base) - committed;
    const int size =
        clamp(std::min({kAirWingEstablishment, free, static_cast<int>(stock)}),
              kAirMinWingSize, free);
    if (size < kAirMinWingSize) return;

    Command cmd = make_command(CommandType::CreateAirWing, c.id);
    cmd.province = base;
    cmd.equipment = model;
    cmd.value = size;
    if (!push_if_valid(g, std::move(cmd))) return;
    if (base.v < stationed.size()) stationed[base.v] += size;
    record_reason(g, AiLayer::Military, "form_wing", 35.0,
                  {{"planes", static_cast<double>(size)},
                   {"stockpile", stock},
                   {"free_capacity", static_cast<double>(free)},
                   {"war", war ? 1.0 : 0.0}});
}

// Naval plan, run from the military layer exactly like the air layer above: it
// needs the posture the army sets and it acts through the same command queue. Every
// naval decision - fleet creation, task force formation, mission assignment and the
// invasion decision - leaves an AiReason with its numeric factors in the military
// layer's log.
//
// Ships are entities only once a task force exists, so the path that puts hulls in a
// port is: the production layer opens a dockyard line for the model, industry builds
// it into Country::equipment_stockpile, and this layer issues CreateTaskForce, whose
// `form_task_force` draws those stockpiled hulls into a task force based at a usable
// port. That is the only path by which an AI ship reaches the world.
void ai_naval_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;

    const bool war = at_war_state(c);
    const uint8_t posture = posture_of(g.ai, c.id);

    int own_ships = 0;
    w.ships.for_each([&](ShipId, const Ship& s) {
        if (s.country == c.id) ++own_ships;
    });

    // (a) administrative fleet. `form_task_force` can create one itself, but a
    // country that means to operate hulls still wants a roster for them.
    const bool fleet_ready =
        !c.fleets.empty() || pending_commands(g, CommandType::CreateFleet, c.id) > 0;
    if (!fleet_ready && (usable_port_count(g, c.id) > 0 || own_ships > 0)) {
        Command cmd = make_command(CommandType::CreateFleet, c.id);
        cmd.text = c.tag + " Fleet";
        if (push_if_valid(g, std::move(cmd))) {
            record_reason(g, AiLayer::Military, "create_fleet", own_ships > 0 ? 25.0 : 30.0,
                          {{"ships", static_cast<double>(own_ships)},
                           {"ports", static_cast<double>(usable_port_count(g, c.id))}});
        }
    }

    // (b) form a task force from the hull stockpile. The model with more hulls
    // waiting wins, so escort and capital production both reach the water.
    int task_forces = 0;
    w.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (tf.country == c.id) ++task_forces;
    });
    const int pending_tf = pending_commands(g, CommandType::CreateTaskForce, c.id);
    if (task_forces + pending_tf < kNavalMaxTaskForces) {
        const EquipmentId escort = best_naval_model(g, c.id, NavalRole::Escort);
        const EquipmentId capital = best_naval_model(g, c.id, NavalRole::Capital);
        auto stock_of = [&](EquipmentId m) {
            return m.valid() && m.v < c.equipment_stockpile.size() ? c.equipment_stockpile[m.v] : 0.0;
        };
        EquipmentId model = escort;
        if (capital.valid() && capital != escort && stock_of(capital) > stock_of(escort)) {
            model = capital;
        }
        const double stock = stock_of(model);
        if (model.valid() && stock >= static_cast<double>(kNavalMinTaskForceShips)) {
            // The usable port with the most free berths takes the new force, so it
            // always fits inside the base capacity the command system checks.
            ProvinceId build_port;
            int free_berths = 0;
            w.provinces.for_each([&](ProvinceId pid, const Province& p) {
                if (p.is_sea || !p.coastal) return;
                if (!is_usable_port(g, c.id, pid)) return;
                const int cap = naval_base_capacity(g, pid) - ships_in_port(w, c.id, pid);
                if (cap <= free_berths) return;  // strictly better wins; ties keep the lower id
                build_port = pid;
                free_berths = cap;
            });
            if (build_port.valid() && free_berths >= kNavalMinTaskForceShips) {
                const int size = clamp(
                    std::min({kNavalShipsPerTaskForce, free_berths, static_cast<int>(stock)}),
                    kNavalMinTaskForceShips, 40);
                if (size >= kNavalMinTaskForceShips) {
                    Command cmd = make_command(CommandType::CreateTaskForce, c.id);
                    cmd.province = build_port;
                    cmd.equipment = model;
                    cmd.value = size;
                    cmd.text = c.tag + " TF " + std::to_string(task_forces + pending_tf + 1);
                    if (push_if_valid(g, std::move(cmd))) {
                        record_reason(g, AiLayer::Military, "form_task_force", 35.0,
                                      {{"stockpile", stock},
                                       {"free_capacity", static_cast<double>(free_berths)},
                                       {"task_forces", static_cast<double>(task_forces)},
                                       {"war", war ? 1.0 : 0.0}});
                    }
                }
            }
        }
    }

    // (c) a hull the country owns but that no task force has taken in joins one, so
    // the fleet roster always accounts for the ships on the map.
    bool assigned_one = false;
    w.ships.for_each([&](ShipId sid, const Ship& s) {
        if (assigned_one || s.country != c.id || s.task_force.valid()) return;
        TaskForceId host;
        w.task_forces.for_each([&](TaskForceId tfd, const TaskForce& tf) {
            if (host.valid() || tf.country != c.id) return;
            if (static_cast<int>(tf.ships.size()) >= kNavalShipsPerTaskForce) return;
            host = tfd;
        });
        if (!host.valid()) return;
        Command cmd = make_command(CommandType::AssignShipToTaskForce, c.id);
        cmd.ship_id = sid;
        cmd.task_force = host;
        if (!push_if_valid(g, std::move(cmd))) return;
        assigned_one = true;
        record_reason(g, AiLayer::Military, "assign_ship", 12.0,
                      {{"ships", static_cast<double>(own_ships)}});
    });

    // (d) missions. A damaged force stands down and goes home; a landing of our own
    // is supported first; otherwise posture and the sea-supply dependency pick
    // between patrol, escort duty, the strike force and the raiders.
    RegionId invasion_zone;
    bool landing = false;
    for (const NavalInvasion& inv : w.invasions) {
        if (inv.country != c.id || inv.landed) continue;
        invasion_zone = inv.sea_region;
        landing = true;
        break;
    }
    bool has_port = false;
    w.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (has_port || p.is_sea || !p.coastal) return;
        if (is_usable_port(g, c.id, pid)) has_port = true;
    });
    const bool sea_supply = has_port && (war || [&] {
        const ProvinceId cap = capital_province(w, c.id);
        const Province* cp = w.province(cap);
        const RegionId home_region = cp ? cp->region : RegionId{};
        bool overseas = false;
        w.provinces.for_each([&](ProvinceId, const Province& p) {
            if (overseas || p.is_sea || p.controller != c.id) return;
            if (p.region != home_region) overseas = true;
        });
        return overseas;
    }());

    w.task_forces.for_each([&](TaskForceId tfd, const TaskForce& tf) {
        if (tf.country != c.id) return;
        const RegionId home = adjacent_sea_region(g, tf.port);
        if (!home.valid()) return;

        int hulls = 0;
        double strength_sum = 0.0;
        for (ShipId sid : tf.ships) {
            const Ship* s = w.ship(sid);
            if (!s) continue;
            strength_sum += clamp01(s->strength);
            ++hulls;
        }
        const double strength = hulls > 0 ? clamp01(strength_sum / static_cast<double>(hulls)) : 1.0;

        const TaskForceStats stats = task_force_stats(g, tfd);
        const bool raider =
            stats.torpedo_attack > stats.naval_attack && stats.torpedo_attack > 0.0;

        NavalMission desired = NavalMission::None;
        const char* reason_name = "return_to_port";
        RegionId target = home;
        if (strength < kNavalDamagedStrength) {
            desired = NavalMission::None;  // the naval phase sends a stood-down force home
            reason_name = "return_to_port";
        } else if (landing && invasion_zone.valid()) {
            desired = NavalMission::InvasionSupport;
            reason_name = "invasion_support";
            target = invasion_zone;
        } else if (!war) {
            desired = sea_supply ? NavalMission::ConvoyEscort : NavalMission::Patrol;
            reason_name = sea_supply ? "convoy_escort" : "patrol";
        } else {
            const RegionId enemy_zone = pick_enemy_zone(g, c.id, home);
            const bool contested = enemy_zone != home ||
                                   hostile_naval_control(g, c.id, home) > kNavalContestedControl;
            if (posture == 2 && contested) {
                desired = raider ? NavalMission::ConvoyRaid : NavalMission::StrikeForce;
                reason_name = raider ? "convoy_raid" : "strike_force";
                target = enemy_zone;
            } else if (posture == 2) {
                desired = NavalMission::StrikeForce;
                reason_name = "strike_force";
            } else {
                desired = sea_supply ? NavalMission::ConvoyEscort : NavalMission::Patrol;
                reason_name = sea_supply ? "convoy_escort" : "patrol";
            }
        }
        if (!target.valid()) return;
        if (tf.mission == desired && tf.sea_region == target) return;

        Command cmd = make_command(CommandType::SetNavalMission, c.id);
        cmd.task_force = tfd;
        cmd.region = target;
        cmd.value = static_cast<int32_t>(desired);
        if (!push_if_valid(g, std::move(cmd))) return;
        record_reason(g, AiLayer::Military, reason_name, 20.0 + strength * 20.0,
                      {{"ships", static_cast<double>(hulls)},
                       {"avg_strength", strength},
                       {"enemy_control", hostile_naval_control(g, c.id, home)},
                       {"posture", static_cast<double>(posture)},
                       {"war", war ? 1.0 : 0.0}});
    });

    // (e) invasion. Only start one when the country has transports, naval control in
    // the crossing zone, and a hostile coast its troops can embark for. When the army
    // is not yet in a port, march it there first and launch only once it has arrived;
    // the decision is recorded with every factor either way.
    if (has_port && war && !landing) {
        const InvasionSetup setup = plan_invasion(g, c.id);
        if (setup.army.valid() && setup.port.valid() && setup.crossing.valid()) {
            const double control = naval_control_share(g, c.id, setup.crossing);
            const double enemy = hostile_naval_control(g, c.id, setup.crossing);
            const double convoys = convoy_stock(c, g.content);
            const double needed = g.content.constants.naval_invasion_convoys_per_division *
                                  static_cast<double>(setup.divisions);
            const bool ready = control >= kNavalInvasionControlThreshold && control > enemy &&
                               convoys >= needed;
            if (ready) {
                // Commit the army before marching: the invasion order is what keeps
                // the front logic from pulling staged divisions back to the line.
                const Army* committed = w.army(setup.army);
                if (committed && committed->order.kind != OrderKind::NavalInvasion) {
                    Command order = make_command(CommandType::SetDivisionOrder, c.id);
                    order.army = setup.army;
                    order.value = static_cast<int32_t>(OrderKind::NavalInvasion);
                    if (push_if_valid(g, std::move(order))) {
                        record_reason(g, AiLayer::Military, "commit_invasion", 35.0,
                                      {{"naval_control", control},
                                       {"divisions", static_cast<double>(setup.divisions)}});
                    }
                }
                // Stage every division that is not yet in the port, every run — a
                // single division's arrival must not stop the rest of the army from
                // gathering, or the crossing would wait for them forever.
                const Army* a = w.army(setup.army);
                std::vector<DivisionId> divisions;
                if (a) {
                    divisions = a->divisions;
                    std::sort(divisions.begin(), divisions.end());
                }
                bool all_at_port = !divisions.empty();
                int staged = 0;
                for (DivisionId did : divisions) {
                    const Division* d = w.division(did);
                    if (!d || !d->location.valid()) {
                        all_at_port = false;
                        continue;
                    }
                    if (d->location == setup.port) continue;
                    all_at_port = false;
                    if (staged >= kNavalStageMovesPerRun) continue;
                    // Troops locked in a fight or already falling back cannot be pulled
                    // out this run; they will be staged once they are free.
                    if (d->retreating || d->in_combat()) continue;
                    // Already on the way to the port: leave the march alone. Anything
                    // else (including a march somewhere else) is redirected here, since
                    // a committed army must gather at the port.
                    if (!d->path.empty() && d->path.back() == setup.port) continue;
                    Command cmd = make_command(CommandType::MoveDivision, c.id);
                    cmd.division = did;
                    cmd.province = setup.port;
                    if (!push_if_valid(g, std::move(cmd))) continue;
                    ++staged;
                    record_reason(g, AiLayer::Military, "stage_invasion", 30.0,
                                  {{"sea_hops", static_cast<double>(setup.hops)},
                                   {"divisions", static_cast<double>(setup.divisions)},
                                   {"convoys", convoys},
                                   {"naval_control", control}});
                }

                // Launch only when the whole army is actually in the port: the
                // crossing waits for every division, so launching early would leave
                // the operation stalled at the gather step.
                if (all_at_port) {
                    RegionId crossing;
                    int hops = 0;
                    const ProvinceId target =
                        hostile_coast_from(g, c.id, setup.port, &crossing, &hops);
                    if (target.valid()) {
                        Command cmd = make_command(CommandType::LaunchNavalInvasion, c.id);
                        cmd.army = setup.army;
                        cmd.province = setup.port;
                        cmd.province_b = target;
                        if (push_if_valid(g, std::move(cmd))) {
                            record_reason(g, AiLayer::Military, "launch_invasion",
                                          50.0 + control * 50.0,
                                          {{"naval_control", control},
                                           {"enemy_control", enemy},
                                           {"convoys", convoys},
                                           {"convoys_needed", needed},
                                           {"divisions", static_cast<double>(setup.divisions)},
                                           {"sea_hops", static_cast<double>(hops)}});
                        }
                    }
                }
            }
        }
    }
}

void ai_military_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;

    const std::vector<ProvinceId> front = compute_front_line(w, c.id);
    const bool war = at_war_state(c);

    int civ = 0, mil = 0, dock = 0;
    count_factories(w, c.id, &civ, &mil, &dock);

    const StrategicAssessment assess = assess_strategy(g, c, front);
    const bool losing = assess.force_ratio < kMilFallbackRatio || assess.capital_threat >= 1.0 ||
                        assess.occupied_share > 0.3;
    uint8_t posture = 0;
    if (war) {
        posture = (assess.offensive_score >= kMilOffensiveThreshold &&
                   assess.force_ratio >= kMilOffensiveRatio)
                      ? 2
                      : 1;
    } else {
        // At peace the posture says whether the country is ready to pick a fight: it
        // needs a neighbour it could actually beat, not merely a large army. The army
        // itself stays on garrison orders until war is declared.
        posture = assess.neighbour_ratio >= kDipWarRatioGate ? 2 : 0;
    }
    set_posture(g, c.id, posture);

    // Front-facing province used as the deployment and movement goal.
    ProvinceId goal;
    if (!front.empty()) {
        goal = front[0];
    } else {
        const ProvinceId cap = capital_province(w, c.id);
        const Province* p = w.province(cap);
        if (p && p->controller == c.id) goal = cap;
    }

    // ---- (a) training and deployment ---------------------------------------
    const int target = division_target(c, mil, front.size());
    const int training = pending_commands(g, CommandType::RecruitDivision, c.id);
    int recruits_this_run = 0;
    const int recruit_limit = war ? kMilRecruitPerRunWar : kMilRecruitPerRunPeace;

    if (static_cast<int>(c.divisions.size()) + training < target) {
        // Recruiting deducts manpower when the command applies, so the layer spends
        // from a local copy: two recruits in one run must not both be told they have
        // the same manpower behind them.
        double manpower_left = c.manpower;
        for (TemplateId t : c.templates) {
            if (recruits_this_run >= recruit_limit) break;
            const DivisionTemplate* td = g.content.template_def(t);
            if (!td) continue;
            if (td->manpower <= 0.0) continue;
            const double readiness = equipment_readiness(g, c, *td);
            const double power = template_power(*td);
            if (readiness < kMilEquipmentReady) continue;
            if (manpower_left < td->manpower) continue;

            Command cmd = make_command(CommandType::RecruitDivision, c.id);
            cmd.template_id = t;
            cmd.value = 1;
            if (!push_if_valid(g, std::move(cmd))) continue;

            manpower_left -= td->manpower;
            AiReason r;
            r.what = "recruit_" + td->key;
            r.score = power + readiness * 20.0;
            r.factors = {{"template_power", power},
                         {"equipment_ready", readiness},
                         {"manpower", c.manpower},
                         {"divisions", static_cast<double>(c.divisions.size())},
                         {"division_target", static_cast<double>(target)}};
            record_reason(g, AiLayer::Military, std::move(r));
            ++recruits_this_run;
        }
    }

    if (goal.valid()) {
        int deploys = 0;
        w.divisions.for_each([&](DivisionId did, const Division& d) {
            if (deploys >= 2 || d.country != c.id) return;
            if (d.location.valid()) return;           // already on the map
            if (d.training_days_left > 0.0) return;   // still training off-map
            Command cmd = make_command(CommandType::DeployDivision, c.id);
            cmd.division = did;
            cmd.province = goal;
            if (!push_if_valid(g, std::move(cmd))) return;
            record_reason(g, AiLayer::Military, "deploy_division", 15.0,
                          {{"to_front", front.empty() ? 0.0 : 1.0},
                           {"capital_fallback", front.empty() ? 1.0 : 0.0}});
            ++deploys;
        });
    }

    // ---- (b) army organisation --------------------------------------------
    std::vector<DivisionId> unassigned;
    w.divisions.for_each([&](DivisionId did, const Division& d) {
        if (d.country != c.id) return;
        if (d.army.valid() || !d.location.valid()) return;
        unassigned.push_back(did);
    });

    if (!unassigned.empty()) {
        // Fill an existing army that still has room and a general first: that keeps
        // the number of armies low instead of creating one per division.
        ArmyId host;
        size_t host_size = 0;
        for (ArmyId aid : c.armies) {
            const Army* a = w.army(aid);
            if (!a) continue;
            if (army_is_committed(w, aid)) continue;  // the crossing owns this army
            if (static_cast<int>(a->divisions.size()) >= kMilDivisionsPerArmy) continue;
            if (!host.valid() || a->divisions.size() < host_size) {
                host = aid;
                host_size = a->divisions.size();
            }
        }
        if (host.valid()) {
            Command cmd = make_command(CommandType::AssignDivisionToArmy, c.id);
            cmd.army = host;
            cmd.division = unassigned.front();
            const size_t take = std::min<size_t>(
                unassigned.size(),
                static_cast<size_t>(std::max(1, kMilDivisionsPerArmy - static_cast<int>(host_size))));
            cmd.divisions.assign(unassigned.begin(), unassigned.begin() + take);
            if (push_if_valid(g, std::move(cmd))) {
                record_reason(g, AiLayer::Military, "assign_divisions", 12.0,
                              {{"divisions", static_cast<double>(take)},
                               {"army_size", static_cast<double>(host_size)}});
            }
        } else if (unassigned.size() >= kMilMinUnassignedForArmy &&
                   static_cast<int>(c.armies.size()) <
                       std::min(kMilMaxArmies, 1 + static_cast<int>(c.divisions.size()) /
                                                      kMilDivisionsPerArmy)) {
            Command cmd = make_command(CommandType::CreateArmy, c.id);
            cmd.text = c.tag + " " + std::to_string(c.armies.size() + 1) + ". Armee";
            if (push_if_valid(g, std::move(cmd))) {
                record_reason(g, AiLayer::Military, "create_army", 20.0,
                              {{"unassigned", static_cast<double>(unassigned.size())},
                               {"armies", static_cast<double>(c.armies.size())}});
            }
        }
    }

    // Generals: an army without one fights worse.
    for (ArmyId aid : c.armies) {
        const Army* a = w.army(aid);
        if (!a || a->general.valid() || a->divisions.empty()) continue;
        CharacterId best;
        int best_skill = -1;
        w.characters.for_each([&](CharacterId cid, const Character& ch) {
            if (ch.country != c.id || !ch.is_general || ch.army.valid()) return;
            const int skill = ch.skill + ch.attack + ch.defense;
            if (skill > best_skill) {
                best_skill = skill;
                best = cid;
            }
        });
        if (!best.valid()) break;
        Command cmd = make_command(CommandType::AssignGeneral, c.id);
        cmd.army = aid;
        cmd.character = best;
        if (push_if_valid(g, std::move(cmd))) {
            record_reason(g, AiLayer::Military, "assign_general", 10.0,
                          {{"general_skill", static_cast<double>(best_skill)}});
        }
    }

    // ---- (c) army orders ---------------------------------------------------
    OrderKind desired = OrderKind::Garrison;
    const char* desired_name = "order_garrison";
    if (war) {
        if (posture == 2) {
            desired = OrderKind::Offensive;
            desired_name = "order_offensive";
        } else if (losing) {
            desired = OrderKind::Fallback;
            desired_name = "order_fallback";
        } else {
            desired = OrderKind::FrontLine;
            desired_name = "order_frontline";
        }
    }
    for (ArmyId aid : c.armies) {
        const Army* a = w.army(aid);
        if (!a || a->divisions.empty()) continue;
        if (army_is_committed(w, aid)) continue;  // the crossing owns this army's orders
        const uint8_t stance = !war ? 2 : (posture == 2 ? 0 : 1);

        if (a->order.kind != desired || a->order.line.empty()) {
            Command cmd = make_command(CommandType::SetDivisionOrder, c.id);
            cmd.army = aid;
            cmd.value = static_cast<int32_t>(desired);
            if (push_if_valid(g, std::move(cmd))) {
                AiReason r;
                r.what = desired_name;
                r.score = assess.offensive_score;
                r.factors = assess.factors;
                record_reason(g, AiLayer::Military, std::move(r));
            }
        }
        if (a->stance != stance) {
            Command cmd = make_command(CommandType::SetStance, c.id);
            cmd.army = aid;
            cmd.value = static_cast<int32_t>(stance);
            if (push_if_valid(g, std::move(cmd))) {
                record_reason(g, AiLayer::Military, "set_stance", 8.0,
                              {{"aggressive", stance == 0 ? 1.0 : 0.0},
                               {"defensive", stance == 1 ? 1.0 : 0.0},
                               {"garrison", stance == 2 ? 1.0 : 0.0},
                               {"force_ratio", assess.force_ratio}});
            }
        }
    }

    // ---- (d) march and attack ----------------------------------------------
    // The movement phase only advances paths that already exist and an army order
    // only marks `order`/`order_target`, so every division that should move needs an
    // explicit MoveDivision. Attack targets come from the *local* balance of force on
    // the front: a division attacks the enemy province next to it when its own front
    // province is locally stronger (or nothing defends the target). That is what
    // starts battles - two armies facing each other across a border would otherwise
    // simply stare, because attacking requires someone to walk into contact.
    int moves = 0;
    if (war && !front.empty()) {
        // Strength by province, one pass over the division store: ours on one side,
        // everything we are at war with on the other.
        std::vector<double> our_strength(w.provinces.capacity() + 1, 0.0);
        std::vector<double> hostile_strength(w.provinces.capacity() + 1, 0.0);
        w.divisions.for_each([&](DivisionId, const Division& d) {
            if (!d.location.valid() || d.location.v >= our_strength.size()) return;
            if (d.country == c.id) {
                our_strength[d.location.v] += clamp01(d.strength);
            } else if (hostile_pair(w, c.id, d.country)) {
                hostile_strength[d.location.v] += clamp01(d.strength);
            }
        });

        // Best opportunity per front province.
        std::vector<ProvinceId> best_target(w.provinces.capacity() + 1, ProvinceId{});
        std::vector<double> best_score(w.provinces.capacity() + 1, 0.0);
        std::vector<double> best_ratio(w.provinces.capacity() + 1, 0.0);
        std::vector<double> best_defenders(w.provinces.capacity() + 1, 0.0);
        for (ProvinceId f : front) {
            const Province* fp = w.province(f);
            if (!fp) continue;
            const double local = our_strength[f.v];
            for (ProvinceId n : fp->adj) {
                const Province* np = w.province(n);
                if (!np || np->is_sea || terrain_is_water(np->terrain)) continue;
                if (!hostile_pair(w, c.id, np->controller)) continue;
                if (n.v >= hostile_strength.size()) continue;
                const double defenders = hostile_strength[n.v];
                // A lone defender must not look unbeatable: compare raw strength, not
                // strength against a padded denominator.
                const double ratio = safe_div(local, std::max(defenders, 0.25));
                const bool undefended = defenders <= 0.0;
                if (!undefended && ratio < kMilLocalAttackRatio) continue;
                const double score = undefended
                                         ? kMilAttackUndefended
                                         : kMilAttackBase + kMilAttackRatioWeight * clamp01(ratio - 1.0);
                if (best_target[f.v].valid() && score <= best_score[f.v]) continue;
                best_target[f.v] = n;
                best_score[f.v] = score;
                best_ratio[f.v] = ratio;
                best_defenders[f.v] = defenders;
            }
        }

        // Divisions already holding a front province take the strongest openings
        // first; the rest of the run's budget goes to filling the line.
        struct AttackOrder {
            DivisionId division;
            double score = 0.0;
            ProvinceId target;
            double local_ratio = 0.0;
            double defender_strength = 0.0;
        };
        std::vector<AttackOrder> orders;
        w.divisions.for_each([&](DivisionId did, const Division& d) {
            if (d.country != c.id || d.moving || d.retreating || d.in_combat()) return;
            if (division_committed_to_invasion(w, d)) return;  // gathering for a landing
            if (!d.location.valid() || d.location.v >= best_target.size()) return;
            if (!best_target[d.location.v].valid()) return;
            AttackOrder o;
            o.division = did;
            o.score = best_score[d.location.v];
            o.target = best_target[d.location.v];
            o.local_ratio = best_ratio[d.location.v];
            o.defender_strength = best_defenders[d.location.v];
            orders.push_back(o);
        });
        std::sort(orders.begin(), orders.end(), [](const AttackOrder& a, const AttackOrder& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.division < b.division;
        });
        int attacks = 0;
        for (const AttackOrder& o : orders) {
            if (moves >= kMilMaxMovesPerRun || attacks >= kMilMaxAttacksPerRun) break;
            const Division* d = w.division(o.division);
            if (!d) continue;
            Command cmd = make_command(CommandType::MoveDivision, c.id);
            cmd.division = o.division;
            cmd.province = o.target;
            if (!push_if_valid(g, std::move(cmd))) continue;
            AiReason r;
            r.what = "attack";
            r.score = o.score;
            r.factors = {{"local_ratio", o.local_ratio},
                         {"defender_strength", o.defender_strength},
                         {"supply", clamp01(d->supply)},
                         {"strength", clamp01(d->strength)}};
            record_reason(g, AiLayer::Military, std::move(r));
            ++moves;
            ++attacks;
        }
    }

    // Divisions away from the fighting march to the front (or hold the capital when
    // there is no front at all). They go to the thinnest part of the line: a front
    // that is only held in a few provinces can be walked around, and a line that is
    // held everywhere is what turns intrusions into battles.
    std::vector<ProvinceId> hold_objectives = front;
    if (hold_objectives.empty()) {
        const ProvinceId cap = capital_province(w, c.id);
        const Province* cp = w.province(cap);
        if (cp && cp->controller == c.id) hold_objectives.push_back(cap);
    }
    if (!hold_objectives.empty() && moves < kMilMaxMovesPerRun) {
        std::vector<int> held(w.provinces.capacity() + 1, 0);
        w.divisions.for_each([&](DivisionId, const Division& d) {
            if (d.country != c.id || !d.location.valid()) return;
            if (d.location.v < held.size()) ++held[d.location.v];
        });
        std::sort(hold_objectives.begin(), hold_objectives.end(),
                  [&](ProvinceId a, ProvinceId b) {
                      if (held[a.v] != held[b.v]) return held[a.v] < held[b.v];
                      return a < b;
                  });

        w.divisions.for_each([&](DivisionId did, const Division& d) {
            if (moves >= kMilMaxMovesPerRun) return;
            if (d.country != c.id || d.moving || d.retreating || d.in_combat()) return;
            if (division_committed_to_invasion(w, d)) return;  // gathering for a landing
            if (!d.location.valid()) return;  // still training off-map
            if (std::find(front.begin(), front.end(), d.location) != front.end()) return;  // on the line

            // The thinnest objective that is not where the division already stands.
            ProvinceId move_goal;
            for (ProvinceId o : hold_objectives) {
                if (o != d.location) {
                    move_goal = o;
                    break;
                }
            }
            if (!move_goal.valid()) return;

            Command cmd = make_command(CommandType::MoveDivision, c.id);
            cmd.division = did;
            cmd.province = move_goal;
            if (!push_if_valid(g, std::move(cmd))) return;
            AiReason r;
            r.what = "move_to_front";
            r.score = 18.0;
            r.factors = {{"line_strength", static_cast<double>(held[move_goal.v])},
                         {"supply", clamp01(d.supply)},
                         {"strength", clamp01(d.strength)},
                         {"posture_offensive", posture == 2 ? 1.0 : 0.0}};
            record_reason(g, AiLayer::Military, std::move(r));
            ++moves;
        });
    }

    // ---- (e) air ---------------------------------------------------------
    // Air planning rides the military layer: it needs the same front line and the
    // posture just decided above, and it acts through the same command queue.
    ai_air_layer(g, c);

    // ---- (f) navy --------------------------------------------------------
    // Naval planning rides the military layer for the same reason as the air layer:
    // the posture and the front line it reads are decided here, and both push their
    // orders through the same queue.
    ai_naval_layer(g, c);
}

// ============================================================= diplomacy =====

void ai_diplomacy_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;
    const Tick now = w.tick;

    const std::vector<ProvinceId> front = compute_front_line(w, c.id);
    uint8_t posture = posture_of(g.ai, c.id);
    StrategicAssessment assess = assess_strategy(g, c, front);
    if (posture == 0) {
        // The military layer owns the posture, but this layer must also work when it
        // is exercised on its own: replay the same rule instead of acting blind.
        if (at_war_state(c)) {
            posture = (assess.offensive_score >= kMilOffensiveThreshold &&
                       assess.force_ratio >= kMilOffensiveRatio)
                          ? 2
                          : 1;
        } else if (assess.neighbour_ratio >= kDipWarRatioGate) {
            posture = 2;
        }
        set_posture(g, c.id, posture);
    }

    // ---- (a) faction membership --------------------------------------------
    // A country looks for protectors when it is losing more than it is winning:
    // threat combines enemy pressure on the capital, lost territory and being
    // outmatched in an active war.
    if (c.faction == 0) {
        const double war_pressure = at_war_state(c) ? clamp01(1.0 - assess.force_ratio) : 0.0;
        const double threat =
            clamp01(assess.capital_threat * 0.5 + assess.occupied_share * 2.0 + war_pressure);

        double best_score = -1.0;
        CountryId best_leader;
        AiReason best;
        for (const Faction& f : w.factions) {
            const Country* leader = w.country(f.leader);
            if (!leader || !leader->alive || f.leader == c.id) continue;
            if (leader->ideology != c.ideology) continue;
            if (hostile_pair(w, c.id, f.leader)) continue;  // an enemy cannot be a patron
            if (f.members.empty()) continue;

            const double threat_factor = kDipFactionThreat * threat;
            const double weakness = kDipFactionWeakness * clamp01(1.0 - assess.force_ratio);
            double patron_strength = 0.0;
            for (DivisionId did : leader->divisions) {
                const Division* d = w.division(did);
                if (d) patron_strength += clamp01(d->strength);
            }
            const double patron = patron_strength > assess.own_strength ? kDipFactionPatron : 0.0;
            const double score = kDipFactionIdeology + threat_factor + weakness + patron;
            if (score > best_score) {
                best_score = score;
                best_leader = f.leader;
                best.what = "join_faction_of_" + leader->tag;
                best.score = score;
                best.factors = {{"ideology_match", kDipFactionIdeology},
                                {"threat", threat_factor},
                                {"war_pressure", war_pressure},
                                {"own_weakness", weakness},
                                {"patron_strength", patron},
                                {"force_ratio", assess.force_ratio},
                                {"capital_threat", assess.capital_threat}};
            }
        }

        if (best_leader.valid() && best_score >= kDipFactionThreshold) {
            Command cmd = make_command(CommandType::JoinFaction, c.id);
            cmd.target_country = best_leader;
            const bool accepted = push_if_valid(g, std::move(cmd));
            if (!accepted) {
                // The intent was worth acting on but the command system says no
                // (leader at war with us, no faction to join): keep the reason so the
                // debugger shows the plan that did not happen.
                best.what += "_deferred";
            }
            best.factors.emplace_back("accepted", accepted ? 1.0 : 0.0);
            record_reason(g, AiLayer::Diplomacy, std::move(best));
        }
    }

    // ---- (b) war declarations ----------------------------------------------
    // Only an offensive posture declares war, and only against a target the country
    // can actually beat: the ratio is measured against that target and its faction,
    // never against the world, so a strong army is not enough on its own.
    if (posture == 2) {
        bool recently_declared = declared_recently(g, c.id, now);
        w.wars.for_each([&](WarId, const War& war) {
            if (recently_declared || war.aggressor != c.id || !war.active) return;
            if (now < war.start_tick + kWarDeclarationCooldown) recently_declared = true;
        });

        if (!recently_declared) {
            CountryId best_target;
            double best_score = -1.0;
            AiReason best;
            for (CountryId oid : assess.neighbours) {
                const Country* other = w.country(oid);
                if (!other || !other->alive || oid == c.id) continue;
                if (other->faction != 0 && other->faction == c.faction) continue;
                if (hostile_pair(w, c.id, oid)) continue;
                if (other->ideology == c.ideology) continue;
                // Reachability is already established: `neighbours` holds exactly the
                // countries that border us, so a war we start can always be walked to.

                const double side = side_strength(w, oid, c.faction);
                const double target_ratio = safe_div(assess.own_strength, side + 0.5);
                if (target_ratio < kDipWarRatioGate) continue;

                const double force_factor = kDipWarForce * clamp01(target_ratio - 1.0);
                const double weakness_factor =
                    kDipWarWeakness * (1.0 - clamp01(safe_div(side, assess.own_strength + 0.5)));
                const double threat_factor = kDipWarOwnThreat * assess.own_threat;
                const double score =
                    force_factor + weakness_factor + kDipWarPosture - threat_factor;
                if (score > best_score) {
                    best_score = score;
                    best_target = oid;
                    best.what = "declare_war_on_" + other->tag;
                    best.score = score;
                    best.factors = {{"target_ratio", target_ratio},
                                    {"target_side_strength", side},
                                    {"claim_state", border_claim_state(w, c.id, oid).valid() ? 1.0 : 0.0},
                                    {"force", force_factor},
                                    {"target_weakness", weakness_factor},
                                    {"posture_offensive", kDipWarPosture},
                                    {"own_threat", -threat_factor},
                                    {"own_strength", assess.own_strength}};
                }
            }

            if (best_target.valid() && best_score >= kDipWarThreshold) {
                Command cmd = make_command(CommandType::DeclareWar, c.id);
                cmd.target_country = best_target;
                cmd.state = border_claim_state(w, c.id, best_target);
                if (push_if_valid(g, std::move(cmd))) {
                    record_reason(g, AiLayer::Diplomacy, std::move(best));
                }
            }
        } else {
            record_reason(g, AiLayer::Diplomacy, "declaration_deferred", 0.0,
                          {{"cooldown", kDipWarCooldown},
                           {"force_ratio", assess.force_ratio}});
        }
    }

    // ---- (c) peace ----------------------------------------------------------
    for (WarId wid : c.wars) {
        const War* war = w.war(wid);
        if (!war || !war->active) continue;
        const bool attacker = captain_of_side(*war, true, c.id);
        const std::vector<WarParticipant>& own =
            attacker ? war->attackers : war->defenders;
        const std::vector<WarParticipant>& foe =
            attacker ? war->defenders : war->attackers;

        double own_home = 0.0, foe_home = 0.0, own_lost = 0.0, foe_taken = 0.0;
        w.provinces.for_each([&](ProvinceId, const Province& p) {
            if (p.is_sea) return;
            if (side_has(own, p.owner)) {
                own_home += 1.0;
                if (side_has(foe, p.controller)) own_lost += 1.0;
            } else if (side_has(foe, p.owner)) {
                foe_home += 1.0;
                if (side_has(own, p.controller)) foe_taken += 1.0;
            }
        });

        const double own_lost_share = clamp01(safe_div(own_lost, own_home));
        const double foe_taken_share = clamp01(safe_div(foe_taken, foe_home));
        const double best_share = std::max(own_lost_share, foe_taken_share);
        const double score = kDipPeaceOccupation * best_share;
        // Only ask when the war is already decided in this country's favour or
        // against it, which is exactly when the diplomacy phase would settle it.
        if (!war_decided_for(w, *war, c.id)) continue;

        const bool winning = foe_taken_share >= own_lost_share;
        Command cmd = make_command(CommandType::OfferPeace, c.id);
        cmd.war = wid;
        if (!push_if_valid(g, std::move(cmd))) continue;
        AiReason r;
        r.what = winning ? "offer_peace_winning" : "sue_for_peace";
        r.score = score;
        r.factors = {{"enemy_occupied_share", foe_taken_share},
                     {"own_occupied_share", own_lost_share},
                     {"force_ratio", assess.force_ratio}};
        record_reason(g, AiLayer::Diplomacy, std::move(r));
        break;  // one peace offer per run
    }
}

// =============================================================== phase =======

namespace {

// Cadence of a layer. AiLayerState ships with the generic 24-tick value, so that
// value (and zero) is read as "not configured" and the layer's own default applies;
// the military layer therefore plans twice a day unless a caller pinned something
// else. The resolved cadence is written back on the first plan, after which the
// state is explicit.
uint32_t layer_interval(const AiLayerState& st, int layer) {
    if (st.interval_ticks != 0 && st.interval_ticks != kDefaultInterval) return st.interval_ticks;
    return layer == static_cast<int>(AiLayer::Military) ? kMilitaryInterval : kDefaultInterval;
}

// Returns true when this layer plans during this tick. `last_run_tick` stores the
// next tick the layer is scheduled for (last run + its interval); zero means "never
// planned", so every layer makes a first plan on the first AI phase instead of
// waiting a day for a schedule that does not exist yet.
bool layer_due(AiLayerState& st, int layer, Tick now) {
    const uint32_t interval = layer_interval(st, layer);
    st.interval_ticks = interval;
    if (st.last_run_tick != 0 && now < static_cast<Tick>(st.last_run_tick)) return false;
    const Tick next = now + static_cast<Tick>(interval);
    st.last_run_tick = next > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(next);
    return true;
}

double timed_call(Game& g, AiLayer l, Country& c, void (*fn)(Game&, Country&)) {
    const auto start = std::chrono::steady_clock::now();
    fn(g, c);
    double* slot = nullptr;
    switch (l) {
        case AiLayer::Industry: slot = &g.metrics.ms_ai_industry; break;
        case AiLayer::Research: slot = &g.metrics.ms_ai_research; break;
        case AiLayer::Production: slot = &g.metrics.ms_ai_production; break;
        case AiLayer::Military: slot = &g.metrics.ms_ai_military; break;
        case AiLayer::Politics: slot = &g.metrics.ms_ai_politics; break;
        case AiLayer::Diplomacy: slot = &g.metrics.ms_ai_diplomacy; break;
        case AiLayer::Count: break;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                start)
                          .count();
    if (slot) *slot += ms;
    return ms;
}

void run_layer(Game& g, AiLayer l, Country& c) {
    switch (l) {
        case AiLayer::Industry: {
            timed_call(g, l, c, &ai_industry_layer);
            timed_call(g, l, c, &ai_trade_layer);
            const auto design_start = std::chrono::steady_clock::now();
            ai_design_layer(g, c);
            g.metrics.ms_ai_design +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          design_start)
                    .count();
            break;
        }
        case AiLayer::Research: timed_call(g, l, c, &ai_research_layer); break;
        case AiLayer::Production: timed_call(g, l, c, &ai_production_layer); break;
        case AiLayer::Military: timed_call(g, l, c, &ai_military_layer); break;
        case AiLayer::Politics: timed_call(g, l, c, &ai_politics_layer); break;
        case AiLayer::Diplomacy: timed_call(g, l, c, &ai_diplomacy_layer); break;
        case AiLayer::Count: break;
    }
}

}  // namespace

// The politics layer aggregates focus selection, event choices and decisions; the
// individual parts live in src/sim/focus.cpp and src/sim/events.cpp.
void ai_politics_layer(Game& g, Country& c) {
    ai_focus_layer(g, c);
    ai_event_layer(g, c);
    ai_decision_layer(g, c);
    ai_spirit_advisor_layer(g, c);
}

void phase_ai(Game& g) {
    const Tick now = g.world.tick;
    ensure_posture_capacity(g.ai, g.world.countries.capacity());

    // Decide which layers plan this tick before walking the countries, so every AI
    // country in the same tick plans on the same schedule.
    constexpr size_t kLayers = static_cast<size_t>(AiLayer::Count);
    std::array<bool, kLayers> due{};
    bool any_due = false;
    for (size_t i = 0; i < kLayers; ++i) {
        due[i] = layer_due(g.ai.layers[i], static_cast<int>(i), now);
        if (due[i]) {
            g.ai.layers[i].last_reasons.clear();
            any_due = true;
        }
    }
    if (!any_due) return;

    g.world.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive || !g.is_ai(id)) return;
        for (size_t i = 0; i < kLayers; ++i) {
            if (due[i]) run_layer(g, static_cast<AiLayer>(i), c);
        }
    });
}

}  // namespace hoi
