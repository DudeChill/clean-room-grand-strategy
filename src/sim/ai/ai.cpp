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
#include "sim/diplomacy.h"
#include "sim/industry.h"
#include "sim/map.h"
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
constexpr int kMilMaxArmies = 6;
constexpr int kMilMinUnassignedForArmy = 4;
constexpr int kMilMaxMovesPerRun = 4;
constexpr double kMilEquipmentReady = 0.5;  // stockpile share needed to train
constexpr double kMilOffensiveThreshold = 45.0;
constexpr double kMilOffensiveRatio = 1.15;
constexpr double kMilFallbackRatio = 0.75;
constexpr double kOrderForceWeight = 40.0;
constexpr double kOrderSupplyWeight = 20.0;
constexpr double kOrderFrontWeight = 10.0;
constexpr double kOrderCapitalThreatWeight = 40.0;

// Diplomacy.
constexpr double kDipFactionThreat = 35.0;
constexpr double kDipFactionIdeology = 20.0;
constexpr double kDipFactionWeakness = 10.0;
constexpr double kDipFactionPatron = 10.0;
constexpr double kDipFactionThreshold = 40.0;
constexpr double kDipWarForce = 40.0;
constexpr double kDipWarWeakness = 20.0;
constexpr double kDipWarPosture = 20.0;
constexpr double kDipWarCooldown = -100.0;
constexpr double kDipWarRatioGate = 1.5;
constexpr double kDipWarThreshold = 50.0;
constexpr double kDipPeaceOccupation = 60.0;
constexpr double kDipPeaceThreshold = 30.0;
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
// does not flag one.
ProvinceId capital_province(const World& w, const Country& c) {
    const State* s = w.state(c.capital);
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
    std::vector<char> mask(equipment_count, 0);
    for (TemplateId t : c.templates) {
        const DivisionTemplate* td = content.template_def(t);
        if (!td) continue;
        for (const BattalionSlot& b : td->battalions) {
            if (b.equipment.valid() && b.equipment.v < equipment_count) mask[b.equipment.v] = 1;
        }
    }
    return mask;
}

struct IndustryCandidate {
    BuildingKind kind = BuildingKind::CivilianFactory;
    StateId state;
    ProvinceId province;
    double score = 0.0;
    AiReason reason;
};

// ------------------------------------------------------------------ posture --

// Strategic assessment shared by the military and diplomacy layers: force ratio
// against everyone the country is at war with, supply health, and how close the
// enemy is to the capital. All of it is information a player could read off the
// map and the ledger.
struct StrategicAssessment {
    double own_strength = 0.0;
    double enemy_strength = 0.0;
    double force_ratio = 0.0;
    double avg_supply = 1.0;
    double capital_threat = 0.0;   // 0..1
    double occupied_share = 0.0;   // own provinces controlled by a hostile
    double offensive_score = 0.0;
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
    const ProvinceId cap = capital_province(w, c);
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
    a.factors.emplace_back("offensive_score", a.offensive_score);
    return a;
}

bool at_war_state(const Country& c) { return c.at_war || !c.wars.empty(); }

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

// Military factories build land and air equipment; ships and convoys come out of
// dockyards, which this layer does not assign.
bool factory_producible(EquipmentCategory c) {
    return c != EquipmentCategory::Ship && c != EquipmentCategory::Convoy;
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

}  // namespace

void ai_production_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;
    int civ = 0, mil = 0, dock = 0;
    count_factories(w, c.id, &civ, &mil, &dock);
    if (mil <= 0) return;

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

    // ---- pick the models worth a line, upgrading to the best known model ----
    const int year = g.world.date.year;
    const std::vector<char> template_mask =
        template_equipment_mask(g.content, c, g.content.equipment.size());
    std::vector<ProdTarget> targets;
    for (size_t i = 0; i < g.content.equipment.size(); ++i) {
        if (need[i] <= 0.0) continue;
        const EquipmentDef& def = g.content.equipment[i];
        if (def.is_archetype || !factory_producible(def.category)) continue;

        // Upgrade within the archetype when the new model is measurably better and
        // the efficiency thrown away by switching is smaller than the gain.
        const EquipmentDef* best = &def;
        for (size_t j = 0; j < g.content.equipment.size(); ++j) {
            const EquipmentDef& other = g.content.equipment[j];
            if (other.is_archetype || other.archetype != def.archetype) continue;
            if (other.year > year) continue;
            if (equipment_stat_score(other) > equipment_stat_score(*best)) best = &other;
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
                need[best->id.v] += need[i];
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

    // No demand at all (fresh country, empty templates): still keep one line on the
    // best unlocked infantry model so the country is not caught without a rifle.
    if (targets.empty()) {
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
        if (!fallback.valid()) return;
        targets.push_back(ProdTarget{fallback, 1.0, 1.0});
    }

    std::sort(targets.begin(), targets.end(), [](const ProdTarget& a, const ProdTarget& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.equipment < b.equipment;
    });

    // One line per distinct need, but never more lines than the country has
    // military factories to fill them.
    const int line_count =
        std::max(kProdMinLines, std::min({kProdMaxLines, static_cast<int>(targets.size()), mil}));
    targets.resize(static_cast<size_t>(line_count));

    // ---- factories proportional to need -------------------------------------
    double need_sum = 0.0;
    for (const ProdTarget& t : targets) need_sum += t.need;
    std::vector<int> want(targets.size(), 1);
    {
        int assigned = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            const double share = safe_div(targets[i].need, need_sum);
            int f = static_cast<int>(std::floor(static_cast<double>(mil) * share));
            f = std::max(1, std::min(f, mil));
            want[i] = f;
            assigned += f;
        }
        // The rounding remainder goes to the highest-need target first, which keeps
        // the split deterministic and monotone in need.
        int leftover = mil - assigned;
        for (size_t i = 0; i < targets.size() && leftover > 0; ++i) {
            want[i] += 1;
            --leftover;
        }
    }

    // ---- turn the plan into commands ---------------------------------------
    // Removals first, then reductions, then raises: every intermediate state stays
    // inside the factory budget the command system checks.
    const int assigned_before = [&] {
        int total = 0;
        for (const ProductionLine& line : c.lines) total += line.factories;
        return total;
    }();
    int assigned_live = assigned_before;

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
        bool kept = false;
        for (const ProdTarget& t : targets) {
            if (t.equipment == line.equipment) kept = true;
        }
        if (!kept) {
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
        if (next > mil) continue;  // keep the batch inside the factory budget
        Command cmd = make_command(CommandType::SetProductionLine, c.id);
        cmd.equipment = op.equipment;
        cmd.value = op.factories;
        if (!push_if_valid(g, std::move(cmd))) continue;
        assigned_live = next;
        record_reason(g, AiLayer::Production, op.reason);
    }
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
double equipment_readiness(const Country& c, const DivisionTemplate& td) {
    double worst = 1.0;
    for (const BattalionSlot& b : td.battalions) {
        if (b.count <= 0 || !b.equipment.valid()) continue;
        double have = 0.0;
        if (b.equipment.v < c.equipment_stockpile.size()) have = c.equipment_stockpile[b.equipment.v];
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

}  // namespace

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
    }
    set_posture(g, c.id, posture);

    // Front-facing province used as the deployment and movement goal.
    ProvinceId goal;
    if (!front.empty()) {
        goal = front[0];
    } else {
        const ProvinceId cap = capital_province(w, c);
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
            const double readiness = equipment_readiness(c, *td);
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
                   static_cast<int>(c.armies.size()) < kMilMaxArmies) {
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
        const uint8_t stance = posture == 2 ? 0 : (posture == 0 ? 2 : 1);

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

    // ---- (d) walk idle divisions to the front ------------------------------
    if (!front.empty()) {
        int moves = 0;
        PathRequest req;
        req.country = c.id;
        req.require_controlled = true;
        w.divisions.for_each([&](DivisionId did, const Division& d) {
            if (moves >= kMilMaxMovesPerRun) return;
            if (d.country != c.id || d.moving || d.in_combat() || d.army.valid()) return;
            if (!d.location.valid()) return;
            bool already_there = false;
            for (ProvinceId f : front) {
                if (f == d.location) already_there = true;
            }
            if (already_there) return;

            // Nearest front province in land distance, then the first step of the
            // path towards it.
            std::vector<int32_t> hops;
            compute_hop_distances(w, d.location, req, &hops);
            ProvinceId best;
            int best_hops = -1;
            for (ProvinceId f : front) {
                if (f.v >= hops.size() || hops[f.v] < 0) continue;
                if (best_hops < 0 || hops[f.v] < best_hops) {
                    best_hops = static_cast<int>(hops[f.v]);
                    best = f;
                }
            }
            if (!best.valid()) return;
            const std::vector<ProvinceId> path = find_path(w, d.location, best, req);
            if (path.empty()) return;
            Command cmd = make_command(CommandType::MoveDivision, c.id);
            cmd.division = did;
            cmd.province = path.front();
            if (!push_if_valid(g, std::move(cmd))) return;
            record_reason(g, AiLayer::Military, "move_to_front", 18.0,
                          {{"hops_to_front", static_cast<double>(best_hops)},
                           {"supply", clamp01(d.supply)}});
            ++moves;
        });
    }
}

// ============================================================= diplomacy =====

void ai_diplomacy_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;
    const Tick now = w.tick;

    const std::vector<ProvinceId> front = compute_front_line(w, c.id);
    uint8_t posture = posture_of(g.ai, c.id);
    StrategicAssessment assess = assess_strategy(g, c, front);
    if (posture == 0 && at_war_state(c)) {
        posture = assess.offensive_score >= kMilOffensiveThreshold ? 2 : 1;
        set_posture(g, c.id, posture);
    }

    // ---- (a) faction intent -------------------------------------------------
    // The engine has no command that joins a faction, so this stays a recorded
    // plan: the AI evaluates the move and the debugger explains it, but the
    // country's faction membership is not something the AI may write.
    if (c.faction == 0) {
        double best_score = -1.0;
        AiReason best;
        for (const Faction& f : w.factions) {
            const Country* leader = w.country(f.leader);
            if (!leader || !leader->alive || f.leader == c.id) continue;
            if (leader->ideology != c.ideology) continue;
            if (f.members.empty()) continue;
            const double threat = clamp01(assess.capital_threat * 0.5 + assess.occupied_share * 2.0);
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
                best.what = "join_faction_of_" + leader->tag;
                best.score = score;
                best.factors = {{"ideology_match", kDipFactionIdeology},
                                {"threat", threat_factor},
                                {"own_weakness", weakness},
                                {"patron_strength", patron},
                                {"force_ratio", assess.force_ratio},
                                {"capital_threat", assess.capital_threat}};
            }
        }
        if (best_score >= kDipFactionThreshold) record_reason(g, AiLayer::Diplomacy, std::move(best));
    }

    // ---- (b) war declarations ----------------------------------------------
    if (posture == 2 && assess.force_ratio >= kDipWarRatioGate) {
        bool recently_declared = false;
        w.wars.for_each([&](WarId, const War& war) {
            if (war.aggressor != c.id || !war.active) return;
            if (now < war.start_tick + kWarDeclarationCooldown) recently_declared = true;
        });

        if (!recently_declared) {
            CountryId best_target;
            double best_score = -1.0;
            AiReason best;
            w.countries.for_each([&](CountryId oid, const Country& other) {
                if (oid == c.id || !other.alive) return;
                if (other.faction != 0 && other.faction == c.faction) return;
                if (hostile_pair(w, c.id, oid)) return;
                if (other.ideology == c.ideology) return;
                // Only neighbours are reachable: a war the army cannot walk to is
                // a war the country should not start.
                bool adjacent_territory = false;
                w.provinces.for_each([&](ProvinceId, const Province& p) {
                    if (adjacent_territory || p.is_sea || p.controller != c.id) return;
                    for (ProvinceId a : p.adj) {
                        if (w.province_controller(a) == oid) {
                            adjacent_territory = true;
                            return;
                        }
                    }
                });
                if (!adjacent_territory) return;

                double target_strength = 0.0;
                for (DivisionId did : other.divisions) {
                    const Division* d = w.division(did);
                    if (d) target_strength += clamp01(d->strength);
                }
                const double force_factor = kDipWarForce * clamp01(assess.force_ratio - 1.0);
                const double weakness_factor =
                    kDipWarWeakness * (1.0 - clamp01(safe_div(target_strength, assess.own_strength + 0.5)));
                const double score = force_factor + weakness_factor + kDipWarPosture;
                if (score > best_score) {
                    best_score = score;
                    best_target = oid;
                    best.what = "declare_war_on_" + other.tag;
                    best.score = score;
                    best.factors = {{"force_ratio", assess.force_ratio},
                                    {"force", force_factor},
                                    {"target_weakness", weakness_factor},
                                    {"posture_offensive", kDipWarPosture},
                                    {"target_strength", target_strength}};
                }
            });

            if (best_target.valid() && best_score >= kDipWarThreshold) {
                Command cmd = make_command(CommandType::DeclareWar, c.id);
                cmd.target_country = best_target;
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
        if (score < kDipPeaceThreshold) continue;

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

void run_layer(Game& g, AiLayer l, Country& c) {
    switch (l) {
        case AiLayer::Industry: ai_industry_layer(g, c); break;
        case AiLayer::Research: ai_research_layer(g, c); break;
        case AiLayer::Production: ai_production_layer(g, c); break;
        case AiLayer::Military: ai_military_layer(g, c); break;
        case AiLayer::Diplomacy: ai_diplomacy_layer(g, c); break;
        case AiLayer::Count: break;
    }
}

}  // namespace

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
