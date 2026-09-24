// Intelligence (INT-001): network growth, counter-intelligence, operations,
// decryption, the two combat hooks and the AI agency policy.
//
// Every test builds a hermetic MiniWorld and asserts numbers, not just progress:
// the growth rates, the decryption rate, the exact operation completion day and the
// two burn branches are all checked against their documented constants.

#include <cmath>
#include <string>
#include <vector>

#include "data/content.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "sim/intel.h"
#include "sim/politics.h"
#include "sim/world.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using hoi_test::MiniWorld;
using hoi_test::add_division;
using hoi_test::make_mini_world;

// ------------------------------------------------------------------ content --

uint32_t add_operation(Content& c, const std::string& key, OperationKind kind, int days,
                       double pp_cost, double network_required, double risk) {
    OperationDef d;
    d.index = static_cast<uint32_t>(c.operations.size());
    d.key = key;
    d.name = key;
    d.kind = kind;
    d.days = days;
    d.pp_cost = pp_cost;
    d.network_required = network_required;
    d.risk = risk;
    d.effect_days = 90;
    c.operation_index[key] = d.index;
    c.operations.push_back(std::move(d));
    return static_cast<uint32_t>(c.operations.size() - 1);
}

uint32_t add_upgrade(Content& c, const std::string& key, int year, double pp_cost,
                     double network_growth, double operation_speed, double crypto_speed,
                     double counter_intel, std::vector<std::string> requires_upgrades = {}) {
    AgencyUpgradeDef d;
    d.index = static_cast<uint32_t>(c.agency_upgrades.size());
    d.key = key;
    d.name = key;
    d.year = year;
    d.pp_cost = pp_cost;
    d.network_growth = network_growth;
    d.operation_speed = operation_speed;
    d.crypto_speed = crypto_speed;
    d.counter_intel = counter_intel;
    d.requires_upgrades = std::move(requires_upgrades);
    c.agency_upgrade_index[key] = d.index;
    c.agency_upgrades.push_back(std::move(d));
    return static_cast<uint32_t>(c.agency_upgrades.size() - 1);
}

// ------------------------------------------------------------------- state ---

void add_network(Game& g, CountryId owner, CountryId target, double strength,
                 double exposure = 0.0) {
    SpyNetwork n;
    n.target = target;
    n.strength = strength;
    n.exposure = exposure;
    g.world.countries[owner].networks.push_back(n);
}

void add_cipher(Game& g, CountryId viewer, CountryId target, double progress) {
    CipherProgress cp;
    cp.target = target;
    cp.progress = progress;
    g.world.countries[viewer].ciphers.push_back(cp);
}

void run_intel(Game& g, int hours) {
    for (int i = 0; i < hours; ++i) phase_intelligence(g);
}

// Runs the intel phase until `operations` is empty (bounded). Returns the number of
// hours it took, or -1 when the operation never completed.
int run_until_operation_done(Game& g, CountryId country, int limit_hours = 2000) {
    for (int i = 0; i < limit_hours; ++i) {
        phase_intelligence(g);
        if (g.world.countries[country].operations.empty()) return i + 1;
    }
    return -1;
}

double net(const Game& g, CountryId owner, CountryId target) {
    return g.world.countries[owner].networks.empty()
               ? 0.0
               : g.world.countries[owner].networks[0].strength;
}

// ------------------------------------------------------------------ battle ---

struct BattleSample {
    bool alive = false;
    double defender_org = 0.0;
    double attacker_org = 0.0;
};

// Two identical battles (a attacks b over the p2|p3 frontier), differing only in the
// intel state of the two countries.
BattleSample run_battle(bool intel_attack_bonus, bool planning_penalty, bool set_planning,
                        int ticks) {
    MiniWorld m = make_mini_world(true);
    Game& g = m.game;
    const TemplateId tmpl_b = g.content.template_id("infantry_template_b");
    const DivisionId def = add_division(g, m.b, m.provinces[3], tmpl_b);
    const DivisionId att = add_division(g, m.a, m.provinces[2], m.tmpl);
    g.world.divisions[att].order = OrderKind::Offensive;
    g.world.divisions[att].order_target = m.provinces[3];
    if (intel_attack_bonus) add_network(g, m.a, m.b, 100.0);
    if (planning_penalty) add_cipher(g, m.b, m.a, 1.0);

    for (int i = 0; i < ticks; ++i) {
        if (set_planning) g.world.divisions[att].planning = 1.0;
        phase_combat(g);
    }

    BattleSample s;
    const Division* d = g.world.division(def);
    const Division* a = g.world.division(att);
    s.alive = g.world.battles.size() > 0;
    s.defender_org = d ? d->organization : 0.0;
    s.attacker_org = a ? a->organization : 0.0;
    return s;
}

}  // namespace

// -----------------------------------------------------------------------------

// Network growth: peace halves it (kIntelPeaceSpyFactor), war does not, and the
// target's counter-intelligence cuts it. All three are asserted as numbers.
HOI_TEST(intel_network_growth_peace_war_and_counter_intel) {
    MiniWorld m = make_mini_world();  // at peace
    Game& g = m.game;
    add_network(g, m.a, m.b, 0.0);

    // peace, no upgrades: 0.40 * 0.5 = 0.20 strength/day
    run_intel(g, TICKS_PER_DAY);
    CHECK_NEAR(net(g, m.a, m.b), 0.20, 1e-9);
    CHECK_NEAR(counter_intel_level(g, m.b), 0.0, 1e-12);

    // counter-intel 0.30: 0.40 * 0.5 * (1 - 0.30) = 0.14/day
    const uint32_t ci = add_upgrade(g.content, "ci1", 1936, 0.0, 0.0, 0.0, 0.0, 0.30);
    g.world.countries[m.b].agency_upgrades.push_back(ci);
    CHECK_NEAR(counter_intel_level(g, m.b), 0.30, 1e-12);
    g.world.countries[m.a].networks[0].strength = 0.0;
    run_intel(g, TICKS_PER_DAY);
    CHECK_NEAR(net(g, m.a, m.b), 0.14, 1e-9);

    // The counter-intel sum is capped at kIntelCounterIntelCap (0.80).
    const uint32_t ci2 = add_upgrade(g.content, "ci2", 1936, 0.0, 0.0, 0.0, 0.0, 0.70);
    g.world.countries[m.b].agency_upgrades.push_back(ci2);
    CHECK_NEAR(counter_intel_level(g, m.b), 0.80, 1e-12);

    // at war the peace factor does not apply: 0.40/day
    MiniWorld war = make_mini_world(true);
    add_network(war.game, war.a, war.b, 0.0);
    run_intel(war.game, TICKS_PER_DAY);
    CHECK_NEAR(net(war.game, war.a, war.b), 0.40, 1e-9);

    // A full agency (kIntelOperationSlots operations) stops growing networks while an
    // operation with an unmet network prerequisite stalls at zero pace.
    MiniWorld full = make_mini_world();
    add_network(full.game, full.a, full.b, 0.0);
    const uint32_t slow =
        add_operation(full.game.content, "slow", OperationKind::BuildNetwork, 9999, 0.0, 50.0, 0.0);
    for (int i = 0; i < 3; ++i) {
        IntelOperation io;
        io.target = full.b;
        io.operation = slow;
        io.days_left = 9999.0;
        full.game.world.countries[full.a].operations.push_back(io);
    }
    run_intel(full.game, TICKS_PER_DAY);
    CHECK_NEAR(net(full.game, full.a, full.b), 0.0, 1e-12);
    CHECK_EQ(full.game.world.countries[full.a].operations.size(), 3u);
}

// A start is rejected before any mutation when the network is too weak or the
// political power is short: validation and application both leave the world alone.
HOI_TEST(intel_start_requires_network_and_political_power) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_operation(g.content, "steal", OperationKind::StealTech, 10, 20.0, 0.0, 0.0);
    add_operation(g.content, "slow", OperationKind::BuildNetwork, 10, 5.0, 50.0, 0.0);

    Command cmd;
    cmd.type = CommandType::StartIntelOperation;
    cmd.country = m.a;
    cmd.target_country = m.b;
    cmd.text = "slow";
    CHECK_EQ(validate_command(g, cmd), CommandResult::PrerequisitesMissing);
    const uint64_t before = world_hash(g);
    CHECK_EQ(apply_command(g, cmd), CommandResult::PrerequisitesMissing);
    CHECK_EQ(world_hash(g), before);
    CHECK(g.world.countries[m.a].operations.empty());
    CHECK(!intel_start_operation(g, m.a, m.b, "slow"));

    // Short of political power: rejected for resources, state untouched.
    g.world.countries[m.a].political_power = 5.0;
    cmd.text = "steal";
    const uint64_t before2 = world_hash(g);
    CHECK_EQ(validate_command(g, cmd), CommandResult::InsufficientResources);
    CHECK(apply_command(g, cmd) != CommandResult::Applied);
    CHECK_EQ(world_hash(g), before2);
    CHECK_NEAR(g.world.countries[m.a].political_power, 5.0, 1e-12);
    CHECK(g.world.countries[m.a].operations.empty());
    CHECK(!intel_start_operation(g, m.a, m.b, "steal"));
}

// An operation runs one work-day per day at pace 1: a 10-day operation completes on
// day 10, not before, and its research_days land on the active slot.
HOI_TEST(intel_operation_completes_in_expected_days) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t steal =
        add_operation(g.content, "steal", OperationKind::StealTech, 10, 10.0, 0.0, 0.0);
    g.content.operations[steal].research_days = 30.0;
    add_network(g, m.a, m.b, 100.0);

    ResearchSlot& slot = g.world.countries[m.a].research.slots[0];
    slot.tech = TechId{0};
    slot.progress = 0.0;
    slot.active = true;

    const double pp0 = g.world.countries[m.a].political_power;
    CHECK(intel_start_operation(g, m.a, m.b, "steal"));
    CHECK_NEAR(g.world.countries[m.a].political_power, pp0 - 10.0, 1e-9);
    CHECK_EQ(g.world.countries[m.a].operations.size(), 1u);
    CHECK_NEAR(g.world.countries[m.a].operations[0].days_left, 10.0, 1e-9);

    run_intel(g, 10 * TICKS_PER_DAY - 1);
    CHECK_EQ(g.world.countries[m.a].operations.size(), 1u);  // day 10 not finished
    run_intel(g, 2);
    CHECK_EQ(g.world.countries[m.a].operations.size(), 0u);
    CHECK_NEAR(slot.progress, 30.0, 1e-9);

    // Cancelling an in-flight operation removes only that operation.
    CHECK(intel_start_operation(g, m.a, m.b, "steal"));
    CHECK_EQ(g.world.countries[m.a].operations.size(), 1u);
    CHECK(intel_cancel_operation(g, m.a, m.b, "steal"));
    CHECK(g.world.countries[m.a].operations.empty());
    CHECK(!intel_cancel_operation(g, m.a, m.b, "steal"));
}

// Completed effects hit the target through public accessors: the timed modifiers on
// output/stability/ideology and the optional script effect.
HOI_TEST(intel_operation_applies_target_effects) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t sab =
        add_operation(g.content, "sabotage", OperationKind::SabotageIndustry, 10, 10.0, 0.0, 0.0);
    g.content.operations[sab].output_penalty = 0.25;
    const uint32_t dest =
        add_operation(g.content, "destabilise", OperationKind::Destabilise, 10, 10.0, 0.0, 0.0);
    g.content.operations[dest].stability_delta = -0.20;
    g.content.operations[dest].ideology_shift = 0.10;
    const uint32_t scripted =
        add_operation(g.content, "scripted", OperationKind::SupportIdeology, 10, 10.0, 0.0, 0.0);
    g.content.operations[scripted].effect = Json::parse(R"({"add_stability": -0.05})");

    add_network(g, m.a, m.b, 100.0);

    CHECK(intel_start_operation(g, m.a, m.b, "sabotage"));
    // A 10-day operation at pace 1 finishes on day 10 (within one tick of
    // floating-point accumulation).
    const int hours = run_until_operation_done(g, m.a);
    CHECK(hours >= 10 * TICKS_PER_DAY);
    CHECK(hours <= 10 * TICKS_PER_DAY + 1);
    const Country& b = g.world.countries[m.b];
    CHECK_NEAR(b.total_modifiers().get(ModifierKind::FactoryOutput), -0.25, 1e-9);
    CHECK(b.timed_modifiers.size() == 1u);
    CHECK_EQ(b.timed_modifiers[0].source, std::string("intel:sabotage"));
    CHECK_EQ(b.timed_modifiers[0].days_left, 90);

    CHECK(intel_start_operation(g, m.a, m.b, "scripted"));
    run_until_operation_done(g, m.a);
    CHECK_NEAR(g.world.countries[m.b].stability, 0.45, 1e-9);  // script effect applied

    CHECK(intel_start_operation(g, m.a, m.b, "destabilise"));
    run_until_operation_done(g, m.a);
    CHECK_NEAR(g.world.countries[m.b].total_modifiers().get(ModifierKind::Stability), -0.20,
               1e-9);
    CHECK_NEAR(g.world.countries[m.b].total_modifiers().get(ModifierKind::WarSupport), 0.10,
               1e-9);

    // The stability modifier is real: politics drifts the target down toward 0.30.
    const double before = g.world.countries[m.b].stability;
    for (int i = 0; i < 10 * TICKS_PER_DAY; ++i) phase_politics(g);
    CHECK_LT(g.world.countries[m.b].stability, before);
    CHECK_GT(g.world.countries[m.b].stability, 0.29);
}

// Both burn branches, with the network strength and exposure asserted: risk 1 always
// burns (strength halved, exposure up), risk 0 never does.
HOI_TEST(intel_operation_burn_both_branches) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_operation(g.content, "risky", OperationKind::Destabilise, 5, 0.0, 0.0, 1.0);
    add_operation(g.content, "safe", OperationKind::Destabilise, 5, 0.0, 0.0, 0.0);
    add_network(g, m.a, m.b, 100.0);

    CHECK(intel_start_operation(g, m.a, m.b, "risky"));
    CHECK_GT(run_until_operation_done(g, m.a), 0);
    CHECK(g.world.countries[m.a].operations.empty());
    CHECK_NEAR(net(g, m.a, m.b), 50.0, 1e-9);  // kIntelBurnStrengthRetained
    CHECK_GT(g.world.countries[m.a].networks[0].exposure, 0.45);

    g.world.countries[m.a].networks[0].strength = 100.0;
    g.world.countries[m.a].networks[0].exposure = 0.0;
    CHECK(intel_start_operation(g, m.a, m.b, "safe"));
    CHECK_GT(run_until_operation_done(g, m.a), 0);
    CHECK(g.world.countries[m.a].operations.empty());
    CHECK_NEAR(net(g, m.a, m.b), 100.0, 1e-9);  // survived at full strength
    CHECK_LT(g.world.countries[m.a].networks[0].exposure, 0.05);

    // Seed-controlled mid-risk roll: both outcomes occur across seeds and the same
    // seed always produces the same branch.
    auto burn_outcome = [](uint64_t seed, double risk) -> bool {
        MiniWorld w = make_mini_world();
        Game& gw = w.game;
        add_operation(gw.content, "op", OperationKind::Destabilise, 3, 0.0, 0.0, risk);
        add_network(gw, w.a, w.b, 100.0);
        gw.world.world_seed = seed;
        gw.rng.seed(seed);
        intel_start_operation(gw, w.a, w.b, "op");
        for (int i = 0; i < 500 && !gw.world.countries[w.a].operations.empty(); ++i) {
            phase_intelligence(gw);
        }
        return net(gw, w.a, w.b) < 99.0;
    };
    int burned = 0;
    int survived = 0;
    for (uint64_t seed = 1; seed <= 16; ++seed) {
        (burn_outcome(seed, 0.5) ? burned : survived) += 1;
    }
    CHECK_GT(burned, 0);
    CHECK_GT(survived, 0);
    CHECK_EQ(burn_outcome(7, 0.5), burn_outcome(7, 0.5));
}

// Decryption accrues at kIntelBaseCrypto/day, is cut by target counter-intel, and
// raises intel_level through kIntelCryptoWeight.
HOI_TEST(intel_decryption_accrues_and_raises_intel_level) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_network(g, m.a, m.b, 100.0);
    CHECK_NEAR(intel_level(g, m.a, m.b), 0.75, 1e-9);
    CHECK_NEAR(decryption_level(g, m.a, m.b), 0.0, 1e-12);

    run_intel(g, TICKS_PER_DAY);
    CHECK_NEAR(decryption_level(g, m.a, m.b), 0.15, 1e-9);
    CHECK_NEAR(intel_level(g, m.a, m.b), 0.75 + 0.25 * 0.15, 1e-9);

    // Counter-intel 0.30 cuts crypto to 0.15 * 0.70 = 0.105/day.
    const uint32_t ci = add_upgrade(g.content, "ci", 1936, 0.0, 0.0, 0.0, 0.0, 0.30);
    g.world.countries[m.b].agency_upgrades.push_back(ci);
    g.world.countries[m.a].ciphers[0].progress = 0.0;
    run_intel(g, TICKS_PER_DAY);
    CHECK_NEAR(decryption_level(g, m.a, m.b), 0.105, 1e-9);

    // Knowledge is capped: kIntelIntelCap < 1.
    CHECK_LT(intel_level(g, m.a, m.b), 1.0);
}

// The intel attack bonus changes a battle's outcome: the informed attacker strips
// more organisation from the same defender.
HOI_TEST(intel_attack_bonus_changes_battle_outcome) {
    const BattleSample plain = run_battle(false, false, false, 6);
    const BattleSample informed = run_battle(true, false, false, 6);
    CHECK(plain.alive);
    CHECK(informed.alive);

    MiniWorld probe = make_mini_world(true);
    add_network(probe.game, probe.a, probe.b, 100.0);
    CHECK_NEAR(intel_attack_bonus(probe.game, probe.a, probe.b), 0.075, 1e-9);

    CHECK_LT(informed.defender_org, plain.defender_org);
}

// The planning penalty changes a battle's outcome too: broken ciphers cost the
// attacker its prepared surprise, so the defender keeps more organisation.
HOI_TEST(intel_planning_penalty_changes_battle_outcome) {
    const BattleSample plain = run_battle(false, false, true, 6);
    const BattleSample broken = run_battle(false, true, true, 6);
    CHECK(plain.alive);
    CHECK(broken.alive);

    MiniWorld probe = make_mini_world(true);
    add_cipher(probe.game, probe.b, probe.a, 1.0);
    CHECK_NEAR(decryption_planning_penalty(probe.game, probe.a, probe.b), 0.5, 1e-9);

    CHECK_GT(broken.defender_org, plain.defender_org);
}

// Agency upgrades: year gate, prerequisite gate and the political-power charge.
HOI_TEST(intel_upgrade_availability_and_command) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t basic = add_upgrade(g.content, "basic", 1936, 40.0, 0.10, 0.0, 0.05, 0.0);
    const uint32_t advanced =
        add_upgrade(g.content, "advanced", 1936, 30.0, 0.0, 0.25, 0.10, 0.0, {"basic"});
    add_upgrade(g.content, "future", 1945, 10.0, 0.10, 0.0, 0.0, 0.0);

    CHECK(intel_upgrade_available(g, m.a, basic));
    CHECK(!intel_upgrade_available(g, m.a, advanced));           // prerequisite missing
    CHECK(!intel_upgrade_available(g, m.a, 2u));                 // year 1945

    const double pp0 = g.world.countries[m.a].political_power;
    CHECK(intel_buy_upgrade(g, m.a, "basic"));
    CHECK_NEAR(g.world.countries[m.a].political_power, pp0 - 40.0, 1e-9);
    CHECK(intel_upgrade_available(g, m.a, advanced));
    CHECK(!intel_buy_upgrade(g, m.a, "basic"));  // already held: rejected

    // The command path charges the same cost and records Applied.
    Command cmd;
    cmd.type = CommandType::BuyAgencyUpgrade;
    cmd.country = m.a;
    cmd.text = "advanced";
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);
    CHECK_EQ(apply_command(g, cmd), CommandResult::Applied);
    CHECK(g.world.countries[m.a].agency_upgrades.size() == 2u);
    CHECK(!intel_upgrade_available(g, m.a, advanced));
}

// The AI agency policy acts only through the command queue: it buys the cheapest
// eligible upgrade and starts an operation, and records reasons for both.
HOI_TEST(intel_ai_layer_plans_upgrade_and_operation) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_upgrade(g.content, "sigint", 1936, 50.0, 0.10, 0.0, 0.05, 0.0);
    add_operation(g.content, "build", OperationKind::BuildNetwork, 20, 10.0, 0.0, 0.0);
    add_operation(g.content, "steal", OperationKind::StealTech, 10, 20.0, 0.0, 0.0);
    // The AI needs a threat to work against.
    const TemplateId tmpl_b = g.content.template_id("infantry_template_b");
    add_division(g, m.b, m.provinces[3], tmpl_b);
    g.set_ai(m.a, true);

    const size_t queue0 = g.queue.pending.size();
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK_GT(g.queue.pending.size(), queue0);
    CHECK(!g.ai.layer(AiLayer::Politics).last_reasons.empty());

    const size_t reasons0 = g.ai.layer(AiLayer::Politics).last_reasons.size();
    CHECK_GT(reasons0, 0u);

    // Applying the queued commands mutates the world through the normal path.
    phase_commands(g);
    CHECK_EQ(g.world.countries[m.a].agency_upgrades.size(), 1u);
    CHECK_EQ(g.world.countries[m.a].agency_upgrades[0], 0u);
    CHECK_GT(g.world.countries[m.a].operations.size(), 0u);
    CHECK(g.world.countries[m.a].operations.size() <= 2u);
}

// Ten years of the intel phase in a loop: everything stays finite and inside its
// bounds, and no vector grows without bound.
HOI_TEST(intel_state_survives_ten_years_within_bounds) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_network(g, m.a, m.b, 10.0);
    add_network(g, m.b, m.a, 10.0);
    add_cipher(g, m.a, m.b, 0.0);
    // A stalled operation (network prerequisite never met) keeps the vector occupied.
    const uint32_t slow =
        add_operation(g.content, "slow", OperationKind::BuildNetwork, 100, 0.0, 90.0, 0.0);
    IntelOperation io;
    io.target = m.b;
    io.operation = slow;
    io.days_left = 100.0;
    g.world.countries[m.a].operations.push_back(io);

    const int days = 3650;
    for (int i = 0; i < days * TICKS_PER_DAY; ++i) phase_intelligence(g);

    const Country& a = g.world.countries[m.a];
    const Country& b = g.world.countries[m.b];
    CHECK(a.networks.size() <= 1u);
    CHECK(a.ciphers.size() <= 1u);
    CHECK(a.operations.size() <= 1u);
    for (const SpyNetwork& n : a.networks) {
        CHECK(std::isfinite(n.strength));
        CHECK(n.strength >= 0.0 && n.strength <= 100.0);
        CHECK(std::isfinite(n.exposure));
        CHECK(n.exposure >= 0.0 && n.exposure <= 1.0);
    }
    for (const CipherProgress& cp : a.ciphers) {
        CHECK(std::isfinite(cp.progress));
        CHECK(cp.progress >= 0.0 && cp.progress <= 1.0);
    }
    CHECK(std::isfinite(intel_level(g, m.a, m.b)));
    CHECK(intel_level(g, m.a, m.b) <= 0.85);
    CHECK(std::isfinite(counter_intel_level(g, m.a)));
    CHECK(std::isfinite(b.networks.empty() ? 0.0 : b.networks[0].strength));
}

// The agency policy is pinned: an upgrade is bought once PP crosses cost+reserve,
// a non-BuildNetwork operation is preferred once its network requirement is met, and
// build_network is not started against a network that is already strong enough.
HOI_TEST(intel_ai_buys_upgrade_at_cost_plus_reserve) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    add_upgrade(g.content, "academy", 1936, 30.0, 0.20, 0.0, 0.0, 0.0);

    auto buy_queued = [&]() {
        for (const Command& qc : g.queue.pending) {
            if (qc.type == CommandType::BuyAgencyUpgrade) return true;
        }
        return false;
    };

    // reserve (25) short by one: nothing planned.
    g.world.countries[m.a].political_power = 54.0;
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK(!buy_queued());
    CHECK(g.world.countries[m.a].agency_upgrades.empty());
    CHECK(!g.ai.layer(AiLayer::Politics).last_reasons.empty());

    // crossing cost + reserve buys the cheapest eligible upgrade.
    g.queue.clear();
    g.world.countries[m.a].political_power = 55.0;
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK(buy_queued());
    phase_commands(g);
    CHECK_EQ(g.world.countries[m.a].agency_upgrades.size(), 1u);
    CHECK_EQ(g.world.countries[m.a].agency_upgrades[0], 0u);
    CHECK_NEAR(g.world.countries[m.a].political_power, 25.0, 1e-9);
}

HOI_TEST(intel_ai_prefers_non_network_operation_when_requirement_met) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t build = add_operation(g.content, "build", OperationKind::BuildNetwork, 30, 25.0,
                                         0.0, 0.0);
    g.content.operations[build].network_gain = 12.0;
    const uint32_t steal =
        add_operation(g.content, "steal", OperationKind::StealTech, 60, 70.0, 25.0, 0.0);
    g.content.operations[steal].research_days = 120.0;
    add_network(g, m.a, m.b, 10.0);  // below steal's requirement
    g.world.countries[m.a].political_power = 500.0;

    auto planned_text = [&]() {
        for (const Command& qc : g.queue.pending) {
            if (qc.type == CommandType::StartIntelOperation) return qc.text;
        }
        return std::string();
    };

    // Network weak: steal is gated, so the policy builds the network.
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK_EQ(planned_text(), std::string("build"));

    // Requirement met: the valuable non-network operation wins over more network work.
    g.queue.clear();
    g.world.countries[m.a].networks[0].strength = 30.0;
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK_EQ(planned_text(), std::string("steal"));

    // The reason carries the factors that explain the choice.
    bool found = false;
    for (const AiReason& r : g.ai.layer(AiLayer::Politics).last_reasons) {
        if (r.what != "intel_operation:steal") continue;
        found = true;
        bool kind = false, value = false, strength = false, required = false, reserve = false;
        for (const auto& f : r.factors) {
            if (f.first == "kind") kind = true;
            if (f.first == "value") value = true;
            if (f.first == "network_strength") strength = true;
            if (f.first == "network_required") required = true;
            if (f.first == "reserve") reserve = true;
        }
        CHECK(kind && value && strength && required && reserve);
        CHECK_NEAR(r.score, 120.0, 1e-9);  // research-days stolen
    }
    CHECK(found);
}

HOI_TEST(intel_ai_does_not_build_network_against_strong_network) {
    MiniWorld m = make_mini_world();
    Game& g = m.game;
    const uint32_t build = add_operation(g.content, "build", OperationKind::BuildNetwork, 30, 25.0,
                                         0.0, 0.0);
    g.content.operations[build].network_gain = 12.0;
    add_network(g, m.a, m.b, 40.0);
    g.world.countries[m.a].political_power = 500.0;

    auto start_queued = [&]() {
        for (const Command& qc : g.queue.pending) {
            if (qc.type == CommandType::StartIntelOperation) return true;
        }
        return false;
    };

    // Below the standing target strength (60): keep building.
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK(start_queued());

    // At or above it: the network is strong enough, nothing is planned.
    g.queue.clear();
    g.world.countries[m.a].networks[0].strength = 60.0;
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK(!start_queued());
    g.world.countries[m.a].networks[0].strength = 85.0;
    ai_intelligence_layer(g, g.world.countries[m.a]);
    CHECK(!start_queued());
}

// Two identical runs of the intel phase hash identically, including the RNG state.
HOI_TEST(intel_two_identical_runs_hash_identically) {
    auto run = [](uint64_t* out_hash, uint64_t* out_rng, double* out_intel) {
        MiniWorld m = make_mini_world(true);
        Game& g = m.game;
        g.world.world_seed = 4242;
        g.rng.seed(g.world.world_seed);
        add_operation(g.content, "steal", OperationKind::StealTech, 10, 5.0, 0.0, 0.5);
        add_operation(g.content, "safe", OperationKind::Destabilise, 5, 5.0, 0.0, 0.0);
        add_network(g, m.a, m.b, 20.0);
        intel_start_operation(g, m.a, m.b, "steal");
        for (int i = 0; i < 40 * TICKS_PER_DAY; ++i) {
            phase_intelligence(g);
            if (g.world.countries[m.a].operations.empty() && (i % 24) == 0) {
                intel_start_operation(g, m.a, m.b, "safe");
            }
        }
        *out_hash = world_hash(g);
        *out_rng = g.rng.state_hash();
        *out_intel = intel_level(g, m.a, m.b);
    };

    uint64_t h1 = 0, h2 = 0, r1 = 0, r2 = 0;
    double i1 = 0.0, i2 = 0.0;
    run(&h1, &r1, &i1);
    run(&h2, &r2, &i2);
    CHECK_EQ(h1, h2);
    CHECK_EQ(r1, r2);
    CHECK_NEAR(i1, i2, 0.0);
}