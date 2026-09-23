// Military slice tests: template aggregation, movement cost, incremental combat,
// retreat/destruction rules, organisation recovery and armour advantage.
//
// Worlds are hand-built (no scenario loading) so each case is exact.

#include <cmath>
#include <string>
#include <vector>

#include "game/game.h"
#include "sim/combat.h"
#include "sim/phases.h"
#include "sim/units.h"
#include "sim/world.h"
#include "test.h"

using namespace hoi;

namespace {

// ------------------------------------------------------------- fixtures ------

EquipmentId add_equipment(Game& g, const char* key, double soft, double hard, double def,
                          double brk, double armor, double pierce, double speed, double hp,
                          double org, double supply, double cost, double manpower) {
    EquipmentDef e;
    e.id = EquipmentId(static_cast<uint32_t>(g.content.equipment.size()));
    e.key = key;
    e.name = key;
    e.soft_attack = soft;
    e.hard_attack = hard;
    e.defense = def;
    e.breakthrough = brk;
    e.armor = armor;
    e.piercing = pierce;
    e.speed = speed;
    e.max_strength = hp;
    e.organization = org;
    e.supply_use = supply;
    e.build_cost = cost;
    e.manpower = manpower;
    g.content.equipment.push_back(e);
    g.content.equipment_by_key[key] = e.id;
    return e.id;
}

TemplateId add_template(Game& g, const char* key, CountryId owner,
                        std::vector<BattalionSlot> slots) {
    DivisionTemplate t;
    t.id = TemplateId(static_cast<uint32_t>(g.content.templates.size()));
    t.key = key;
    t.name = key;
    t.country = owner;
    t.battalions = std::move(slots);
    recompute_template_stats(t, g.content.equipment);
    g.content.templates.push_back(t);
    g.content.template_by_key[key] = t.id;
    return t.id;
}

CountryId add_country(World& w, const char* tag) {
    CountryId id = w.countries.create();
    Country& c = *w.countries.try_get(id);
    c.id = id;
    c.tag = tag;
    c.name = tag;
    return id;
}

StateId add_state(World& w, const char* name, CountryId owner) {
    StateId id = w.states.create();
    State& s = *w.states.try_get(id);
    s.id = id;
    s.name = name;
    s.owner = owner;
    s.controller = owner;
    return id;
}

ProvinceId add_province(World& w, const char* name, Terrain terrain, StateId state,
                        CountryId owner, int infrastructure) {
    ProvinceId id = w.provinces.create();
    Province& p = *w.provinces.try_get(id);
    p.id = id;
    p.name = name;
    p.terrain = terrain;
    p.state = state;
    p.owner = owner;
    p.controller = owner;
    p.infrastructure = infrastructure;
    p.population = 100000.0;
    return id;
}

void link(World& w, ProvinceId a, ProvinceId b) {
    w.province(a)->adj.push_back(b);
    w.province(b)->adj.push_back(a);
}

void add_war(World& w, CountryId attacker, CountryId defender) {
    WarId id = w.wars.create();
    War& war = *w.wars.try_get(id);
    war.id = id;
    war.active = true;
    war.aggressor = attacker;
    WarParticipant ap;
    ap.country = attacker;
    WarParticipant dp;
    dp.country = defender;
    war.attackers.push_back(ap);
    war.defenders.push_back(dp);
}

DivisionId add_division(Game& g, CountryId country, TemplateId tpl, ProvinceId location) {
    World& w = g.world;
    DivisionId id = w.divisions.create();
    Division& d = *w.divisions.try_get(id);
    d.id = id;
    d.country = country;
    d.template_id = tpl;
    d.location = location;
    const DivisionTemplate* t = g.content.template_def(tpl);
    d.equipment.assign(g.content.equipment.size(), 0.0);
    if (t) {
        for (const BattalionSlot& s : t->battalions) {
            if (s.equipment.valid() && s.equipment.v < d.equipment.size()) {
                d.equipment[s.equipment.v] += static_cast<double>(s.count);
            }
        }
    }
    d.strength = 1.0;
    d.supply = 1.0;
    d.fuel = 1.0;
    const DivisionStats stats = compute_division_stats(g, d);
    d.max_organization = stats.max_organization;
    d.organization = stats.max_organization;
    d.manpower = t ? t->manpower : 0.0;
    Country* c = w.country(country);
    if (c) c->divisions.push_back(id);
    return id;
}

// A generic two-country world with one equipment model.
struct BaseWorld {
    Game g;
    CountryId a;
    CountryId b;
    EquipmentId rifle;
    TemplateId infantry;

    BaseWorld() {
        a = add_country(g.world, "AAA");
        b = add_country(g.world, "BBB");
        rifle = add_equipment(g, "rifle", 6.0, 0.5, 8.0, 2.0, 0.0, 1.0, 4.0, 25.0, 60.0, 0.2,
                              0.5, 1000.0);
        BattalionSlot slot;
        slot.equipment = rifle;
        slot.count = 1;
        infantry = add_template(g, "infantry", a, {slot});
        g.rng.seed(12345);
    }
};

}  // namespace

HOI_TEST(military_template_stat_aggregation) {
    BaseWorld w;
    EquipmentId support = add_equipment(w.g, "support", 2.0, 0.0, 1.0, 0.0, 0.0, 0.0, 5.0,
                                        10.0, 20.0, 0.1, 0.25, 500.0);

    BattalionSlot line;
    line.equipment = w.rifle;
    line.count = 3;
    BattalionSlot sup;
    sup.equipment = support;
    sup.count = 1;
    sup.support = true;

    DivisionTemplate t;
    t.id = TemplateId(0);
    t.key = "mixed";
    t.name = "mixed";
    t.country = w.a;
    t.battalions = {line, sup};
    recompute_template_stats(t, w.g.content.equipment);

    // Line stats are count-scaled; the support company contributes but no width.
    CHECK_NEAR(t.soft_attack, 6.0 * 3 + 2.0, 1e-9);
    CHECK_NEAR(t.defense, 8.0 * 3 + 1.0, 1e-9);
    CHECK_NEAR(t.breakthrough, 2.0 * 3, 1e-9);
    CHECK_NEAR(t.hardness, 0.0, 1e-9);
    CHECK_NEAR(t.armor, 0.0, 1e-9);
    CHECK_NEAR(t.piercing, 1.0, 1e-9);
    CHECK_NEAR(t.max_strength, 25.0 * 3 + 10.0, 1e-9);
    CHECK_NEAR(t.max_organization, (60.0 * 3 + 20.0) / 4.0, 1e-9);
    CHECK_NEAR(t.speed, 4.0, 1e-9);  // support (5.0) does not raise it
    CHECK_NEAR(t.combat_width, 6.0, 1e-9);
    CHECK_NEAR(t.build_cost, 0.5 * 3 + 0.25, 1e-9);
    CHECK_NEAR(t.manpower, 1000.0 * 3 + 500.0, 1e-9);
    CHECK_EQ(t.battalion_count(), 3);
}

HOI_TEST(military_movement_mountains_slower_than_plains) {
    BaseWorld w;
    StateId st = add_state(w.g.world, "home", w.a);
    ProvinceId origin = add_province(w.g.world, "origin", Terrain::Plains, st, w.a, 0);
    ProvinceId plain = add_province(w.g.world, "plain", Terrain::Plains, st, w.a, 0);
    ProvinceId mountain = add_province(w.g.world, "mountain", Terrain::Mountain, st, w.a, 0);
    link(w.g.world, origin, plain);
    link(w.g.world, origin, mountain);

    DivisionId d1 = add_division(w.g, w.a, w.infantry, origin);
    DivisionId d2 = add_division(w.g, w.a, w.infantry, origin);
    w.g.world.divisions.try_get(d1)->path = {plain};
    w.g.world.divisions.try_get(d2)->path = {mountain};

    int plain_ticks = -1;
    int mountain_ticks = -1;
    for (int t = 1; t <= 1000 && (plain_ticks < 0 || mountain_ticks < 0); ++t) {
        phase_movement(w.g);
        if (plain_ticks < 0 && w.g.world.division(d1)->location == plain) plain_ticks = t;
        if (mountain_ticks < 0 && w.g.world.division(d2)->location == mountain) mountain_ticks = t;
    }

    CHECK_GT(plain_ticks, 0);
    CHECK_GT(mountain_ticks, plain_ticks);
    CHECK_EQ(w.g.world.division(d1)->location, plain);
    CHECK_EQ(w.g.world.division(d2)->location, mountain);
}

HOI_TEST(military_unsupplied_half_speed) {
    BaseWorld w;
    StateId st = add_state(w.g.world, "home", w.a);
    ProvinceId origin = add_province(w.g.world, "o", Terrain::Plains, st, w.a, 0);
    ProvinceId dest = add_province(w.g.world, "d", Terrain::Plains, st, w.a, 0);
    link(w.g.world, origin, dest);

    DivisionId fed = add_division(w.g, w.a, w.infantry, origin);
    DivisionId starved = add_division(w.g, w.a, w.infantry, origin);
    w.g.world.divisions.try_get(fed)->supply = 1.0;
    w.g.world.divisions.try_get(starved)->supply = 0.1;
    w.g.world.divisions.try_get(fed)->path = {dest};
    w.g.world.divisions.try_get(starved)->path = {dest};

    int fed_ticks = -1;
    int starved_ticks = -1;
    for (int t = 1; t <= 1000 && (fed_ticks < 0 || starved_ticks < 0); ++t) {
        phase_movement(w.g);
        if (fed_ticks < 0 && w.g.world.division(fed)->location == dest) fed_ticks = t;
        if (starved_ticks < 0 && w.g.world.division(starved)->location == dest) starved_ticks = t;
    }
    CHECK_GT(fed_ticks, 0);
    CHECK_GT(starved_ticks, fed_ticks);
}

// Builds a province P held by B, staging province Q held by A, and optionally a
// retreat province R held by B. `count` divisions of A stand in Q with orders onto P.
struct BattleWorld {
    BaseWorld base;
    ProvinceId p;
    ProvinceId q;
    ProvinceId r;
    std::vector<DivisionId> attackers;
    std::vector<DivisionId> defenders;

    BattleWorld(int count, bool retreat_available) {
        add_war(base.g.world, base.a, base.b);
        StateId sa = add_state(base.g.world, "sa", base.a);
        StateId sb = add_state(base.g.world, "sb", base.b);
        p = add_province(base.g.world, "p", Terrain::Plains, sb, base.b, 0);
        q = add_province(base.g.world, "q", Terrain::Plains, sa, base.a, 0);
        link(base.g.world, p, q);
        if (retreat_available) {
            r = add_province(base.g.world, "r", Terrain::Plains, sb, base.b, 0);
            link(base.g.world, p, r);
        }
        for (int i = 0; i < count; ++i) {
            DivisionId id = add_division(base.g, base.a, base.infantry, q);
            base.g.world.divisions.try_get(id)->order_target = p;
            attackers.push_back(id);
        }
        defenders.push_back(add_division(base.g, base.b, base.infantry, p));
    }
};

HOI_TEST(military_combat_is_incremental_and_attacker_wins) {
    BattleWorld bw(3, true);
    Game& g = bw.base.g;

    phase_combat(g);
    CHECK_EQ(g.world.battles.size(), 1u);
    const Division* def0 = g.world.division(bw.defenders[0]);
    CHECK_GT(def0->organization, 0.0);
    CHECK_LT(def0->organization, def0->max_organization);
    const BattleId bid = def0->battle;
    CHECK(bid.valid());
    CHECK(!g.world.battle(bid)->debug.empty());  // debug breakdown populated

    int ticks = 1;
    while (g.world.battles.size() > 0 && ticks < 2000) {
        phase_combat(g);
        ++ticks;
    }
    CHECK_GT(ticks, 1);
    CHECK_EQ(g.world.battles.size(), 0u);

    // The defender retreated to the one legal province and the attackers advanced.
    CHECK(g.world.division(bw.defenders[0]) != nullptr);
    CHECK_EQ(g.world.division(bw.defenders[0])->location, bw.r);
    CHECK_EQ(g.world.division(bw.attackers[0])->location, bw.p);

    g.world.divisions.for_each([](DivisionId, const Division& d) {
        CHECK(d.organization >= 0.0);
        CHECK(std::isfinite(d.strength));
        CHECK(d.strength >= 0.0 && d.strength <= 1.0);
        for (double q : d.equipment) CHECK(std::isfinite(q));
    });
}

HOI_TEST(military_defender_surrounded_is_destroyed) {
    BattleWorld bw(3, false);
    Game& g = bw.base.g;
    const DivisionId defender = bw.defenders[0];

    int ticks = 0;
    do {
        phase_combat(g);
        ++ticks;
    } while (g.world.battles.size() > 0 && ticks < 2000);
    CHECK_EQ(g.world.battles.size(), 0u);
    CHECK(g.world.division(defender) == nullptr);  // no legal retreat -> destroyed
}

HOI_TEST(military_organization_recovery_scales_with_supply) {
    BaseWorld w;
    StateId st = add_state(w.g.world, "home", w.a);
    ProvinceId home = add_province(w.g.world, "home", Terrain::Plains, st, w.a, 0);

    DivisionId fed = add_division(w.g, w.a, w.infantry, home);
    DivisionId dry = add_division(w.g, w.a, w.infantry, home);
    for (DivisionId id : {fed, dry}) {
        Division& d = *w.g.world.division(id);
        d.organization = 0.0;
        d.moving = false;
        d.retreating = false;
    }
    w.g.world.division(fed)->supply = 1.0;
    w.g.world.division(dry)->supply = 0.2;

    phase_combat(w.g);

    CHECK_GT(w.g.world.division(fed)->organization, w.g.world.division(dry)->organization);
    CHECK_GT(w.g.world.division(fed)->entrenchment, 0.0);
}

HOI_TEST(military_armour_advantage_multiplies_damage) {
    // Attacker and defender use different equipment so the armour edge is
    // one-sided; with no enemy piercing the armour roll is deterministic.
    auto run = [](double attacker_armor) -> double {
        Game g;
        CountryId a = add_country(g.world, "AAA");
        CountryId b = add_country(g.world, "BBB");
        EquipmentId atk_eq = add_equipment(g, "atk", 6.0, 0.5, 8.0, 2.0, attacker_armor, 0.0,
                                           4.0, 25.0, 60.0, 0.2, 0.5, 1000.0);
        EquipmentId def_eq = add_equipment(g, "def", 6.0, 0.5, 8.0, 2.0, 0.0, 0.0, 4.0, 25.0,
                                           60.0, 0.2, 0.5, 1000.0);
        BattalionSlot sa;
        sa.equipment = atk_eq;
        sa.count = 1;
        BattalionSlot sd;
        sd.equipment = def_eq;
        sd.count = 1;
        TemplateId atk_t = add_template(g, "atk_inf", a, {sa});
        TemplateId def_t = add_template(g, "def_inf", b, {sd});
        add_war(g.world, a, b);
        StateId st_a = add_state(g.world, "sa", a);
        StateId st_b = add_state(g.world, "sb", b);
        ProvinceId p = add_province(g.world, "p", Terrain::Plains, st_b, b, 0);
        ProvinceId q = add_province(g.world, "q", Terrain::Plains, st_a, a, 0);
        link(g.world, p, q);
        ProvinceId r = add_province(g.world, "r", Terrain::Plains, st_b, b, 0);
        link(g.world, p, r);
        for (int i = 0; i < 3; ++i) {
            DivisionId id = add_division(g, a, atk_t, q);
            g.world.division(id)->order_target = p;
        }
        DivisionId def = add_division(g, b, def_t, p);
        g.rng.seed(999);
        phase_combat(g);
        const Division* d = g.world.division(def);
        return d ? d->max_organization - d->organization : 0.0;
    };

    const double plain = run(0.0);
    const double armoured = run(500.0);
    CHECK_GT(plain, 0.0);
    CHECK_NEAR(armoured / plain, 1.5, 1e-6);
}

HOI_TEST(military_territory_transfers_control_to_occupier) {
    BaseWorld w;
    add_war(w.g.world, w.a, w.b);
    StateId sb = add_state(w.g.world, "sb", w.b);
    ProvinceId p = add_province(w.g.world, "p", Terrain::Plains, sb, w.b, 0);
    w.g.world.state(sb)->provinces.push_back(p);

    add_division(w.g, w.a, w.infantry, p);
    phase_territory(w.g);

    CHECK_EQ(w.g.world.province(p)->controller, w.a);
    CHECK_EQ(w.g.world.state(sb)->controller, w.a);
}
