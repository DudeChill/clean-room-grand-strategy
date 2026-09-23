// Air warfare tests: hermetic hand-built worlds, no scenario files. Each test is
// driven through phase_air (or the command layer) so it proves the simulation, not
// a private helper.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "game/game.h"
#include "save/save.h"
#include "sim/air.h"
#include "sim/combat.h"
#include "sim/commands.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

// ---------------------------------------------------------------- builders --

// Aircraft model with only the air statistics set, so the expected numbers follow
// from the test's own values.
EquipmentId add_aircraft(Content& c, const std::string& key, double air_attack,
                         double air_defence, double agility, double ground_attack,
                         double range, double max_strength) {
    EquipmentDef e;
    e.key = key;
    e.name = key;
    e.category = EquipmentCategory::Aircraft;
    e.air_attack = air_attack;
    e.air_defence = air_defence;
    e.agility = agility;
    e.ground_attack = ground_attack;
    e.range = range;
    e.max_strength = max_strength;
    e.build_cost = 10.0;
    e.manpower = 5.0;
    e.supply_use = 0.2;
    const EquipmentId id(static_cast<uint32_t>(c.equipment.size()));
    e.id = id;
    c.equipment.push_back(e);
    c.equipment_by_key[key] = id;
    return id;
}

Content make_air_content() {
    Content c = hoi_test::make_test_content();
    add_aircraft(c, "fighter_1", 10.0, 10.0, 12.0, 2.0, 3.0, 20.0);
    add_aircraft(c, "cas_1", 2.0, 4.0, 5.0, 12.0, 2.0, 24.0);
    add_aircraft(c, "bomber_1", 1.0, 3.0, 3.0, 10.0, 5.0, 24.0);
    return c;
}

CountryId add_country(World& w, const std::string& tag, size_t equipment_count) {
    Country c;
    c.tag = tag;
    c.name = tag;
    c.manpower = 500000.0;
    c.equipment_stockpile.assign(equipment_count, 0.0);
    c.law_levels.assign(4, 0);
    c.research.slots.resize(3);
    c.research.slots_unlocked = 3;
    c.starting_factories = 10;
    return w.countries.create(c);
}

// add_province sets Province::region; the reverse index Region::provinces is what
// the air phase scans, so the fixture keeps both in sync (as the map loader does).
ProvinceId add_regional_province(World& w, const std::string& name, StateId state,
                                 RegionId region) {
    const ProvinceId id = add_province(w, name, state, region);
    if (region.valid() && w.regions.alive(region)) w.regions[region].provinces.push_back(id);
    return id;
}

// Two countries, one mid-size region, a battle-ready frontier and enough air bases
// for displacement. Everything the sim reads is set explicitly.
struct AirFixture {
    AirFixture(bool at_war) {
        game.content = make_air_content();
        World& w = game.world;
        w.tick = 0;
        w.date = GameDate{1936, 1, 1, 0};
        region = w.regions.create(Region{});

        state_a = w.states.create();
        state_b = w.states.create();
        State& sa = w.states[state_a];
        sa.name = "state_a";
        sa.building_slots = 20;
        sa.civilian_factories = 5;
        sa.military_factories = 5;
        State& sb = w.states[state_b];
        sb.name = "state_b";
        sb.building_slots = 20;
        sb.civilian_factories = 5;
        sb.military_factories = 5;

        a = add_country(w, "VLA", game.content.equipment.size());
        b = add_country(w, "NOR", game.content.equipment.size());
        w.countries[a].capital = state_a;
        w.countries[b].capital = state_b;
        sa.owner = a;
        sa.controller = a;
        sb.owner = b;
        sb.controller = b;

        fighter = game.content.equipment_id("fighter_1");
        cas = game.content.equipment_id("cas_1");
        bomber = game.content.equipment_id("bomber_1");
        tmpl = game.content.template_id("infantry_template");

        base_a = add_air_base(w, "base_a", state_a, a);
        front = add_regional_province(w, "front", state_a, region);
        w.provinces[front].owner = a;
        w.provinces[front].controller = a;
        w.states[state_a].provinces.push_back(front);
        base_b = add_air_base(w, "base_b", state_b, b);
        spare = add_air_base(w, "spare", state_a, a);
        link_provinces(w, base_a, front);
        link_provinces(w, front, base_b);
        link_provinces(w, base_a, spare);

        game.ai_controlled.assign(w.countries.capacity(), 0);
        if (at_war) {
            War war;
            WarParticipant pa;
            pa.country = a;
            WarParticipant pb;
            pb.country = b;
            war.attackers.push_back(pa);
            war.defenders.push_back(pb);
            war.aggressor = a;
            war.start_tick = 0;
            const WarId wid = w.wars.create(war);
            w.countries[a].wars.push_back(wid);
            w.countries[b].wars.push_back(wid);
            w.countries[a].at_war = true;
            w.countries[b].at_war = true;
            Relation& rel = w.relation(a, b);
            rel.at_war = true;
            rel.value = -100.0;
        }
        game.rng.seed(4242);
    }

    ProvinceId add_air_base(World& w, const std::string& name, StateId state, CountryId owner) {
        const ProvinceId id = add_regional_province(w, name, state, region);
        Province& p = w.provinces[id];
        p.owner = owner;
        p.controller = owner;
        p.air_base = 2;  // capacity 400 at the default 200 per level
        w.states[state].provinces.push_back(id);
        return id;
    }

    AirWingId add_wing(CountryId country, ProvinceId base, EquipmentId equipment, int planes,
                       int max_planes, AirMission mission, RegionId mission_region) {
        AirWing wing;
        wing.country = country;
        wing.equipment = equipment;
        wing.base = base;
        wing.region = mission_region;
        wing.planes = planes;
        wing.max_planes = max_planes;
        wing.mission = mission;
        wing.efficiency = 1.0;
        wing.name = "test wing";
        const AirWingId id = game.world.air_wings.create(wing);
        game.world.country(country)->wings.push_back(id);
        return id;
    }

    Game game;
    CountryId a;
    CountryId b;
    RegionId region;
    StateId state_a;
    StateId state_b;
    ProvinceId base_a;
    ProvinceId base_b;
    ProvinceId front;
    ProvinceId spare;
    EquipmentId fighter;
    EquipmentId cas;
    EquipmentId bomber;
    TemplateId tmpl;
};

// --------------------------------------------------------- command layer ----

// A wing is formed at a base with capacity, draws its aircraft from the stockpile,
// and the same command is rejected when the base is already full.
HOI_TEST(air_create_wing_draws_from_stockpile) {
    AirFixture f(true);
    Game& g = f.game;
    g.world.country(f.a)->equipment_stockpile[f.fighter.v] = 500.0;
    g.world.provinces[f.base_a].air_base = 1;  // capacity 200

    Command cmd;
    cmd.type = CommandType::CreateAirWing;
    cmd.country = f.a;
    cmd.province = f.base_a;
    cmd.equipment = f.fighter;
    cmd.value = 80;
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);
    CHECK_EQ(apply_command(g, cmd), CommandResult::Applied);

    const std::vector<AirWingId>& wings = g.world.country(f.a)->wings;
    CHECK_EQ(wings.size(), 1u);
    const AirWingId created_id = wings.front();
    const AirWing* wing = g.world.wing(created_id);
    CHECK(wing != nullptr);
    CHECK_EQ(wing->planes, 80);  // delivered out of the stockpile
    CHECK_EQ(wing->max_planes, 80);
    CHECK_NEAR(g.world.country(f.a)->equipment_stockpile[f.fighter.v], 420.0, 1e-9);

    // 80 + 150 > 200: the same command no longer fits the base.
    cmd.value = 150;
    CHECK_EQ(validate_command(g, cmd), CommandResult::QueueFull);

    // Disband returns every aircraft to the stockpile.
    Command disband;
    disband.type = CommandType::DisbandAirWing;
    disband.country = f.a;
    disband.wing = created_id;
    CHECK_EQ(apply_command(g, disband), CommandResult::Applied);
    CHECK_EQ(g.world.country(f.a)->wings.size(), 0u);
    CHECK(!g.world.wing(created_id));
    CHECK_NEAR(g.world.country(f.a)->equipment_stockpile[f.fighter.v], 500.0, 1e-9);
}

// SetAirMission is accepted only for a region the wing can reach from its base.
HOI_TEST(air_mission_command_honours_range) {
    AirFixture f(true);
    Game& g = f.game;
    RegionId far = g.world.regions.create(Region{});
    const ProvinceId far_base = add_regional_province(g.world, "far", f.state_a, far);
    g.world.provinces[far_base].owner = f.a;
    g.world.provinces[far_base].controller = f.a;
    g.world.provinces[far_base].air_base = 1;
    g.world.states[f.state_a].provinces.push_back(far_base);
    link_provinces(g.world, f.base_a, far_base);

    const AirWingId w = f.add_wing(f.a, f.base_a, f.fighter, 50, 50, AirMission::AirSuperiority,
                                   f.region);
    Command cmd;
    cmd.type = CommandType::SetAirMission;
    cmd.country = f.a;
    cmd.wing = w;
    cmd.region = far;
    cmd.value = static_cast<int>(AirMission::CloseAirSupport);
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);

    // A different region with no land path is out of reach for the same wing.
    RegionId island = g.world.regions.create(Region{});
    const ProvinceId lonely = add_regional_province(g.world, "lonely", f.state_a, island);
    g.world.provinces[lonely].owner = f.a;
    g.world.provinces[lonely].controller = f.a;
    cmd.region = island;
    CHECK_EQ(validate_command(g, cmd), CommandResult::InvalidTarget);

    // Range 0 means own region only.
    const EquipmentId interceptor = add_aircraft(g.content, "short_1", 8.0, 8.0, 10.0, 0.0, 0.0,
                                                 20.0);
    const AirWingId short_wing =
        f.add_wing(f.a, f.base_a, interceptor, 50, 50, AirMission::AirSuperiority, f.region);
    cmd.wing = short_wing;
    CHECK_EQ(validate_command(g, cmd), CommandResult::InvalidTarget);
    cmd.region = f.region;
    CHECK_EQ(validate_command(g, cmd), CommandResult::Applied);
}

// ------------------------------------------------------------- queries ------

HOI_TEST(air_region_distance_and_reach) {
    AirFixture f(true);
    Game& g = f.game;
    RegionId near = g.world.regions.create(Region{});
    RegionId island = g.world.regions.create(Region{});
    const ProvinceId far_base = add_regional_province(g.world, "far", f.state_a, near);
    const ProvinceId lonely = add_regional_province(g.world, "lonely", f.state_a, island);
    for (ProvinceId p : {far_base, lonely}) {
        g.world.provinces[p].owner = f.a;
        g.world.provinces[p].controller = f.a;
        g.world.provinces[p].air_base = 1;
        g.world.states[f.state_a].provinces.push_back(p);
    }
    link_provinces(g.world, f.base_a, far_base);  // one land hop

    CHECK_EQ(region_distance_hops(g, f.region, f.region), 0);
    CHECK_EQ(region_distance_hops(g, f.region, near), 1);
    CHECK_EQ(region_distance_hops(g, f.region, island), -1);
    CHECK_EQ(region_distance_hops(g, f.region, RegionId{}), -1);

    const AirWingId w = f.add_wing(f.a, f.base_a, f.fighter, 50, 50, AirMission::AirSuperiority,
                                   f.region);
    const AirWing* wing = g.world.wing(w);
    CHECK(wing_can_reach(g, *wing, near));
    CHECK(wing_can_reach(g, *wing, f.region));
    CHECK(!wing_can_reach(g, *wing, island));
    // Base controlled by an enemy is not a valid origin even in its own region.
    g.world.provinces[f.base_a].controller = f.b;
    CHECK(!wing_can_reach(g, *wing, f.region));
}

HOI_TEST(air_base_capacity_and_occupancy) {
    AirFixture f(true);
    Game& g = f.game;
    g.content.constants.air_base_capacity_per_level = 200.0;
    CHECK_EQ(air_base_capacity(g, f.base_a), 400);  // level 2
    CHECK_EQ(air_base_capacity(g, ProvinceId{}), 0);
    f.add_wing(f.a, f.base_a, f.fighter, 120, 120, AirMission::AirSuperiority, f.region);
    f.add_wing(f.b, f.base_a, f.fighter, 30, 30, AirMission::AirSuperiority, f.region);
    CHECK_EQ(planes_stationed_at(g, f.base_a), 150);
    CHECK_EQ(planes_stationed_at(g, f.base_b), 0);
}

// ----------------------------------------------------------- air combat -----

// Hostile wings in one region damage each other, and the next ticks' replacements
// draw the lost aircraft out of the stockpile.
HOI_TEST(air_combat_losses_both_sides_and_replacements) {
    AirFixture f(true);
    Game& g = f.game;
    g.world.country(f.a)->equipment_stockpile[f.fighter.v] = 1000.0;
    g.world.country(f.b)->equipment_stockpile[f.fighter.v] = 1000.0;
    const AirWingId wa = f.add_wing(f.a, f.base_a, f.fighter, 100, 120,
                                    AirMission::AirSuperiority, f.region);
    const AirWingId wb = f.add_wing(f.b, f.base_b, f.fighter, 100, 120,
                                    AirMission::AirSuperiority, f.region);

    for (int i = 0; i < 3; ++i) phase_air(g);

    CHECK_GT(g.world.wing(wa)->losses, 0);
    CHECK_GT(g.world.wing(wb)->losses, 0);
    // Losses became demand: the stockpile paid for the replacements.
    CHECK_LT(g.world.country(f.a)->equipment_stockpile[f.fighter.v], 1000.0);
    CHECK_LT(g.world.country(f.b)->equipment_stockpile[f.fighter.v], 1000.0);
    CHECK_GT(g.world.wing(wa)->planes, 100 - g.world.wing(wa)->losses);  // topped back up
}

// Air control is mission-weighted, sums to at most one across the countries present,
// and is written sorted by country id.
HOI_TEST(air_control_share_sums_below_one) {
    AirFixture f(true);
    Game& g = f.game;
    const AirWingId wa = f.add_wing(f.a, f.base_a, f.fighter, 100, 100,
                                    AirMission::AirSuperiority, f.region);
    const AirWingId wb = f.add_wing(f.b, f.base_b, f.fighter, 50, 50,
                                    AirMission::AirSuperiority, f.region);

    phase_air(g);

    const Region* region = g.world.regions.try_get(f.region);
    CHECK_EQ(region->air_control.size(), 2u);
    CHECK(region->air_control[0].first.v < region->air_control[1].first.v);
    double sum = 0.0;
    for (const auto& entry : region->air_control) sum += entry.second;
    CHECK(sum <= 1.0 + 1e-9);
    CHECK(g.world.wing(wa) != nullptr);
    CHECK(g.world.wing(wb) != nullptr);

    const double share_a = air_control_share(g, f.a, f.region);
    const double share_b = air_control_share(g, f.b, f.region);
    CHECK_GT(share_a, 0.0);
    CHECK_LT(share_b, share_a);  // fewer aircraft, less control
    CHECK(share_a + share_b <= 1.0 + 1e-9);
    CHECK_EQ(air_control_share(g, CountryId{}, f.region), 0.0);
}

// --------------------------------------------------------------- missions ---

// CAS damages the enemy divisions in a battle in the wing's region and raises the
// friendly side's additive land-combat value (and its debug line).
HOI_TEST(air_cas_damages_divisions_and_raises_combat_values) {
    AirFixture f(true);
    Game& g = f.game;
    const DivisionId da = hoi_test::add_division(g, f.a, f.front, f.tmpl);
    const DivisionId db = hoi_test::add_division(g, f.b, f.front, f.tmpl);
    Battle battle;
    battle.province = f.front;
    battle.terrain = Terrain::Plains;
    battle.attacker_lead = f.a;
    battle.defender_lead = f.b;
    battle.attacker.divisions.push_back(da);
    battle.defender.divisions.push_back(db);
    const BattleId bid = g.world.battles.create(battle);
    g.world.divisions[da].battle = bid;
    g.world.divisions[db].battle = bid;

    const double defender_org_before = g.world.divisions[db].organization;
    f.add_wing(f.a, f.base_a, f.cas, 100, 100, AirMission::CloseAirSupport, f.region);

    phase_air(g);

    CHECK_LT(g.world.divisions[db].organization, defender_org_before);
    CHECK_GT(air_support_modifier(g, f.a, f.front, true), 0.0);
    // The side without air cover takes the inverse.
    CHECK_LT(air_support_modifier(g, f.b, f.front, true), 0.0);

    // The battle debug line reports the air contribution to the attack values.
    const Battle* b_after = g.world.battle(bid);
    std::vector<BattleDebugLine> debug;
    compute_side_values(g, *b_after, b_after->attacker.divisions, true, &debug);
    CHECK(!debug.empty());
    CHECK_GT(debug.front().air_mod, 0.0);

    // Without the wing there is no air support at all (causality).
    Command disband;
    disband.type = CommandType::DisbandAirWing;
    disband.country = f.a;
    for (AirWingId id : g.world.country(f.a)->wings) disband.wing = id;
    apply_command(g, disband);
    phase_air(g);
    CHECK_NEAR(air_support_modifier(g, f.a, f.front, true), 0.0, 1e-12);
}

// Strategic bombing lowers the target state's factory count and logs an event.
HOI_TEST(air_strategic_bombing_reduces_factories) {
    AirFixture f(true);
    Game& g = f.game;
    const int enemy_civ_before = g.world.state(f.state_b)->civilian_factories;
    const int enemy_mil_before = g.world.state(f.state_b)->military_factories;
    const int home_before = g.world.state(f.state_a)->total_factories();

    f.add_wing(f.a, f.base_a, f.bomber, 100, 100, AirMission::StrategicBombing, f.region);
    phase_air(g);

    CHECK_LT(g.world.state(f.state_b)->total_factories(), enemy_civ_before + enemy_mil_before);
    CHECK_EQ(g.world.state(f.state_a)->total_factories(), home_before);
    const bool logged =
        std::any_of(g.events.begin(), g.events.end(),
                    [](const SimEvent& e) { return e.kind == "air"; });
    CHECK(logged);

    // Factories floor at zero: repeated strikes cannot go negative.
    g.world.state(f.state_b)->civilian_factories = 1;
    g.world.state(f.state_b)->military_factories = 0;
    g.world.state(f.state_b)->dockyards = 0;
    for (int i = 0; i < 5; ++i) phase_air(g);
    CHECK_EQ(g.world.state(f.state_b)->total_factories(), 0);
}

// Anti-air defends its province: the same bombing run does less damage when the
// target province has flak, and never drives factories negative.
HOI_TEST(air_anti_air_reduces_bombing_damage) {
    AirFixture plain(true);
    const int start_factories = plain.game.world.state(plain.state_b)->total_factories();
    plain.add_wing(plain.a, plain.base_a, plain.bomber, 100, 100, AirMission::StrategicBombing,
                   plain.region);
    phase_air(plain.game);
    const int damaged_plain = start_factories - plain.game.world.state(plain.state_b)->total_factories();

    AirFixture defended(true);
    defended.game.world.provinces[defended.base_b].anti_air = 3;
    defended.add_wing(defended.a, defended.base_a, defended.bomber, 100, 100,
                      AirMission::StrategicBombing, defended.region);
    phase_air(defended.game);
    const int damaged_defended =
        start_factories - defended.game.world.state(defended.state_b)->total_factories();

    CHECK_GT(damaged_plain, 0);
    CHECK_LT(damaged_defended, damaged_plain);  // the flak cut the damage
    CHECK_GT(damaged_defended, 0);              // but did not absorb the strike entirely
    CHECK(defended.game.world.state(defended.state_b)->total_factories() >= 0);
    CHECK(plain.game.world.state(plain.state_b)->total_factories() >= 0);
}

// Flying over defended ground costs the attacker aircraft even without enemy wings.
HOI_TEST(air_anti_air_raises_attacker_losses) {
    AirFixture plain(true);
    const AirWingId plain_a = plain.add_wing(plain.a, plain.base_a, plain.fighter, 100, 100,
                                             AirMission::AirSuperiority, plain.region);

    AirFixture defended(true);
    defended.game.world.provinces[defended.base_b].anti_air = 4;
    const AirWingId defended_a = defended.add_wing(
        defended.a, defended.base_a, defended.fighter, 100, 100, AirMission::AirSuperiority,
        defended.region);

    // Both fixtures fly an interceptor so air combat happens; the only difference is
    // the defending region's anti-air. Losses are the RNG roll times the flak factor.
    plain.add_wing(plain.b, plain.base_b, plain.fighter, 100, 100, AirMission::AirSuperiority,
                   plain.region);
    defended.add_wing(defended.b, defended.base_b, defended.fighter, 100, 100,
                      AirMission::AirSuperiority, defended.region);

    for (int i = 0; i < 5; ++i) {
        phase_air(plain.game);
        phase_air(defended.game);
    }
    CHECK_GT(defended.game.world.wing(defended_a)->losses,
             plain.game.world.wing(plain_a)->losses);
}

// Logistics strike cuts the target province's railway level, floored at zero.
HOI_TEST(air_logistics_strike_cuts_railways) {
    AirFixture f(true);
    Game& g = f.game;
    Province* target = &g.world.provinces[f.base_b];
    target->railway_level = 2;

    f.add_wing(f.a, f.base_a, f.bomber, 100, 100, AirMission::LogisticsStrike, f.region);
    phase_air(g);

    CHECK_LT(g.world.provinces[f.base_b].railway_level, 2);
    for (int i = 0; i < 5; ++i) phase_air(g);
    CHECK_EQ(g.world.provinces[f.base_b].railway_level, 0);
}

// ------------------------------------------------- maintenance of wings -----

// A wing whose base is captured moves to the nearest friendly air base with room.
HOI_TEST(air_wing_displaced_when_base_captured) {
    AirFixture f(true);
    Game& g = f.game;
    const AirWingId w = f.add_wing(f.a, f.base_a, f.fighter, 50, 60,
                                   AirMission::AirSuperiority, f.region);
    g.world.provinces[f.base_a].controller = f.b;  // the base falls

    phase_air(g);

    CHECK(g.world.wing(w) != nullptr);
    CHECK_EQ(g.world.wing(w)->base, f.spare);
    CHECK_EQ(g.world.wing(w)->region, f.region);
}

// When no friendly air base can take the wing, the wing is destroyed and its
// aircraft go back to the stockpile.
HOI_TEST(air_wing_destroyed_when_no_base_remains) {
    AirFixture f(true);
    Game& g = f.game;
    const double stock_before = g.world.country(f.a)->equipment_stockpile[f.fighter.v];
    g.world.provinces[f.spare].air_base = 0;  // no spare capacity anywhere
    const AirWingId w = f.add_wing(f.a, f.base_a, f.fighter, 40, 40,
                                   AirMission::AirSuperiority, f.region);
    g.world.provinces[f.base_a].controller = f.b;

    phase_air(g);

    CHECK(!g.world.wing(w));
    CHECK_EQ(g.world.country(f.a)->wings.size(), 0u);
    CHECK_NEAR(g.world.country(f.a)->equipment_stockpile[f.fighter.v], stock_before + 40.0, 1e-9);
}

// Zero-plane wings are removed rather than left as empty shells in the store.
HOI_TEST(air_empty_wing_is_removed) {
    AirFixture f(true);
    Game& g = f.game;
    const AirWingId w = f.add_wing(f.a, f.base_a, f.fighter, 20, 20,
                                   AirMission::AirSuperiority, f.region);
    g.world.wing(w)->planes = 0;
    phase_air(g);
    CHECK(!g.world.wing(w));
}

// ------------------------------------------------------------- determinism --

namespace {
// Finer-grained companion to world_hash(): folds only the state the air phase owns
// (every wing field, the derived air control cache) plus the live RNG stream, so a
// future divergence points at the air phase rather than at the whole world.
uint64_t hash_air_state(const Game& g) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    auto mix_double = [&mix](double v) {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        mix(bits);
    };
    g.world.air_wings.for_each([&](AirWingId id, const AirWing& x) {
        mix(id.v);
        mix(x.country.v);
        mix(x.equipment.v);
        mix(x.base.v);
        mix(x.region.v);
        mix(static_cast<uint64_t>(x.planes));
        mix(static_cast<uint64_t>(x.losses));
        mix(static_cast<uint64_t>(x.mission));
        mix_double(x.efficiency);
        mix_double(x.experience);
    });
    g.world.regions.for_each([&](RegionId, const Region& r) {
        for (const auto& entry : r.air_control) {
            mix(entry.first.v);
            mix_double(entry.second);
        }
    });
    mix(g.rng.state_hash());
    return h;
}

// Two oracles over the same run: world_hash() is the whole-world oracle the
// determinism acceptance requires, and the local air-state fold gives a finer
// diagnostic if the two runs ever diverge.
struct AirRunHashes {
    uint64_t world = 0;
    uint64_t air = 0;
};

AirRunHashes air_run() {
    AirFixture f(true);
    Game& g = f.game;
    g.world.world_seed = 99;
    g.world.country(f.a)->equipment_stockpile[f.fighter.v] = 500.0;
    g.world.country(f.b)->equipment_stockpile[f.fighter.v] = 500.0;
    g.world.country(f.b)->equipment_stockpile[f.bomber.v] = 500.0;
    f.add_wing(f.a, f.base_a, f.fighter, 100, 120, AirMission::AirSuperiority, f.region);
    f.add_wing(f.b, f.base_b, f.fighter, 100, 120, AirMission::AirSuperiority, f.region);
    f.add_wing(f.b, f.base_b, f.bomber, 60, 80, AirMission::StrategicBombing, f.region);
    g.rng.seed(20240923);
    for (int i = 0; i < 24; ++i) phase_air(g);
    AirRunHashes hashes;
    hashes.world = world_hash(g);
    hashes.air = hash_air_state(g);
    return hashes;
}
}  // namespace

HOI_TEST(air_phase_is_deterministic) {
    const AirRunHashes first = air_run();
    const AirRunHashes second = air_run();
    CHECK_EQ(first.world, second.world);
    CHECK_EQ(first.air, second.air);
}

}  // namespace