// Intelligence subsystem (INT-001): agencies, spy networks, operations, decryption
// and the two combat hooks that turn knowledge into an edge on the battlefield.
//
// Model. A country owns an intelligence agency (a set of agency upgrades), keeps a
// spy network in other countries (strength 0..100, exposure 0..1), runs data-driven
// operations against them and slowly breaks their ciphers. Everything derives from
// world state - no level is cached - so a phase can never disagree with the UI.
//
// ------------------------------------------------------------------- formulas --
//
// All numbers are either content data (OperationDef / AgencyUpgradeDef) or one of the
// named constants below. Iteration is always ascending id; the only randomness is the
// burn roll, which uses RngStream::Intel.
//
//   network growth (per day, applied as 1/24 per tick)
//       (kIntelBaseNetworkGrowth + sum of owned upgrades' network_growth)
//       * (1 - counter_intel_level(target))
//       * (at_war(owner, target) ? 1.0 : kIntelPeaceSpyFactor)
//     It only applies while the agency has a free operation slot (spying is passive
//     work; a service already running kIntelOperationSlots operations has no people
//     left to recruit). Spying on a country you are at war with is the fast case;
//     against everyone else the network grows at kIntelPeaceSpyFactor of the rate.
//
//   counter_intel_level = clamp(sum of owned upgrades' counter_intel, 0,
//                               kIntelCounterIntelCap)
//     A country is never unspyable: the sum is capped by kIntelCounterIntelCap. The
//     engine's ModifierKind vocabulary has no counter-intelligence modifier (see
//     src/sim/units.h), so the national term of the documented formula is 0 and only
//     agency upgrades contribute.
//
//   decryption_level = clamp01(progress), accrued per day at
//       (kIntelBaseCrypto + sum of owned upgrades' crypto_speed)
//       * (1 - counter_intel_level(target))
//     A cipher entry is created when a network is established; decryption is the
//     passive yield of that presence.
//
//   intel_level = clamp(kIntelNetworkWeight * clamp01(strength/100)
//                       + kIntelCryptoWeight * decryption_level, 0, kIntelIntelCap)
//     kIntelIntelCap < 1: knowledge is never perfect.
//
//   intel_attack_bonus(attacker, defender) =
//       kIntelAttackBonusPerLevel * intel_level(attacker, defender)
//   decryption_planning_penalty(attacker, defender) =
//       kIntelPlanningPenaltyPerLevel * decryption_level(defender, attacker)
//     The second one is read by the defender: broken traffic costs the attacker part
//     of its planning bonus.
//
// The nine constants above are fixed by the project (they are data-independent base
// values); the remaining tuning constants below are local to this phase.
//
//   operation pace (per day) =
//       (1 + sum of owned upgrades' operation_speed)
//       * clamp01(network_strength / max(network_required, 1))
//     An operation crawls until the network is strong enough and then runs at full
//     speed; the ratio is capped at 1 so a strong network never runs it faster than
//     the agency's own speed upgrades allow.
//
//   burn chance (checked once, when the operation completes) =
//       clamp01(risk * (1 + exposure))
//     Exposure is the pressure that decides whether the next operation burns the
//     network (src/sim/world.h), so it scales the data risk. A burned operation is
//     lost, the network loses kIntelBurnStrengthRetained of its strength and its
//     exposure jumps by kIntelBurnExposureAdd.
//
// -------------------------------------------------------------------- effects --
//
// On completion an operation applies, in this order: network_gain to the owner's
// network, research_days to the owner's first active research slot, timed modifiers
// on the *target* for output_penalty / stability_delta / ideology_shift (all three
// with the same effect_days lifetime), and finally the optional script `effect`
// block, evaluated with the target as its country scope.
//
// The engine has no ruling-party-support vector, so ideology_shift is carried by
// ModifierKind::WarSupport - the closest existing modifier for ideological pressure.

#include "sim/intel.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/script.h"

namespace hoi {
namespace {

// ------------------------------------------------------------- tuning -------
// Every number below is a named constant; nothing is buried in a formula.

// Project-fixed base values (Main pinned these so content and code cannot drift).
constexpr double kIntelBaseNetworkGrowth = 0.40;   // network strength/day, no upgrades
constexpr double kIntelBaseCrypto = 0.15;          // decryption progress/day, no upgrades
constexpr double kIntelCounterIntelCap = 0.80;     // a country is never unspyable
constexpr double kIntelPeaceSpyFactor = 0.5;       // growth vs a country you are not at war with
constexpr double kIntelIntelCap = 0.85;            // knowledge is never perfect
constexpr double kIntelNetworkWeight = 0.75;       // network share of intel_level
constexpr double kIntelCryptoWeight = 0.25;        // decrypted share of intel_level
constexpr double kIntelAttackBonusPerLevel = 0.10;  // +10% attack at a full intel level
constexpr double kIntelPlanningPenaltyPerLevel = 0.50;  // up to half the planning bonus denied

// Local tuning, owned by this phase.
// Spy networks are 0..100 (SpyNetwork::strength).
constexpr double kIntelMaxNetworkStrength = 100.0;
// Operations an agency can run at once; a full agency stops growing networks.
constexpr int kIntelOperationSlots = 3;
// Exposure added per operation-day of work, and its passive decay per day.
constexpr double kIntelExposurePerOperationDay = 0.04;
constexpr double kIntelExposureDecayPerDay = 0.05;
// A burned network loses half its strength and jumps in exposure.
constexpr double kIntelBurnStrengthRetained = 0.5;
constexpr double kIntelBurnExposureAdd = 0.50;

// AI policy. The AI keeps only a small political-power buffer: the agency competes
// with focuses, decisions and advisors, so a large reserve meant no country ever
// bought an upgrade (measured on the shipped scenario: mean PP ~74 while the cheapest
// upgrade costs 120).
constexpr double kIntelAiPpReserve = 25.0;
// A network is built until it reaches this strength; past it - and past the
// requirement of the best operation the country could run there - build_network is
// never started again (the network is already strong).
constexpr double kIntelAiNetworkTargetStrength = 60.0;
// Weights that turn an OperationDef's data into one comparable value score, in
// "useful points": a research-day stolen, a point of output cut, a point of stability
// or ruling-party support swung, a strength point of an enemy network hunted, and a
// strength point added to a network.
constexpr double kIntelAiValueResearchDay = 1.0;
constexpr double kIntelAiValueOutputPenalty = 200.0;
constexpr double kIntelAiValueStability = 300.0;
constexpr double kIntelAiValueIdeology = 300.0;
constexpr double kIntelAiValueCounterIntel = 60.0;
constexpr double kIntelAiValueNetworkGain = 5.0;
// The AI debugger cap, matching the other layers (src/sim/focus.cpp, spirits.cpp).
constexpr size_t kIntelMaxReasons = 32;

constexpr double kEps = 1e-9;

// ------------------------------------------------------------- helpers ------

// Pure trigger evaluation for read-only availability checks: the script engine wants
// a mutable Game pointer for its RNG, but neither check writes gameplay state.
ScriptScope const_country_scope(const Game& g, CountryId country) {
    ScriptScope scope;
    scope.game = const_cast<Game*>(&g);
    scope.country = country;
    return scope;
}

// Script effects of a completed operation run against the target country, with the
// acting country available as `target_country` in the scope.
ScriptScope target_scope(Game* g, CountryId target, CountryId from) {
    ScriptScope scope;
    scope.game = g;
    scope.country = target;
    scope.target_country = from;
    return scope;
}

bool holds_upgrade(const Country& c, uint32_t index) {
    return std::find(c.agency_upgrades.begin(), c.agency_upgrades.end(), index) !=
           c.agency_upgrades.end();
}

// Network entry for `target`, or nullptr.
SpyNetwork* find_network(Country& c, CountryId target) {
    for (SpyNetwork& n : c.networks) {
        if (n.target == target) return &n;
    }
    return nullptr;
}

// Inserts an empty network entry unless one already exists. Networks stay sorted by
// target id (the order every reader relies on).
void ensure_network(Country& c, CountryId target) {
    auto it = std::lower_bound(c.networks.begin(), c.networks.end(), target,
                               [](const SpyNetwork& n, CountryId t) { return n.target < t; });
    if (it != c.networks.end() && it->target == target) return;
    SpyNetwork n;
    n.target = target;
    c.networks.insert(it, n);
}

// Inserts a zero-progress cipher entry unless one already exists (sorted by target).
void ensure_cipher(Country& c, CountryId target) {
    auto it = std::lower_bound(c.ciphers.begin(), c.ciphers.end(), target,
                               [](const CipherProgress& cp, CountryId t) { return cp.target < t; });
    if (it != c.ciphers.end() && it->target == target) return;
    CipherProgress cp;
    cp.target = target;
    c.ciphers.insert(it, cp);
}

// Inserts an operation keeping Country::operations sorted by (target, operation).
void insert_operation(Country& c, IntelOperation op) {
    const auto less = [](const IntelOperation& a, const IntelOperation& b) {
        if (a.target != b.target) return a.target < b.target;
        return a.operation < b.operation;
    };
    auto it = std::lower_bound(c.operations.begin(), c.operations.end(), op, less);
    c.operations.insert(it, op);
}

// Network growth multiplier against `target`: full speed against a country the owner
// is at war with, kIntelPeaceSpyFactor against everyone else.
double spying_factor(const Game& g, CountryId owner, CountryId target) {
    return g.world.at_war(owner, target) ? 1.0 : kIntelPeaceSpyFactor;
}

// Sum of the agency_upgrades fields the phase needs, in one ascending pass.
struct AgencySums {
    double network_growth = 0.0;
    double operation_speed = 0.0;
    double crypto_speed = 0.0;
};

AgencySums agency_sums(const Game& g, const Country& c) {
    AgencySums s;
    for (uint32_t u : c.agency_upgrades) {  // ascending id
        const AgencyUpgradeDef* def = g.content.agency_upgrade(u);
        if (def == nullptr) continue;
        s.network_growth += def->network_growth;
        s.operation_speed += def->operation_speed;
        s.crypto_speed += def->crypto_speed;
    }
    return s;
}

// True when at least one other country has a live network inside `country`. Used by
// the kind-specific gate of CounterIntel operations.
bool has_enemy_network_at_home(const Game& g, CountryId country) {
    bool found = false;
    g.world.countries.for_each([&](CountryId owner, const Country& c) {
        if (found || owner == country || !c.alive) return;
        for (const SpyNetwork& n : c.networks) {
            if (n.target == country && n.strength > 0.0) {
                found = true;
                return;
            }
        }
    });
    return found;
}

// Adds research-days to the country's first active slot. One stolen document dump
// advances the project that is actually running; with no slot active there is
// nothing to advance.
void grant_research_days(Country& c, double days) {
    if (!(days > 0.0)) return;
    for (ResearchSlot& slot : c.research.slots) {
        if (!slot.active || !slot.tech.valid()) continue;
        slot.progress += days;
        if (!std::isfinite(slot.progress)) slot.progress = 0.0;
        return;
    }
}

// Raises (or creates) the owner's network in `target` by `gain` strength points.
void raise_network(Country& owner, CountryId target, double gain) {
    if (!(gain > 0.0)) return;
    ensure_network(owner, target);
    SpyNetwork* n = find_network(owner, target);
    if (n == nullptr) return;
    n->strength = clamp(n->strength + gain, 0.0, kIntelMaxNetworkStrength);
}

// Applies a completed operation's data effects.
void apply_operation_effects(Game& g, Country& owner, CountryId owner_id, CountryId target_id,
                             const OperationDef& def) {
    raise_network(owner, target_id, def.network_gain);
    grant_research_days(owner, def.research_days);

    Country* target = g.world.country(target_id);
    if (target == nullptr) return;

    if (def.output_penalty != 0.0 || def.stability_delta != 0.0 || def.ideology_shift != 0.0) {
        TimedModifier tm;
        tm.source = "intel:" + def.key;
        tm.days_left = def.effect_days;
        // An output penalty is a negative FactoryOutput modifier; stability and
        // ideology land on their closest modifier kinds (see the file header).
        tm.mods.add(ModifierKind::FactoryOutput, -def.output_penalty);
        tm.mods.add(ModifierKind::Stability, def.stability_delta);
        tm.mods.add(ModifierKind::WarSupport, def.ideology_shift);
        target->timed_modifiers.push_back(std::move(tm));
    }

    if (!def.effect.is_null()) {
        apply_effects(target_scope(&g, target_id, owner_id), def.effect);
    }
}

// Burns the owner's network in `target`: exposure up, strength cut. Leaves the entry
// in place so the country can rebuild it.
void burn_network(Country& owner, CountryId target) {
    SpyNetwork* n = find_network(owner, target);
    if (n == nullptr) return;
    n->exposure = clamp01(n->exposure + kIntelBurnExposureAdd);
    n->strength = clamp(n->strength * kIntelBurnStrengthRetained, 0.0, kIntelMaxNetworkStrength);
}

// ------------------------------------------------------------- AI helpers ---

// A new reason appends while there is room; once full it replaces the weakest, so
// the debugger shows the decisions that actually drove the country. The AI layer
// records into the politics slot: the AI layer enum (src/sim/ai/ai.h) has no
// intelligence entry, and intelligence is a political instrument.
void record_reason(Game& g, AiReason reason) {
    reason.score = finite_or(reason.score, 0.0);
    for (auto& f : reason.factors) f.second = finite_or(f.second, 0.0);
    AiLayerState& st = g.ai.layer(AiLayer::Politics);
    if (st.last_reasons.size() < kIntelMaxReasons) {
        st.last_reasons.push_back(std::move(reason));
    } else {
        size_t weakest = 0;
        for (size_t i = 1; i < st.last_reasons.size(); ++i) {
            if (st.last_reasons[i].score < st.last_reasons[weakest].score) weakest = i;
        }
        if (reason.score > st.last_reasons[weakest].score) {
            st.last_reasons[weakest] = std::move(reason);
        }
    }
    ++g.ai.decisions_made;
}

// Every AI action is a command, checked by the command system before it is queued -
// the same path a human's order takes. An illegal command is never queued.
bool push_if_valid(Game& g, Command cmd) {
    cmd.issued_tick = g.world.tick;
    if (validate_command(g, cmd) != CommandResult::Applied) return false;
    g.queue.push(std::move(cmd));
    ++g.ai.commands_issued;
    return true;
}

// Total enemy network strength inside `country`, clamped to 100. Counter-intelligence
// is only worth running when there is something to hunt.
double enemy_network_strength_at_home(const Game& g, CountryId country) {
    double total = 0.0;
    g.world.countries.for_each([&](CountryId other, const Country& c) {
        if (other == country || !c.alive) return;
        for (const SpyNetwork& n : c.networks) {
            if (n.target == country) total += std::max(0.0, finite_or(n.strength, 0.0));
        }
    });
    return clamp(finite_or(total, 0.0), 0.0, kIntelMaxNetworkStrength);
}

// One comparable value score for an operation. The weights are named above; the scale
// is deliberately coarse so the AI prefers what is useful, not what is cheap (an
// operation's pp_cost never enters the value). `counter_pressure` is the enemy network
// strength at home, precomputed once per AI call.
double operation_value(const OperationDef& def, double counter_pressure) {
    double value = def.research_days * kIntelAiValueResearchDay;
    value += def.output_penalty * kIntelAiValueOutputPenalty;
    value += std::fabs(def.stability_delta) * kIntelAiValueStability;
    value += std::fabs(def.ideology_shift) * kIntelAiValueIdeology;
    if (def.kind == OperationKind::CounterIntel) {
        value += kIntelAiValueCounterIntel * counter_pressure / kIntelMaxNetworkStrength;
    }
    if (def.kind == OperationKind::BuildNetwork) {
        value += def.network_gain * kIntelAiValueNetworkGain;
    }
    return finite_or(value, 0.0);
}

// Network strength worth reaching in `target`: the standing target strength, raised to
// the requirement of the most valuable non-network operation the country could run
// there (its trigger passing). build_network is only started while the network is
// below this, so it is never thrown at an already strong network.
double build_target_strength(const Game& g, CountryId actor, CountryId target,
                             double counter_pressure) {
    double threshold = kIntelAiNetworkTargetStrength;
    double best_value = -1.0;
    for (uint32_t op = 0; op < g.content.operations.size(); ++op) {
        const OperationDef* def = g.content.operation(op);
        if (def == nullptr || def->kind == OperationKind::BuildNetwork) continue;
        if (!def->available.is_null() &&
            !eval_trigger(const_country_scope(g, actor), def->available)) {
            continue;
        }
        const double value = operation_value(*def, counter_pressure);
        if (value > best_value + kEps) {
            best_value = value;
            threshold = std::max(threshold, def->network_required);
        }
    }
    return threshold;
}

// True when this operation is already in flight against the target, or a start for it
// is already queued by this AI call.
bool already_working(const Game& g, const Country& c, CountryId target, uint32_t op) {
    for (const IntelOperation& io : c.operations) {
        if (io.target == target && io.operation == op) return true;
    }
    const OperationDef* def = g.content.operation(op);
    if (def == nullptr) return true;
    for (const Command& qc : g.queue.pending) {
        if (qc.type == CommandType::StartIntelOperation && qc.country == c.id &&
            qc.target_country == target && qc.text == def->key) {
            return true;
        }
    }
    return false;
}

// Operations the AI keeps in flight at once. The phase allows kIntelOperationSlots, but
// the agency also has to pay for its own upgrades: an eager service that fills every
// slot drains ~2 pp/day, which on the shipped scenario left mean political power at 44
// and no country ever reached the price of its first upgrade. One operation at a time
// costs less than the ~2 pp/day income, so the agency grows while it works.
constexpr int kIntelAiMaxOperationsInFlight = 1;

// Political power the AI keeps untouched for its agency while it still has an upgrade
// it cannot afford yet: the cheapest currently eligible upgrade's price plus the buffer.
// Operations are funded only from the surplus above this, so saving for the next upgrade
// is not raided by cheap operations. When nothing is eligible (a year gap in the tree)
// the floor drops to the buffer and the service works on its ordinary budget.
double upgrade_saving_floor(const Game& g, const Country& c) {
    bool found = false;
    double cheapest = 0.0;
    for (uint32_t u = 0; u < g.content.agency_upgrades.size(); ++u) {
        if (!intel_upgrade_available(g, c.id, u)) continue;
        const AgencyUpgradeDef* def = g.content.agency_upgrade(u);
        if (def == nullptr) continue;
        if (!found || def->pp_cost < cheapest - kEps) {
            found = true;
            cheapest = def->pp_cost;
        }
    }
    return found ? cheapest + kIntelAiPpReserve : kIntelAiPpReserve;
}

// One chosen operation: enough to build the command and explain it in an AiReason.
struct OpChoice {
    bool valid = false;
    uint32_t operation = 0;
    CountryId target;
    double value = 0.0;
    double pp_cost = 0.0;
    double network = 0.0;  // network strength in the target at planning time
};

}  // namespace

// ------------------------------------------------------------------ levels --

double network_strength(const Game& g, CountryId owner, CountryId target) {
    const Country* c = g.world.country(owner);
    if (c == nullptr) return 0.0;
    for (const SpyNetwork& n : c->networks) {
        if (n.target == target) return finite_or(n.strength, 0.0);
    }
    return 0.0;
}

double decryption_level(const Game& g, CountryId viewer, CountryId target) {
    const Country* c = g.world.country(viewer);
    if (c == nullptr) return 0.0;
    for (const CipherProgress& cp : c->ciphers) {
        if (cp.target == target) return clamp01(finite_or(cp.progress, 0.0));
    }
    return 0.0;
}

double counter_intel_level(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr) return 0.0;
    double sum = 0.0;
    for (uint32_t u : c->agency_upgrades) {  // ascending id
        const AgencyUpgradeDef* def = g.content.agency_upgrade(u);
        if (def != nullptr) sum += def->counter_intel;
    }
    return clamp(finite_or(sum, 0.0), 0.0, kIntelCounterIntelCap);
}

double intel_level(const Game& g, CountryId viewer, CountryId target) {
    if (!viewer.valid() || !target.valid() || viewer == target) return 0.0;
    const double strength = network_strength(g, viewer, target);
    const double decryption = decryption_level(g, viewer, target);
    const double raw = kIntelNetworkWeight * clamp01(strength / kIntelMaxNetworkStrength) +
                       kIntelCryptoWeight * decryption;
    return clamp(finite_or(raw, 0.0), 0.0, kIntelIntelCap);
}

double intel_attack_bonus(const Game& g, CountryId attacker, CountryId defender) {
    return kIntelAttackBonusPerLevel * intel_level(g, attacker, defender);
}

double decryption_planning_penalty(const Game& g, CountryId attacker, CountryId defender) {
    // The defender is the one who broke the attacker's ciphers.
    return kIntelPlanningPenaltyPerLevel * decryption_level(g, defender, attacker);
}

// ------------------------------------------------------------------ gates ---

bool intel_operation_available(const Game& g, CountryId country, CountryId target,
                               uint32_t operation) {
    const Country* c = g.world.country(country);
    const Country* t = g.world.country(target);
    if (c == nullptr || !c->alive || t == nullptr || !t->alive) return false;
    if (country == target) return false;
    const OperationDef* def = g.content.operation(operation);
    if (def == nullptr) return false;

    if (network_strength(g, country, target) + kEps < def->network_required) return false;

    if (def->kind == OperationKind::BuildNetwork &&
        network_strength(g, country, target) >= kIntelMaxNetworkStrength - kEps) {
        return false;  // nothing left to build
    }
    if (def->kind == OperationKind::CounterIntel && !has_enemy_network_at_home(g, country)) {
        return false;  // no foreign service to hunt
    }
    if (!def->available.is_null() &&
        !eval_trigger(const_country_scope(g, country), def->available)) {
        return false;
    }
    return true;
}

bool intel_upgrade_available(const Game& g, CountryId country, uint32_t upgrade) {
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const AgencyUpgradeDef* def = g.content.agency_upgrade(upgrade);
    if (def == nullptr) return false;
    if (holds_upgrade(*c, upgrade)) return false;
    if (g.world.date.year < def->year) return false;
    for (const std::string& req : def->requires_upgrades) {
        const uint32_t req_index = g.content.agency_upgrade_id(req);
        if (req_index == 0xFFFFFFFFu) return false;  // content references nothing
        if (!holds_upgrade(*c, req_index)) return false;
    }
    if (!def->available.is_null() &&
        !eval_trigger(const_country_scope(g, country), def->available)) {
        return false;
    }
    return true;
}

// --------------------------------------------------------------- commands ---

bool intel_start_operation(Game& g, CountryId country, CountryId target,
                           const std::string& operation_key) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const uint32_t op = g.content.operation_id(operation_key);
    if (op == 0xFFFFFFFFu) return false;
    const OperationDef* def = g.content.operation(op);
    if (def == nullptr) return false;
    // Validate before any mutation: a rejected start leaves zero state.
    if (!intel_operation_available(g, country, target, op)) return false;
    if (c->political_power < def->pp_cost) return false;
    for (const IntelOperation& running : c->operations) {
        if (running.target == target && running.operation == op) return false;
    }

    c->political_power -= def->pp_cost;
    // Starting an operation means working the target: the network presence and the
    // cipher effort exist from that moment on.
    ensure_network(*c, target);
    ensure_cipher(*c, target);
    IntelOperation io;
    io.target = target;
    io.operation = op;
    io.days_left = static_cast<double>(def->days);
    io.progress = 0.0;
    insert_operation(*c, io);
    g.log_event("intel", c->tag + " started '" + def->key + "' against " +
                              (g.world.country(target) ? g.world.country(target)->tag : "?"),
                country);
    return true;
}

bool intel_cancel_operation(Game& g, CountryId country, CountryId target,
                            const std::string& operation_key) {
    Country* c = g.world.country(country);
    if (c == nullptr) return false;
    const uint32_t op = g.content.operation_id(operation_key);
    if (op == 0xFFFFFFFFu) return false;
    for (size_t i = 0; i < c->operations.size(); ++i) {
        if (c->operations[i].target != target || c->operations[i].operation != op) continue;
        c->operations.erase(c->operations.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

bool intel_buy_upgrade(Game& g, CountryId country, const std::string& upgrade_key) {
    Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return false;
    const uint32_t index = g.content.agency_upgrade_id(upgrade_key);
    if (index == 0xFFFFFFFFu) return false;
    const AgencyUpgradeDef* def = g.content.agency_upgrade(index);
    if (def == nullptr) return false;
    if (!intel_upgrade_available(g, country, index)) return false;
    if (c->political_power < def->pp_cost) return false;

    c->political_power -= def->pp_cost;
    const auto it = std::lower_bound(c->agency_upgrades.begin(), c->agency_upgrades.end(), index);
    c->agency_upgrades.insert(it, index);
    g.log_event("intel", c->tag + " bought agency upgrade '" + def->key + "'", country);
    return true;
}

// ------------------------------------------------------------------ phase ---

void phase_intelligence(Game& g) {
    World& w = g.world;
    const double hour_fraction = 1.0 / static_cast<double>(TICKS_PER_DAY);

    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;

        const AgencySums sums = agency_sums(g, c);

        // An operation in flight always implies a network presence (it was created on
        // start), so converge any entry a loaded save may be missing.
        for (const IntelOperation& io : c.operations) ensure_network(c, io.target);

        // Drop state aimed at countries that are gone.
        auto target_alive = [&](CountryId t) {
            const Country* tc = w.country(t);
            return tc != nullptr && tc->alive;
        };
        c.networks.erase(std::remove_if(c.networks.begin(), c.networks.end(),
                                        [&](const SpyNetwork& n) { return !target_alive(n.target); }),
                         c.networks.end());
        c.ciphers.erase(std::remove_if(c.ciphers.begin(), c.ciphers.end(),
                                       [&](const CipherProgress& cp) { return !target_alive(cp.target); }),
                        c.ciphers.end());
        c.operations.erase(std::remove_if(c.operations.begin(), c.operations.end(),
                                          [&](const IntelOperation& io) { return !target_alive(io.target); }),
                           c.operations.end());

        const bool has_free_slot = static_cast<int>(c.operations.size()) < kIntelOperationSlots;

        // Networks: passive growth while the agency has room, and exposure decay.
        for (SpyNetwork& n : c.networks) {
            if (has_free_slot) {
                const double per_day = (kIntelBaseNetworkGrowth + sums.network_growth) *
                                       (1.0 - counter_intel_level(g, n.target)) *
                                       spying_factor(g, cid, n.target);
                n.strength = clamp(n.strength + finite_or(per_day, 0.0) * hour_fraction, 0.0,
                                   kIntelMaxNetworkStrength);
            }
            n.exposure = clamp01(n.exposure - kIntelExposureDecayPerDay * hour_fraction);
            if (!std::isfinite(n.strength)) n.strength = 0.0;
        }

        // Operations: pace, progress, completion effects and the burn roll. Erased
        // on completion either way, so the vector cannot grow without bound.
        for (size_t i = 0; i < c.operations.size();) {
            IntelOperation& io = c.operations[i];
            const OperationDef* def = g.content.operation(io.operation);
            if (def == nullptr) {  // content changed under a live save
                c.operations.erase(c.operations.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }

            const double strength = network_strength(g, cid, io.target);
            const double need = std::max(def->network_required, 1.0);
            const double pace = (1.0 + sums.operation_speed) * clamp01(strength / need);
            const double work = finite_or(pace, 0.0) * hour_fraction;

            io.progress += work;
            io.days_left -= work;
            if (SpyNetwork* n = find_network(c, io.target)) {
                n->exposure =
                    clamp01(n->exposure + kIntelExposurePerOperationDay * hour_fraction);
            }
            if (io.days_left > 0.0) {
                ++i;
                continue;
            }

            io.days_left = 0.0;

            // Exposure at completion is the pressure that decides the burn. Read it
            // before the effects run: they can move the network.
            double exposure = 0.0;
            if (const SpyNetwork* n = find_network(c, io.target)) exposure = n->exposure;

            apply_operation_effects(g, c, cid, io.target, *def);

            // One roll per completed operation, on the intel stream. Exposure scales
            // the data risk.
            Rng& rng = g.rng.get(RngStream::Intel);
            const bool burned = rng.chance(clamp01(def->risk * (1.0 + exposure)));
            if (burned) {
                burn_network(c, io.target);
                g.log_event("intel", c.tag + " network burned by '" + def->key + "'", cid);
            } else {
                g.log_event("intel", c.tag + " completed '" + def->key + "'", cid);
            }
            c.operations.erase(c.operations.begin() + static_cast<std::ptrdiff_t>(i));
        }

        // Decryption: every network yields cipher effort against its target.
        for (const SpyNetwork& n : c.networks) ensure_cipher(c, n.target);
        for (CipherProgress& cp : c.ciphers) {
            const double per_day = (kIntelBaseCrypto + sums.crypto_speed) *
                                   (1.0 - counter_intel_level(g, cp.target));
            cp.progress = clamp01(cp.progress + finite_or(per_day, 0.0) * hour_fraction);
        }
    });
}

// --------------------------------------------------------------------- AI ---

void ai_intelligence_layer(Game& g, Country& c) {
    const World& w = g.world;
    if (!c.alive) return;

    // 1. Agency upgrades: cheapest affordable upgrade whose prerequisites are met and
    //    which actually raises a service stat, leaving a political-power reserve.
    {
        uint32_t best = 0xFFFFFFFFu;
        double best_cost = 0.0;
        int eligible = 0;
        for (uint32_t u = 0; u < g.content.agency_upgrades.size(); ++u) {
            const AgencyUpgradeDef* def = g.content.agency_upgrade(u);
            if (def == nullptr) continue;
            if (!intel_upgrade_available(g, c.id, u)) continue;
            if (c.political_power - def->pp_cost < kIntelAiPpReserve) continue;
            const bool raises = def->network_growth > 0.0 || def->operation_speed > 0.0 ||
                                def->crypto_speed > 0.0 || def->counter_intel > 0.0;
            if (!raises) continue;
            ++eligible;
            if (best == 0xFFFFFFFFu || def->pp_cost < best_cost - kEps) {
                best = u;
                best_cost = def->pp_cost;
            }
        }
        if (best != 0xFFFFFFFFu) {
            Command cmd;
            cmd.type = CommandType::BuyAgencyUpgrade;
            cmd.country = c.id;
            cmd.text = g.content.agency_upgrade(best)->key;
            if (push_if_valid(g, std::move(cmd))) {
                AiReason r;
                r.what = "intel_upgrade:" + g.content.agency_upgrade(best)->key;
                r.score = best_cost;
                r.factors = {{"pp_cost", best_cost},
                             {"eligible", static_cast<double>(eligible)},
                             {"reserve", kIntelAiPpReserve}};
                record_reason(g, std::move(r));
            }
        } else {
            AiReason r;
            r.what = "intel_upgrade_none";
            r.score = 0.0;
            r.factors = {{"eligible", static_cast<double>(eligible)},
                         {"political_power", c.political_power},
                         {"reserve", kIntelAiPpReserve}};
            record_reason(g, std::move(r));
        }
    }

    // Operation slots: the AI concentrates its service on one operation at a time (see
    // kIntelAiMaxOperationsInFlight). Commands planned earlier in this call are not in
    // c.operations yet, so count them too.
    auto queued_starts = [&]() {
        int n = 0;
        for (const Command& qc : g.queue.pending) {
            if (qc.type == CommandType::StartIntelOperation && qc.country == c.id) ++n;
        }
        return n;
    };
    if (static_cast<int>(c.operations.size()) + queued_starts() >=
        kIntelAiMaxOperationsInFlight) {
        return;
    }

    // 2. Operations by value, not by price.
    //    (a) A non-network operation whose network prerequisite is met and which fits
    //        the PP budget with the buffer - preferred.
    //    (b) build_network, and only against a network still below
    //        build_target_strength() - never against an already strong one.
    //    Ties break by (target, operation key): targets ascend and operation indices
    //    are the content's declaration order, so on an equal value the first candidate
    //    kept is the lowest target id and then the lowest operation key.
    const double counter_pressure = enemy_network_strength_at_home(g, c.id);
    // Operations spend only the political power left after funding the next upgrade.
    const double pp_floor = upgrade_saving_floor(g, c);
    OpChoice best_other;
    OpChoice best_build;
    w.countries.for_each([&](CountryId target, const Country& tc) {
        if (target == c.id || !tc.alive) return;
        const double strength = network_strength(g, c.id, target);
        const double build_ceiling = build_target_strength(g, c.id, target, counter_pressure);
        for (uint32_t op = 0; op < g.content.operations.size(); ++op) {
            const OperationDef* def = g.content.operation(op);
            if (def == nullptr) continue;
            if (def->pp_cost > c.political_power - pp_floor + kEps) continue;
            if (already_working(g, c, target, op)) continue;
            if (!intel_operation_available(g, c.id, target, op)) continue;
            const double value = operation_value(*def, counter_pressure);
            if (def->kind == OperationKind::BuildNetwork) {
                if (strength >= build_ceiling - kEps) continue;  // already strong enough
                if (!best_build.valid || value > best_build.value + kEps) {
                    best_build = OpChoice{true, op, target, value, def->pp_cost, strength};
                }
                continue;
            }
            if (!best_other.valid || value > best_other.value + kEps) {
                best_other = OpChoice{true, op, target, value, def->pp_cost, strength};
            }
        }
    });

    const OpChoice& pick = best_other.valid ? best_other : best_build;
    if (!pick.valid) {
        AiReason r;
        r.what = "intel_operation_none";
        r.score = 0.0;
        r.factors = {{"political_power", c.political_power}, {"reserve", kIntelAiPpReserve}};
        record_reason(g, std::move(r));
        return;
    }

    const OperationDef* picked = g.content.operation(pick.operation);
    Command cmd;
    cmd.type = CommandType::StartIntelOperation;
    cmd.country = c.id;
    cmd.target_country = pick.target;
    cmd.text = picked->key;
    if (push_if_valid(g, std::move(cmd))) {
        AiReason r;
        r.what = "intel_operation:" + picked->key;
        r.score = pick.value;
        r.factors = {{"kind", static_cast<double>(picked->kind)},
                     {"value", pick.value},
                     {"network_strength", pick.network},
                     {"network_required", picked->network_required},
                     {"pp_cost", pick.pp_cost},
                     {"reserve", kIntelAiPpReserve},
                     {"saving_floor", pp_floor},
                     {"political_power", c.political_power}};
        record_reason(g, std::move(r));
    }
}

}  // namespace hoi