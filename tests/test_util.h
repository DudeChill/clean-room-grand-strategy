#pragma once
// Shared helpers for hand-built hermetic test worlds.
//
// Tests never depend on generated scenario files: they construct a tiny world with
// known geometry so failures point at the rule under test, not at data.

#include <string>
#include <vector>

#include "data/content.h"
#include "game/game.h"
#include "sim/commands.h"
#include "sim/map.h"
#include "sim/units.h"

namespace hoi_test {

using namespace hoi;

inline EquipmentId add_equipment(Content& c, const std::string& key, EquipmentCategory category,
                                 double soft_attack, double defense, double build_cost,
                                 double organization = 60.0, double hp = 25.0,
                                 double speed = 4.0, double hardness = 0.0) {
    EquipmentDef e;
    e.key = key;
    e.name = key;
    e.category = category;
    e.soft_attack = soft_attack;
    e.defense = defense;
    e.breakthrough = defense * 0.75;
    e.build_cost = build_cost;
    e.organization = organization;
    e.max_strength = hp;
    e.speed = speed;
    e.hardness = hardness;
    e.manpower = 100.0;
    e.supply_use = 0.5;
    const EquipmentId id(static_cast<uint32_t>(c.equipment.size()));
    e.id = id;  // content definitions carry their own id; nothing else may guess it
    c.equipment.push_back(e);
    c.equipment_by_key[key] = id;
    return id;
}

// Content with two equipment models, one template and the building/law definitions
// the command layer needs. No files involved.
inline Content make_test_content() {
    Content c;
    add_equipment(c, "infantry_equipment_1", EquipmentCategory::Infantry, 6.0, 22.0, 0.7);
    add_equipment(c, "artillery_1", EquipmentCategory::Artillery, 25.0, 12.0, 4.0, 0.0, 2.0, 0.0);
    add_equipment(c, "support_equipment_1", EquipmentCategory::Support, 0.5, 2.0, 2.0, 0.0, 1.0,
                  0.0);

    DivisionTemplate t;
    t.key = "infantry_template";
    t.name = "Infantry";
    t.country = CountryId{};
    BattalionSlot b;
    b.equipment = c.equipment_by_key["infantry_equipment_1"];
    b.count = 6;
    t.battalions.push_back(b);
    t.train_days = 10.0;
    recompute_template_stats(t, c.equipment);
    c.template_by_key[t.key] = TemplateId(static_cast<uint32_t>(c.templates.size()));
    c.templates.push_back(t);

    for (int kind = 0; kind < 3; ++kind) {
        BuildingDef bd;
        bd.kind = kind == 0 ? BuildingKind::CivilianFactory
                            : (kind == 1 ? BuildingKind::MilitaryFactory : BuildingKind::Dockyard);
        bd.key = building_kind_name(bd.kind);
        bd.name = bd.key;
        bd.base_cost = 10800.0;
        bd.max_level = 10;
        c.buildings.push_back(bd);
    }

    auto add_law = [&](const std::string& key, int law_kind, int level, double cost) {
        LawDef l;
        l.key = key;
        l.name = key;
        l.kind = law_kind;
        l.level = level;
        l.cost = cost;
        c.law_index[key] = static_cast<int>(c.laws.size());
        c.laws.push_back(l);
    };
    add_law("conscription_limited", 0, 0, 0.0);
    add_law("conscription_service", 0, 1, 100.0);
    add_law("economy_civilian", 1, 0, 0.0);
    add_law("economy_partial", 1, 1, 100.0);
    add_law("trade_free", 2, 0, 0.0);
    add_law("trade_export_focus", 2, 1, 50.0);
    return c;
}

inline ProvinceId add_province(World& w, const std::string& name, StateId state, RegionId region,
                               Terrain terrain = Terrain::Plains) {
    Province p;
    p.name = name;
    p.state = state;
    p.region = region;
    p.terrain = terrain;
    p.infrastructure = 5;
    p.population = 100000.0;
    const ProvinceId id = w.provinces.create(p);
    w.provinces[id].id = id;
    return id;
}

inline void link_provinces(World& w, ProvinceId a, ProvinceId b, bool sea = false) {
    Province* pa = w.provinces.try_get(a);
    Province* pb = w.provinces.try_get(b);
    if (!pa || !pb) return;
    auto& vec_a = sea ? pa->sea_adj : pa->adj;
    auto& vec_b = sea ? pb->sea_adj : pb->adj;
    vec_a.push_back(b);
    vec_b.push_back(a);
}

inline DivisionId add_division(Game& g, CountryId country, ProvinceId province, TemplateId tmpl) {
    const DivisionTemplate* t = g.content.template_def(tmpl);
    Division d;
    d.country = country;
    d.template_id = tmpl;
    d.location = province;
    d.previous_location = province;
    d.max_organization = t ? t->max_organization : 60.0;
    d.organization = d.max_organization;
    d.strength = 1.0;
    d.manpower = t ? t->manpower : 600.0;
    d.equipment.assign(g.content.equipment.size(), 0.0);
    if (t) {
        for (const auto& slot : t->battalions) {
            if (slot.equipment.valid() && slot.equipment.v < d.equipment.size()) {
                d.equipment[slot.equipment.v] += static_cast<double>(slot.count);
            }
        }
    }
    d.created_tick = g.world.tick;
    const DivisionId id = g.world.divisions.create(d);
    Country* c = g.world.countries.try_get(country);
    if (c) c->divisions.push_back(id);
    return id;
}

// Two-country world:
//   provinces p0 p1 p2 belong to country A (VLA), p3 p4 p5 to country B (NOR)
//   p0-p1-p2-p3-p4-p5 form a line, so the frontier is p2|p3.
struct MiniWorld {
    Game game;
    CountryId a;
    CountryId b;
    StateId state_a;
    StateId state_b;
    RegionId region;
    std::vector<ProvinceId> provinces;  // p0..p5
    TemplateId tmpl;
};

inline MiniWorld make_mini_world(bool at_war = false, int province_count = 6) {
    MiniWorld m;
    m.game.content = make_test_content();
    Game& g = m.game;
    World& w = g.world;
    w.tick = 0;
    w.date = GameDate{1936, 1, 1, 0};

    Region r;
    r.name = "region";
    m.region = w.regions.create(r);
    w.regions[m.region].id = m.region;

    State sa;
    sa.name = "state_a";
    sa.building_slots = 20;
    m.state_a = w.states.create(sa);
    w.states[m.state_a].id = m.state_a;
    State sb;
    sb.name = "state_b";
    sb.building_slots = 20;
    m.state_b = w.states.create(sb);
    w.states[m.state_b].id = m.state_b;

    Country ca;
    ca.tag = "VLA";
    ca.name = "Valtia";
    ca.capital = m.state_a;
    ca.manpower = 500000.0;
    ca.political_power = 500.0;
    ca.equipment_stockpile.assign(g.content.equipment.size(), 10000.0);
    ca.law_levels.assign(4, 0);
    ca.research.slots.resize(3);
    ca.research.slots_unlocked = 3;
    ca.starting_factories = 10;
    m.a = w.countries.create(ca);
    w.countries[m.a].id = m.a;

    Country cb;
    cb.tag = "NOR";
    cb.name = "Norlund";
    cb.capital = m.state_b;
    cb.manpower = 500000.0;
    cb.political_power = 500.0;
    cb.equipment_stockpile.assign(g.content.equipment.size(), 10000.0);
    cb.law_levels.assign(4, 0);
    cb.research.slots.resize(3);
    cb.research.slots_unlocked = 3;
    cb.starting_factories = 10;
    m.b = w.countries.create(cb);
    w.countries[m.b].id = m.b;

    w.states[m.state_a].owner = m.a;
    w.states[m.state_a].controller = m.a;
    w.states[m.state_a].civilian_factories = 5;
    w.states[m.state_a].military_factories = 5;
    w.states[m.state_b].owner = m.b;
    w.states[m.state_b].controller = m.b;
    w.states[m.state_b].civilian_factories = 5;
    w.states[m.state_b].military_factories = 5;

    const int half = province_count / 2;
    m.provinces.reserve(static_cast<size_t>(province_count));
    for (int i = 0; i < province_count; ++i) {
        const bool side_a = i < half;
        ProvinceId pid = add_province(w, "p" + std::to_string(i),
                                      side_a ? m.state_a : m.state_b, m.region);
        Province& p = w.provinces[pid];
        p.owner = side_a ? m.a : m.b;
        p.controller = side_a ? m.a : m.b;
        p.supply_hub = (i == 0 || i == half);
        p.population = 200000.0;
        (side_a ? w.states[m.state_a].provinces : w.states[m.state_b].provinces).push_back(pid);
        m.provinces.push_back(pid);
    }
    for (int i = 0; i + 1 < province_count; ++i) {
        link_provinces(w, m.provinces[static_cast<size_t>(i)],
                       m.provinces[static_cast<size_t>(i + 1)]);
    }

    m.tmpl = g.content.template_id("infantry_template");
    g.content.templates[m.tmpl.v].country = m.a;
    DivisionTemplate second = g.content.templates[m.tmpl.v];
    second.country = m.b;
    second.key = "infantry_template_b";
    const TemplateId tmpl_b(static_cast<uint32_t>(g.content.templates.size()));
    g.content.templates.push_back(second);
    g.content.template_by_key[second.key] = tmpl_b;

    g.ai_controlled.assign(w.countries.capacity(), 0);
    if (at_war) {
        std::vector<WarGoal> goals;
        WarGoal goal;
        goal.claimant = m.a;
        goal.target = m.b;
        goals.push_back(goal);
        War w2;
        w2.id = WarId{};
        w2.aggressor = m.a;
        WarParticipant pa;
        pa.country = m.a;
        WarParticipant pb;
        pb.country = m.b;
        w2.attackers.push_back(pa);
        w2.defenders.push_back(pb);
        w2.goals = goals;
        w2.start_tick = 0;
        const WarId wid = w.wars.create(w2);
        w.wars[wid].id = wid;
        w.countries[m.a].wars.push_back(wid);
        w.countries[m.b].wars.push_back(wid);
        w.countries[m.a].at_war = true;
        w.countries[m.b].at_war = true;
        Relation& rel = w.relation(m.a, m.b);
        rel.at_war = true;
        rel.value = -100.0;
    }
    return m;
}

}  // namespace hoi_test
