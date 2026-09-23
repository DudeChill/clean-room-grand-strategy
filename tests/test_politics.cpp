// Politics, diplomacy, occupation and weather tests: hand-built worlds only.

#include <algorithm>
#include <string>
#include <vector>

#include "game/game.h"
#include "sim/diplomacy.h"
#include "sim/politics.h"
#include "test.h"

namespace {

using namespace hoi;

StateId make_state(World& w, const std::string& name, RegionId region) {
    StateId id = w.states.create();
    State* s = w.states.try_get(id);
    s->id = id;
    s->name = name;
    s->region = region;
    return id;
}

ProvinceId make_province(World& w, const std::string& name, StateId state, RegionId region) {
    ProvinceId id = w.provinces.create();
    Province* p = w.provinces.try_get(id);
    p->id = id;
    p->name = name;
    p->state = state;
    p->region = region;
    p->terrain = Terrain::Plains;
    p->infrastructure = 0;
    w.states.try_get(state)->provinces.push_back(id);
    return id;
}

CountryId make_country(World& w, const std::string& tag) {
    CountryId id = w.countries.create();
    Country* c = w.countries.try_get(id);
    c->id = id;
    c->tag = tag;
    c->name = tag;
    return id;
}

void give_state(World& w, StateId state, CountryId owner) {
    State* s = w.states.try_get(state);
    s->owner = owner;
    s->controller = owner;
    s->core_owners.push_back(owner);
    for (ProvinceId pid : s->provinces) {
        Province* p = w.provinces.try_get(pid);
        p->owner = owner;
        p->controller = owner;
    }
}

void link(World& w, ProvinceId a, ProvinceId b) {
    w.provinces.try_get(a)->adj.push_back(b);
    w.provinces.try_get(b)->adj.push_back(a);
}

// One province in its own state, owned and controlled by `owner`, capable of
// hosting a capital.
ProvinceId make_capital(World& w, StateId state, RegionId region, CountryId owner,
                        const std::string& name) {
    const ProvinceId pid = make_province(w, name, state, region);
    give_state(w, state, owner);
    w.provinces.try_get(pid)->is_capital = true;
    w.countries.try_get(owner)->capital = state;
    return pid;
}

ProvinceId make_home_state(World& w, RegionId region, CountryId owner, double population,
                           const std::string& name) {
    const StateId state = make_state(w, name, region);
    const ProvinceId pid = make_capital(w, state, region, owner, name + "_capital");
    w.provinces.try_get(pid)->population = population;
    return pid;
}

}  // namespace

// Manpower accrues daily from the population of controlled states, scaled by the
// manpower and recruitable-population modifiers.
HOI_TEST(politics_manpower_growth_from_controlled_states) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    make_home_state(g.world, region, alpha, 365000.0, "alpha_home");
    make_home_state(g.world, region, alpha, 365000.0, "alpha_second");
    make_home_state(g.world, region, beta, 365000.0, "beta_home");

    // Pop 2 * 365000 * recruitable 0.025 / 365 days = 50 per day.
    CHECK_NEAR(daily_manpower_gain(g, alpha), 50.0, 1e-9);
    CHECK_NEAR(daily_manpower_gain(g, beta), 25.0, 1e-9);

    g.world.countries.try_get(alpha)->base_modifiers.set(ModifierKind::ManpowerGrowth, 0.10);
    CHECK_NEAR(daily_manpower_gain(g, alpha), 55.0, 1e-9);
    g.world.countries.try_get(alpha)->base_modifiers.set(ModifierKind::ManpowerGrowth, 0.0);
    g.world.countries.try_get(alpha)->base_modifiers.set(ModifierKind::RecruitablePopulation, 1.0);
    CHECK_NEAR(daily_manpower_gain(g, alpha), 100.0, 1e-9);

    // A day of politics credits the pool hourly and grows the state pools too.
    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_politics(g);
    const Country* a = g.world.countries.try_get(alpha);
    CHECK_NEAR(a->manpower, 100.0, 1e-9);
    double pool = 0.0;
    g.world.states.for_each([&](StateId, const State& s) {
        if (s.controller == alpha) pool += s.manpower_pool;
    });
    CHECK_NEAR(pool, 100.0, 1e-9);

    // Loss of control stops the growth: an enemy-held state no longer contributes.
    g.world.states.for_each([&](StateId, State& s) {
        if (s.name == "alpha_second") s.controller = beta;
    });
    CHECK_NEAR(daily_manpower_gain(g, alpha), 50.0, 1e-9);
}

HOI_TEST(politics_political_power_accumulates_daily) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    make_home_state(g.world, region, alpha, 0.0, "alpha_home");

    CHECK_NEAR(daily_political_power(g, alpha), 2.0, 1e-12);
    g.world.countries.try_get(alpha)->base_modifiers.set(ModifierKind::PoliticalPowerGain, 0.5);
    CHECK_NEAR(daily_political_power(g, alpha), 3.0, 1e-12);

    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_politics(g);
    CHECK_NEAR(g.world.countries.try_get(alpha)->political_power, 3.0, 1e-9);
    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_politics(g);
    CHECK_NEAR(g.world.countries.try_get(alpha)->political_power, 6.0, 1e-9);
}

// A law change sets the level and refreshes the law modifiers; the political power
// cost is the caller's business (command validation), so none is charged here.
HOI_TEST(politics_law_change_applies_modifiers_without_charging_pp) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    make_home_state(g.world, region, alpha, 0.0, "alpha_home");

    LawDef conscription;
    conscription.key = "conscription_1";
    conscription.kind = 0;
    conscription.level = 1;
    conscription.cost = 100.0;
    conscription.modifiers.set(ModifierKind::Stability, 0.10);
    conscription.modifiers.set(ModifierKind::RecruitablePopulation, 0.50);
    g.content.laws.push_back(conscription);
    LawDef conscription_deep;
    conscription_deep.key = "conscription_2";
    conscription_deep.kind = 0;
    conscription_deep.level = 2;
    conscription_deep.modifiers.set(ModifierKind::Stability, 0.20);
    g.content.laws.push_back(conscription_deep);
    // A different law kind must not leak into the kind-0 slots.
    LawDef economy;
    economy.key = "economy_1";
    economy.kind = 1;
    economy.level = 1;
    economy.modifiers.set(ModifierKind::FactoryOutput, 0.15);
    g.content.laws.push_back(economy);

    Country* a = g.world.countries.try_get(alpha);
    a->political_power = 250.0;
    apply_law_change(g, *a, 0, 1);
    CHECK_EQ(a->law_levels[0], static_cast<uint8_t>(1));
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::Stability), 0.10, 1e-12);
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::RecruitablePopulation), 0.50, 1e-12);
    CHECK_NEAR(a->political_power, 250.0, 1e-12);

    // Escalating the law replaces the old level's modifiers rather than stacking them.
    apply_law_change(g, *a, 0, 2);
    CHECK_EQ(a->law_levels[0], static_cast<uint8_t>(2));
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::Stability), 0.20, 1e-12);
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::RecruitablePopulation), 0.0, 1e-12);

    // An unrelated law kind is unaffected by the kind-0 changes.
    apply_law_change(g, *a, 1, 1);
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::FactoryOutput), 0.15, 1e-12);
    CHECK_NEAR(a->law_modifiers.get(ModifierKind::Stability), 0.20, 1e-12);
}

// War pulls the home front: stability drifts down, war support up, and the war
// economy releases civilian output.
HOI_TEST(politics_war_drifts_stability_and_war_support) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    make_home_state(g.world, region, alpha, 0.0, "alpha_home");
    make_home_state(g.world, region, beta, 0.0, "beta_home");

    Country* a = g.world.countries.try_get(alpha);
    CHECK(!a->at_war);
    CHECK_NEAR(a->consumer_goods_ratio, 0.35, 1e-12);
    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_politics(g);
    CHECK_NEAR(a->stability, 0.5, 1e-12);  // at ease: no drift
    CHECK_NEAR(a->consumer_goods_ratio, 0.35, 1e-12);

    CHECK(declare_war(g, alpha, beta, {}).valid());
    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_politics(g);
    CHECK(a->at_war);
    CHECK_LT(a->stability, 0.5);
    CHECK_GT(a->war_support, 0.5);
    CHECK_NEAR(a->consumer_goods_ratio, 0.25, 1e-12);
}

// Declaring war pulls faction members and guarantors into the right camps and marks
// both sides at war; repeating the declaration is idempotent.
HOI_TEST(diplomacy_declare_war_pulls_faction_and_guarantors) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId gamma = make_country(g.world, "CCC");
    const CountryId delta = make_country(g.world, "DDD");
    const CountryId beta = make_country(g.world, "BBB");
    const CountryId epsilon = make_country(g.world, "EEE");
    for (CountryId c : {alpha, gamma, delta, beta, epsilon}) {
        make_home_state(g.world, region, c, 0.0, "home");
    }

    Faction pact;
    pact.id = 3;
    pact.leader = alpha;
    pact.members = {alpha, gamma};
    g.world.factions.push_back(pact);
    g.world.countries.try_get(alpha)->faction = 3;
    g.world.countries.try_get(gamma)->faction = 3;
    // Delta guarantees Beta, so it defends Beta; epsilon has no obligation at all.
    g.world.relation(delta, beta).guarantee = true;

    const WarId war = declare_war(g, alpha, beta, {});
    CHECK(war.valid());
    CHECK_EQ(declare_war(g, alpha, beta, {}), war);  // idempotent per pair

    const War* w = g.world.war(war);
    CHECK(w != nullptr && w->active);
    CHECK(w->goals.size() == 1);  // a default war aim exists so peace has a subject
    auto side_has = [](const std::vector<WarParticipant>& side, CountryId c) {
        return std::any_of(side.begin(), side.end(),
                           [&](const WarParticipant& p) { return p.country == c; });
    };
    CHECK(side_has(w->attackers, alpha));
    CHECK(side_has(w->attackers, gamma));
    CHECK(!side_has(w->attackers, delta));
    CHECK(side_has(w->defenders, beta));
    CHECK(side_has(w->defenders, delta));
    CHECK(!side_has(w->defenders, epsilon));

    CHECK(countries_at_war(g.world, alpha, beta));
    CHECK(countries_at_war(g.world, gamma, delta));
    CHECK(!countries_at_war(g.world, alpha, epsilon));
    const std::vector<CountryId> allies = co_belligerents(g.world, alpha);
    CHECK(std::find(allies.begin(), allies.end(), gamma) != allies.end());
    CHECK(std::find(allies.begin(), allies.end(), delta) == allies.end());
    CHECK(g.world.countries.try_get(alpha)->at_war);
    CHECK(g.world.countries.try_get(delta)->at_war);
    CHECK(!g.world.countries.try_get(epsilon)->at_war);
    CHECK(g.world.find_relation(alpha, beta)->at_war);
    CHECK(g.world.find_relation(alpha, beta)->value <= -50.0);
    CHECK(!g.world.find_relation(delta, beta)->guarantee);  // called to arms: pact honoured
}

// Capitulation transfers territory to the occupier, removes the country from play
// and closes its wars.
HOI_TEST(diplomacy_capitulation_transfers_territory_and_closes_wars) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const CountryId vessel = make_country(g.world, "VVV");

    const ProvinceId alpha_home = make_home_state(g.world, region, alpha, 0.0, "alpha_home");
    const StateId beta_main = make_state(g.world, "beta_main", region);
    const ProvinceId beta_capital = make_capital(g.world, beta_main, region, beta, "beta_capital");
    const StateId beta_second = make_state(g.world, "beta_second", region);
    const ProvinceId beta_second_capital =
        make_capital(g.world, beta_second, region, beta, "beta_second_capital");
    link(g.world, alpha_home, beta_capital);
    g.world.states.try_get(beta_main)->civilian_factories = 10;
    g.world.countries.try_get(beta)->starting_factories = 10;
    Faction pact;
    pact.id = 5;
    pact.leader = beta;
    pact.members = {beta};
    g.world.factions.push_back(pact);
    g.world.countries.try_get(beta)->faction = 5;
    const WarId war = declare_war(g, alpha, beta, {});
    // The puppet is bound after the declaration so the war itself stays a clean
    // two-country affair; the release below is what is under test.
    CHECK(war.valid());
    g.world.countries.try_get(beta)->puppets.push_back(vessel);
    g.world.countries.try_get(vessel)->overlord = beta;
    const DivisionId beta_division = g.world.divisions.create();
    {
        Division* d = g.world.divisions.try_get(beta_division);
        d->id = beta_division;
        d->country = beta;
        d->location = beta_capital;
    }

    CHECK(countries_at_war(g.world, alpha, beta));
    CHECK(!check_capitulation(g, beta));  // capital still held: no capitulation yet

    // The defeated country still holds industry assignments and a build queue.
    g.world.countries.try_get(beta)->lines.push_back(ProductionLine{});
    g.world.countries.try_get(beta)->lines.push_back(ProductionLine{});
    g.world.countries.try_get(beta)->construction.queue.push_back(ConstructionProject{});

    // The capital falls with 100% of the industry: the country capitulates.
    g.world.provinces.try_get(beta_capital)->controller = alpha;
    g.world.provinces.try_get(beta_second_capital)->controller = alpha;
    g.world.states.try_get(beta_main)->controller = alpha;
    g.world.states.try_get(beta_second)->controller = alpha;
    CHECK(check_capitulation(g, beta));

    const Country* b = g.world.countries.try_get(beta);
    CHECK(!b->alive);
    CHECK(!b->at_war);
    CHECK(b->wars.empty());
    CHECK(b->divisions.empty());
    CHECK(b->puppets.empty());
    CHECK(g.world.divisions.try_get(beta_division) == nullptr);
    CHECK(g.world.provinces.try_get(beta_capital)->controller == alpha);
    CHECK(g.world.provinces.try_get(beta_capital)->owner == alpha);
    CHECK(g.world.states.try_get(beta_main)->controller == alpha);
    CHECK(g.world.states.try_get(beta_main)->owner == alpha);
    CHECK(g.world.countries.try_get(vessel)->overlord == CountryId{});
    CHECK(g.world.countries.try_get(alpha)->at_war == false);
    CHECK(!g.world.war(war)->active);
    CHECK(!countries_at_war(g.world, alpha, beta));

    // A defeated country owns no industry: assignments and the build queue stand down.
    CHECK(b->lines.empty());
    CHECK(b->construction.queue.empty());
    // An ended war keeps both sides populated (the auditor inspects ended wars too).
    CHECK(!g.world.war(war)->attackers.empty());
    CHECK(!g.world.war(war)->defenders.empty());
}

// Peace is offered only when the war is decided, and it settles the claims the
// winner actually holds.
HOI_TEST(diplomacy_offer_peace_settles_held_goals) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const ProvinceId alpha_home = make_home_state(g.world, region, alpha, 0.0, "alpha_home");
    const StateId beta_main = make_state(g.world, "beta_main", region);
    const ProvinceId beta_capital = make_capital(g.world, beta_main, region, beta, "beta_capital");
    link(g.world, alpha_home, beta_capital);

    std::vector<WarGoal> goals;
    WarGoal goal;
    goal.claimant = alpha;
    goal.target = beta;
    goal.state = beta_main;
    goals.push_back(goal);
    const WarId war = declare_war(g, alpha, beta, goals);

    CHECK(!offer_peace(g, war, alpha));  // the goal is not held yet
    CHECK(!offer_peace(g, war, beta));   // and the loser cannot dictate terms

    g.world.provinces.try_get(beta_capital)->controller = alpha;
    g.world.states.try_get(beta_main)->controller = alpha;
    CHECK(offer_peace(g, war, alpha));
    CHECK(!g.world.war(war)->active);
    CHECK(g.world.provinces.try_get(beta_capital)->controller == alpha);
    CHECK(!g.world.countries.try_get(alpha)->at_war);
    CHECK(!g.world.countries.try_get(beta)->at_war);
    CHECK(!g.world.find_relation(alpha, beta)->at_war);
    CHECK(g.world.find_relation(alpha, beta)->value >= -25.0);
}

// Occupation: resistance and compliance grow on foreign ground and the garrison
// requirement follows them; own ground decays back to zero.
HOI_TEST(occupation_resistance_growth_and_garrison) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const ProvinceId alpha_home = make_home_state(g.world, region, alpha, 100000.0, "alpha_home");
    const StateId occupied = make_state(g.world, "occupied", region);
    const ProvinceId occupied_capital =
        make_capital(g.world, occupied, region, beta, "occupied_capital");
    g.world.provinces.try_get(occupied_capital)->population = 250000.0;
    g.world.states.try_get(occupied)->controller = alpha;  // occupied, not annexed

    phase_occupation(g, alpha);
    const State* s = g.world.states.try_get(occupied);
    CHECK_NEAR(s->resistance, 0.5 / TICKS_PER_DAY, 1e-12);
    CHECK_GT(s->compliance, 0.0);
    CHECK_NEAR(s->garrison_required, s->resistance * 250000.0 * 0.02, 1e-9);

    // One day of occupation: resistance keeps climbing while compliance lags.
    for (int hour = 0; hour < TICKS_PER_DAY; ++hour) phase_occupation(g, alpha);
    CHECK_GT(g.world.states.try_get(occupied)->resistance, 0.5);

    // Own, core ground: the model decays toward zero and needs no garrison.
    const StateId core_state = g.world.provinces.try_get(alpha_home)->state;
    g.world.states.try_get(core_state)->resistance = 0.4;
    g.world.states.try_get(core_state)->compliance = 0.4;
    phase_occupation(g, alpha);
    CHECK_NEAR(g.world.states.try_get(core_state)->resistance, 0.4 - 0.20 / TICKS_PER_DAY, 1e-12);
    CHECK_NEAR(g.world.states.try_get(core_state)->compliance, 0.4 - 0.05 / TICKS_PER_DAY, 1e-12);
    CHECK_NEAR(g.world.states.try_get(core_state)->garrison_required, 0.0, 1e-12);
}

// Control follows whoever stands in the province; ownership does not change.
HOI_TEST(province_control_follows_occupying_divisions) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const StateId state = make_state(g.world, "disputed", region);
    const ProvinceId province = make_province(g.world, "disputed_p", state, region);
    give_state(g.world, state, beta);

    update_province_control(g, province);  // empty province: nothing changes
    CHECK(g.world.provinces.try_get(province)->controller == beta);

    const DivisionId id = g.world.divisions.create();
    {
        Division* d = g.world.divisions.try_get(id);
        d->id = id;
        d->country = alpha;
        d->location = province;
        d->strength = 1.0;
    }
    update_province_control(g, province);
    CHECK(g.world.provinces.try_get(province)->controller == alpha);
    CHECK(g.world.provinces.try_get(province)->owner == beta);  // ownership untouched
}

// Weather is a daily, deterministic pass over land regions with latitude-driven
// temperature, and its multipliers match the documented effect table.
HOI_TEST(weather_is_daily_deterministic_and_affects_movement) {
    Game g;
    const RegionId north = g.world.regions.create();
    const RegionId south = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const StateId ns = make_state(g.world, "north_state", north);
    const StateId ss = make_state(g.world, "south_state", south);
    make_province(g.world, "north_p", ns, north);
    make_province(g.world, "south_p", ss, south);
    give_state(g.world, ns, alpha);
    give_state(g.world, ss, alpha);
    g.world.date = GameDate{1936, 6, 21, 0};

    auto prelude = [](Game& game) {
        game.rng.seed(424242);
        game.world.tick = 0;
        phase_weather(game);
        game.world.tick = 5;  // same day, later hour: no weather change
        phase_weather(game);
    };
    prelude(g);
    const Region* n = g.world.regions.try_get(north);
    CHECK_GT(n->temperature, g.world.regions.try_get(south)->temperature);  // June: north warm
    CHECK(!(n->rain && n->snow));
    const double first_temperature = n->temperature;
    const bool first_rain = n->rain;

    CHECK_NEAR(g.world.regions.try_get(north)->temperature, first_temperature, 1e-12);
    CHECK_EQ(g.world.regions.try_get(north)->rain, first_rain);

    // A month of days: some precipitation must occur, and rain never coexists with
    // snow. Weather converges to the same result for the same seed.
    Game other;
    const RegionId other_north = other.world.regions.create();
    const RegionId other_south = other.world.regions.create();
    const CountryId other_alpha = make_country(other.world, "AAA");
    const StateId other_ns = make_state(other.world, "north_state", other_north);
    const StateId other_ss = make_state(other.world, "south_state", other_south);
    make_province(other.world, "north_p", other_ns, other_north);
    make_province(other.world, "south_p", other_ss, other_south);
    give_state(other.world, other_ns, other_alpha);
    give_state(other.world, other_ss, other_alpha);
    other.world.date = GameDate{1936, 6, 21, 0};
    prelude(other);

    bool precipitation = false;
    for (int day = 0; day < 40; ++day) {
        g.world.tick = static_cast<Tick>(day) * TICKS_PER_DAY;
        other.world.tick = g.world.tick;
        phase_weather(g);
        phase_weather(other);
        const Region* a = g.world.regions.try_get(north);
        const Region* b = other.world.regions.try_get(other_north);
        CHECK_EQ(a->rain, b->rain);
        CHECK_EQ(a->snow, b->snow);
        CHECK_EQ(a->mud, b->mud);
        CHECK_NEAR(a->temperature, b->temperature, 0.0);
        CHECK(!(a->rain && a->snow));
        if (a->rain || a->snow) precipitation = true;
    }
    CHECK(precipitation);

    Region wet;
    wet.rain = true;
    wet.mud = true;
    CHECK_NEAR(weather_movement_multiplier(wet), 1.5, 1e-12);
    CHECK_NEAR(weather_attack_penalty(wet), 0.20, 1e-12);
    CHECK_NEAR(weather_attrition_per_day(wet), 0.010, 1e-12);
    Region clear_sky;
    CHECK_NEAR(weather_movement_multiplier(clear_sky), 1.0, 1e-12);
    CHECK_NEAR(weather_attack_penalty(clear_sky), 0.0, 1e-12);
    CHECK_NEAR(weather_attrition_per_day(clear_sky), 0.0, 1e-12);
}

// Faction membership: every refusal leaves the world untouched.
HOI_TEST(diplomacy_join_faction_refusals_leave_state_untouched) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId gamma = make_country(g.world, "CCC");
    const CountryId delta = make_country(g.world, "DDD");
    const CountryId beta = make_country(g.world, "BBB");
    for (CountryId c : {alpha, gamma, delta, beta}) {
        make_home_state(g.world, region, c, 0.0, "home");
        g.world.countries.try_get(c)->ideology = Ideology::Democratic;
    }
    g.world.countries.try_get(beta)->ideology = Ideology::Fascist;

    Faction pact;
    pact.id = 9;
    pact.name = "Test Pact";
    pact.leader = alpha;
    pact.members = {alpha, gamma};
    g.world.factions.push_back(pact);
    g.world.countries.try_get(alpha)->faction = 9;
    g.world.countries.try_get(gamma)->faction = 9;

    CHECK_EQ(faction_of(g.world, alpha), 9u);
    CHECK_EQ(faction_of(g.world, delta), 0u);  // delta leads nothing

    CHECK(!join_faction(g, alpha, alpha));              // cannot join itself
    CHECK(!join_faction(g, delta, CountryId{}));        // unknown leader
    CHECK(!join_faction(g, gamma, alpha));              // already a member
    CHECK(!join_faction(g, beta, alpha));               // ideology mismatch
    CHECK(declare_war(g, delta, alpha, {}).valid());
    CHECK(!join_faction(g, delta, alpha));              // at war with the leader

    CHECK_EQ(g.world.countries.try_get(delta)->faction, 0u);
    CHECK_EQ(g.world.countries.try_get(beta)->faction, 0u);
    CHECK_EQ(g.world.factions.size(), 1u);
    CHECK_EQ(g.world.factions.front().members.size(), 2u);
}

// Joining means co-belligerence, and a departing leader hands over to the lowest-id
// successor; the last member out dissolves the faction.
HOI_TEST(diplomacy_join_faction_then_leader_steps_down) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId gamma = make_country(g.world, "CCC");
    const CountryId delta = make_country(g.world, "DDD");
    for (CountryId c : {alpha, gamma, delta}) {
        make_home_state(g.world, region, c, 0.0, "home");
        g.world.countries.try_get(c)->ideology = Ideology::Democratic;
    }

    Faction pact;
    pact.id = 9;
    pact.name = "Test Pact";
    pact.leader = alpha;
    pact.members = {alpha, gamma};
    g.world.factions.push_back(pact);
    g.world.countries.try_get(alpha)->faction = 9;
    g.world.countries.try_get(gamma)->faction = 9;

    CHECK(join_faction(g, delta, alpha));
    CHECK_EQ(g.world.countries.try_get(delta)->faction, 9u);
    CHECK(g.world.find_relation(alpha, delta)->value >= 25.0);
    auto is_member = [&](CountryId c) {
        const std::vector<CountryId> allies = co_belligerents(g.world, alpha);
        return std::find(allies.begin(), allies.end(), c) != allies.end();
    };
    CHECK(is_member(delta));
    CHECK(is_member(gamma));

    // A non-leader leaves: the faction carries on under the same leader.
    CHECK(leave_faction(g, gamma));
    CHECK(g.world.countries.try_get(gamma)->faction == 0u);
    CHECK_EQ(faction_of(g.world, alpha), 9u);
    CHECK(!is_member(gamma));

    // The leader leaves: the lowest-id remaining member is promoted.
    CHECK(leave_faction(g, alpha));
    CHECK(g.world.countries.try_get(alpha)->faction == 0u);
    CHECK_EQ(faction_of(g.world, delta), 9u);

    // The last member leaving removes the empty faction entirely.
    CHECK(leave_faction(g, delta));
    CHECK_EQ(faction_of(g.world, delta), 0u);
    CHECK(g.world.factions.empty());
    CHECK(!leave_faction(g, delta));
}

// A war outlives a defeated member: the surviving belligerents keep fighting, and the
// war only ends once a side has no living participant left. Neither side of the war
// record is ever emptied.
HOI_TEST(diplomacy_war_outlives_a_capitulating_ally) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    const CountryId beta = make_country(g.world, "BBB");
    const CountryId delta = make_country(g.world, "DDD");
    const ProvinceId alpha_home = make_home_state(g.world, region, alpha, 0.0, "alpha_home");
    const StateId beta_main = make_state(g.world, "beta_main", region);
    const ProvinceId beta_capital = make_capital(g.world, beta_main, region, beta, "beta_capital");
    const StateId delta_main = make_state(g.world, "delta_main", region);
    const ProvinceId delta_capital = make_capital(g.world, delta_main, region, delta, "delta_capital");
    link(g.world, alpha_home, beta_capital);
    g.world.states.try_get(beta_main)->civilian_factories = 10;
    g.world.states.try_get(delta_main)->civilian_factories = 10;
    g.world.countries.try_get(beta)->starting_factories = 10;
    g.world.countries.try_get(delta)->starting_factories = 10;

    Faction pact;
    pact.id = 11;
    pact.leader = beta;
    pact.members = {beta, delta};
    g.world.factions.push_back(pact);
    g.world.countries.try_get(beta)->faction = 11;
    g.world.countries.try_get(delta)->faction = 11;

    const WarId war = declare_war(g, alpha, beta, {});
    CHECK(war.valid());
    CHECK(countries_at_war(g.world, alpha, delta));  // delta joins the defence

    g.world.provinces.try_get(beta_capital)->controller = alpha;
    g.world.states.try_get(beta_main)->controller = alpha;
    CHECK(check_capitulation(g, beta));
    // Beta is gone, the war is not: delta still holds the line.
    CHECK(!g.world.countries.try_get(beta)->alive);
    CHECK(g.world.war(war)->active);
    CHECK(countries_at_war(g.world, alpha, delta));
    CHECK(g.world.countries.try_get(alpha)->at_war);
    CHECK(g.world.countries.try_get(delta)->at_war);
    CHECK(!g.world.war(war)->attackers.empty());
    CHECK(!g.world.war(war)->defenders.empty());

    // When the last defender goes, the war is over for everyone.
    g.world.provinces.try_get(delta_capital)->controller = alpha;
    g.world.states.try_get(delta_main)->controller = alpha;
    CHECK(check_capitulation(g, delta));
    CHECK(!g.world.war(war)->active);
    CHECK(!countries_at_war(g.world, alpha, delta));
    CHECK(!g.world.countries.try_get(alpha)->at_war);
    CHECK(!g.world.war(war)->attackers.empty());
    CHECK(!g.world.war(war)->defenders.empty());
}

// Occupation only applies to ground that has a political owner: unowned/neutral
// states accumulate no resistance and require no garrison.
HOI_TEST(occupation_skips_unowned_states) {
    Game g;
    const RegionId region = g.world.regions.create();
    const CountryId alpha = make_country(g.world, "AAA");
    make_home_state(g.world, region, alpha, 0.0, "alpha_home");
    const StateId neutral = make_state(g.world, "neutral", region);
    const ProvinceId neutral_province = make_province(g.world, "neutral_p", neutral, region);
    g.world.provinces.try_get(neutral_province)->population = 500000.0;
    g.world.states.try_get(neutral)->controller = alpha;  // held, but owned by nobody

    phase_occupation(g, alpha);
    CHECK_NEAR(g.world.states.try_get(neutral)->resistance, 0.0, 1e-12);
    CHECK_NEAR(g.world.states.try_get(neutral)->compliance, 0.0, 1e-12);
    CHECK_NEAR(g.world.states.try_get(neutral)->garrison_required, 0.0, 1e-12);
}
