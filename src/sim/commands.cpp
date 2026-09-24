// Command validation and application.
//
// Design rules:
//  * validate_command is pure - it must never mutate state, so the AI can probe
//    whether an action is legal without side effects.
//  * apply_command performs exactly the mutation the command describes and is only
//    called after validation returned Applied (phase_commands does both).
//  * Every command is serializable, so the command log plus the initial state and
//    seed reproduces a run exactly.

#include "sim/commands.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/air.h"
#include "sim/combat.h"
#include "sim/diplomacy.h"
#include "sim/events.h"
#include "sim/focus.h"
#include "sim/industry.h"
#include "sim/map.h"
#include "sim/navy.h"
#include "sim/phases.h"
#include "sim/politics.h"
#include "sim/research.h"
#include "sim/units.h"

namespace hoi {

namespace {

constexpr int TRADE_LAW_KIND = 2;  // law kind index used by SetTradePolicy

bool is_state_wide(BuildingKind kind) {
    switch (kind) {
        case BuildingKind::CivilianFactory:
        case BuildingKind::MilitaryFactory:
        case BuildingKind::Dockyard:
        case BuildingKind::SyntheticRefinery:
            return true;
        default:
            return false;
    }
}

const BuildingDef* find_building(const Content& content, BuildingKind kind) {
    for (const auto& b : content.buildings) {
        if (b.kind == kind) return &b;
    }
    return nullptr;
}

int state_used_slots(const State& s) {
    return s.civilian_factories + s.military_factories + s.dockyards;
}

int current_building_level(const World& w, BuildingKind kind, StateId state, ProvinceId province) {
    if (is_state_wide(kind)) {
        const State* s = w.state(state);
        if (!s) return 0;
        switch (kind) {
            case BuildingKind::CivilianFactory: return s->civilian_factories;
            case BuildingKind::MilitaryFactory: return s->military_factories;
            case BuildingKind::Dockyard: return s->dockyards;
            case BuildingKind::SyntheticRefinery: return s->synthetic_refineries;
            default: return 0;
        }
    }
    const Province* p = w.province(province);
    if (!p) return 0;
    switch (kind) {
        case BuildingKind::Infrastructure: return p->infrastructure;
        case BuildingKind::Railway: return p->railway_level;
        case BuildingKind::SupplyHub: return p->supply_hub ? 1 : 0;
        case BuildingKind::AirBase: return p->air_base;
        case BuildingKind::NavalBase: return p->naval_base;
        case BuildingKind::Radar: return p->radar;
        case BuildingKind::AntiAir: return p->anti_air;
        case BuildingKind::Fort: return p->fort_level;
        default: return 0;
    }
}

double building_cost(const Content& content, BuildingKind kind, int existing_level) {
    const SimConstants& k = content.constants;
    double base = 0.0;
    // Data wins when it defines a cost; the constants table is the fallback so that
    // a mod can retune costs without touching the engine.
    if (const BuildingDef* def = find_building(content, kind); def && def->base_cost > 0.0) {
        base = def->base_cost;
    } else {
        switch (kind) {
            case BuildingKind::CivilianFactory:
            case BuildingKind::MilitaryFactory:
            case BuildingKind::Dockyard: base = k.construction_cost_factory; break;
            case BuildingKind::Infrastructure: base = k.construction_cost_infrastructure; break;
            case BuildingKind::Railway: base = k.construction_cost_railway; break;
            case BuildingKind::SupplyHub: base = k.construction_cost_supply_hub; break;
            case BuildingKind::AirBase: base = k.construction_cost_air_base; break;
            case BuildingKind::NavalBase: base = k.construction_cost_naval_base; break;
            case BuildingKind::Fort: base = k.construction_cost_fort; break;
            case BuildingKind::Radar: base = k.construction_cost_radar; break;
            case BuildingKind::SyntheticRefinery: base = k.construction_cost_synthetic; break;
            default: base = 4000.0; break;
        }
    }
    // Cost grows linearly with the number of levels already present:
    // cost = base * (1 + (level_scaling - 1) * existing_level). An exponential
    // curve made late levels in a large state unreachable for every AI at once.
    const double scale =
        1.0 + (k.construction_level_scaling - 1.0) * static_cast<double>(existing_level);
    return base * scale;
}

bool owns_army(const World& w, const Command& cmd, const Army** out) {
    const Army* a = w.army(cmd.army);
    if (!a || a->country != cmd.country) return false;
    if (out) *out = a;
    return true;
}

bool valid_stance(int v) { return v >= 0 && v <= 2; }

// Retired lines keep their identity so switching equipment back is rewarded. The
// efficiency/cap retention math is owned by phase_industry (single owner): commands
// only records the switch by pushing the previous model onto `previous`, which
// industry consumes as a pending-switch marker.
ProductionLine* find_retired_line(Country& c, const Content& content, EquipmentId target) {
    const EquipmentDef* target_def = content.equipment_def(target);
    ProductionLine* same_archetype = nullptr;
    ProductionLine* any = nullptr;
    for (auto& line : c.lines) {
        if (line.factories != 0 || line.equipment.valid()) continue;
        if (!any) any = &line;
        for (EquipmentId prev : line.previous) {
            const EquipmentDef* pd = content.equipment_def(prev);
            if (pd && target_def && pd->archetype == target_def->archetype) {
                same_archetype = &line;
                break;
            }
        }
        if (same_archetype) break;
    }
    return same_archetype ? same_archetype : any;
}

void mark_switch(ProductionLine& line, EquipmentId from) {
    if (!from.valid()) return;
    line.previous.insert(line.previous.begin(), from);
    if (line.previous.size() > 8) line.previous.resize(8);
}

CommandResult do_set_production_line(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    const SimConstants& k = g.content.constants;

    ProductionLine* existing = nullptr;
    for (auto& line : c.lines) {
        if (line.equipment == cmd.equipment && line.factories > 0) existing = &line;
    }

    if (cmd.value == 0) {
        if (existing) {
            mark_switch(*existing, existing->equipment);
            existing->equipment = EquipmentId{};
            existing->factories = 0;
        }
        return CommandResult::Applied;
    }

    const FactoryPool pool = equipment_factory_pool(g.content, cmd.equipment);
    int others = 0;
    for (const auto& line : c.lines) {
        if (&line == existing) continue;
        if (line_factory_pool(g.content, line) != pool) continue;
        others += line.factories;
    }
    int mil = 0, civ = 0, dock = 0;
    count_factories(g.world, cmd.country, &civ, &mil, &dock);
    const int available = pool == FactoryPool::Dockyard ? dock : mil;
    if (others + cmd.value > available) return CommandResult::InsufficientResources;

    if (existing) {
        existing->factories = cmd.value;
        return CommandResult::Applied;
    }

    if (ProductionLine* retired = find_retired_line(c, g.content, cmd.equipment)) {
        // Reviving a retired line: industry applies the documented retention rule
        // when it sees previous.front() != equipment.
        if (retired->previous.empty()) {
            retired->efficiency = k.efficiency_start;
            retired->efficiency_cap = k.efficiency_cap_base;
        }
        retired->equipment = cmd.equipment;
        retired->factories = cmd.value;
        retired->output_today = 0.0;
        retired->started = g.world.tick;
        return CommandResult::Applied;
    }

    ProductionLine line;
    line.equipment = cmd.equipment;
    line.factories = cmd.value;
    line.efficiency = k.efficiency_start;
    line.efficiency_cap = k.efficiency_cap_base;
    line.started = g.world.tick;
    c.lines.push_back(line);
    return CommandResult::Applied;
}

CommandResult do_start_construction(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    const BuildingDef* def = find_building(g.content, static_cast<BuildingKind>(cmd.value));
    if (!def) return CommandResult::PrerequisitesMissing;
    if (static_cast<int>(c.construction.queue.size()) >= c.construction.max_queue_size) {
        return CommandResult::QueueFull;
    }

    ConstructionProject project;
    project.kind = static_cast<BuildingKind>(cmd.value);
    project.province = is_state_wide(project.kind) ? ProvinceId{} : cmd.province;
    project.state = cmd.state;
    const int level = current_building_level(g.world, project.kind, cmd.state, cmd.province);
    project.target_level = level + 1;
    project.cost = building_cost(g.content, project.kind, level);
    project.progress = 0.0;
    c.construction.queue.push_back(project);
    return CommandResult::Applied;
}

CommandResult do_cancel_construction(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    auto& q = c.construction.queue;
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->kind == static_cast<BuildingKind>(cmd.value) && it->state == cmd.state &&
            (is_state_wide(it->kind) || it->province == cmd.province)) {
            q.erase(it);
            return CommandResult::Applied;
        }
    }
    return CommandResult::UnknownEntity;
}

CommandResult do_start_research(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    for (auto& slot : c.research.slots) {
        if (!slot.active) {
            slot.active = true;
            slot.tech = cmd.tech;
            slot.progress = 0.0;
            return CommandResult::Applied;
        }
    }
    return CommandResult::SlotUnavailable;
}

CommandResult do_cancel_research(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    for (auto& slot : c.research.slots) {
        if (slot.active && slot.tech == cmd.tech) {
            slot.active = false;
            slot.tech = TechId{};
            slot.progress = 0.0;
            return CommandResult::Applied;
        }
    }
    return CommandResult::UnknownEntity;
}

CommandResult do_create_template(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    DivisionTemplate t;
    t.key = cmd.text;
    t.name = cmd.text;
    t.country = cmd.country;
    t.battalions = cmd.battalions;
    const double train_days = cmd.value > 0 ? static_cast<double>(cmd.value) : 90.0;
    t.train_days = train_days;
    recompute_template_stats(t, g.content.equipment);
    TemplateId id(static_cast<uint32_t>(g.content.templates.size()));
    g.content.templates.push_back(t);
    g.content.template_by_key[t.key] = id;
    c.templates.push_back(id);
    return CommandResult::Applied;
}

CommandResult do_edit_template(Game& g, const Command& cmd) {
    DivisionTemplate& t = g.content.templates[cmd.template_id.v];
    t.battalions = cmd.battalions;
    recompute_template_stats(t, g.content.equipment);
    return CommandResult::Applied;
}

CommandResult do_recruit_division(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    const DivisionTemplate* t = g.content.template_def(cmd.template_id);
    const int count = cmd.value;
    for (int i = 0; i < count; ++i) {
        Division d;
        d.country = cmd.country;
        d.template_id = cmd.template_id;
        d.name = t->name + " #" + std::to_string(c.divisions.size() + 1);
        d.location = ProvinceId{};  // off-map until deployed
        d.previous_location = ProvinceId{};
        d.organization = 0.0;
        d.max_organization = t->max_organization;
        d.strength = 0.0;
        d.manpower = 0.0;
        d.equipment.assign(g.content.equipment.size(), 0.0);
        d.training_days_left = t->train_days;
        d.created_tick = g.world.tick;
        DivisionId id = g.world.divisions.create(d);
        c.divisions.push_back(id);
        c.training.push_back(TrainingDivision{id, cmd.template_id, t->train_days});
    }
    c.manpower -= t->manpower * count;
    if (c.manpower < 0.0) c.manpower = 0.0;
    return CommandResult::Applied;
}

CommandResult do_deploy_division(Game& g, const Command& cmd) {
    Division& d = g.world.divisions[cmd.division];
    d.location = cmd.province;
    d.previous_location = cmd.province;
    d.training_days_left = 0.0;
    Country& c = g.world.countries[cmd.country];
    for (auto it = c.training.begin(); it != c.training.end(); ++it) {
        if (it->division == cmd.division) {
            c.training.erase(it);
            break;
        }
    }
    g.log_event("deploy", "division deployed", cmd.country);
    return CommandResult::Applied;
}

CommandResult do_move_division(Game& g, const Command& cmd) {
    Division& d = g.world.divisions[cmd.division];
    PathRequest req;
    req.country = d.country;
    req.require_controlled = false;
    req.allow_hostile = g.world.at_war(d.country, g.world.province_controller(cmd.province));
    std::vector<ProvinceId> path = find_path(g.world, d.location, cmd.province, req);
    if (path.empty()) return CommandResult::InvalidTarget;
    d.path = path;
    d.moving = true;
    d.move_from = d.location;
    d.move_to = path.front();
    d.move_progress = 0.0;
    d.order = OrderKind::None;
    d.order_target = cmd.province;
    return CommandResult::Applied;
}

CommandResult do_set_division_order(Game& g, const Command& cmd) {
    Army& a = g.world.armies[cmd.army];
    const OrderKind kind = static_cast<OrderKind>(cmd.value);
    a.order.kind = kind;
    a.order.started = g.world.tick;
    a.order.progress = 0.0;
    a.order.line.clear();
    a.order.target_line.clear();

    switch (kind) {
        case OrderKind::FrontLine: {
            a.order.line = compute_front_line(g.world, a.country);
            break;
        }
        case OrderKind::Offensive: {
            a.order.line = compute_front_line(g.world, a.country);
            if (cmd.province.valid()) {
                a.order.target_line.push_back(cmd.province);
            } else {
                // Target the enemy-controlled provinces adjacent to the front.
                for (ProvinceId p : a.order.line) {
                    const Province* prov = g.world.province(p);
                    if (!prov) continue;
                    for (ProvinceId n : prov->adj) {
                        CountryId ctl = g.world.province_controller(n);
                        if (ctl.valid() && g.world.at_war(a.country, ctl)) {
                            a.order.target_line.push_back(n);
                        }
                    }
                }
            }
            break;
        }
        case OrderKind::Fallback: {
            a.order.line = front_reserve_provinces(g.world, a.country,
                                                   compute_front_line(g.world, a.country));
            break;
        }
        case OrderKind::Garrison: {
            g.world.provinces.for_each([&](ProvinceId pid, const Province& p) {
                if (p.controller == a.country && p.victory_points > 0) a.order.line.push_back(pid);
            });
            break;
        }
        default: break;
    }

    for (DivisionId did : a.divisions) {
        Division* d = g.world.divisions.try_get(did);
        if (!d) continue;
        d->order = kind;
        d->planning = 0.0;
        if (kind == OrderKind::Offensive && !a.order.target_line.empty()) {
            d->order_target = a.order.target_line.front();
        }
    }
    return CommandResult::Applied;
}

CommandResult do_create_army(Game& g, const Command& cmd) {
    Army a;
    a.country = cmd.country;
    a.name = cmd.text;
    ArmyId id = g.world.armies.create(a);
    g.world.armies[id].id = id;
    g.world.countries[cmd.country].armies.push_back(id);
    g.log_event("army", "army '" + a.name + "' formed", cmd.country);
    return CommandResult::Applied;
}

CommandResult do_assign_division_to_army(Game& g, const Command& cmd) {
    Army& a = g.world.armies[cmd.army];
    for (DivisionId did : cmd.divisions) {
        Division* d = g.world.divisions.try_get(did);
        if (!d) continue;
        if (d->army.valid() && d->army != cmd.army) {
            Army* old = g.world.armies.try_get(d->army);
            if (old) {
                old->divisions.erase(std::remove(old->divisions.begin(), old->divisions.end(), did),
                                     old->divisions.end());
            }
        }
        d->army = cmd.army;
        if (std::find(a.divisions.begin(), a.divisions.end(), did) == a.divisions.end()) {
            a.divisions.push_back(did);
        }
    }
    return CommandResult::Applied;
}

CommandResult do_assign_general(Game& g, const Command& cmd) {
    Army& a = g.world.armies[cmd.army];
    if (a.general.valid()) {
        Character* old = g.world.characters.try_get(a.general);
        if (old) old->army = ArmyId{};
    }
    a.general = cmd.character;
    Character* ch = g.world.characters.try_get(cmd.character);
    if (ch) ch->army = cmd.army;
    return CommandResult::Applied;
}

CommandResult do_declare_war(Game& g, const Command& cmd) {
    std::vector<WarGoal> goals;
    WarGoal goal;
    goal.claimant = cmd.country;
    goal.target = cmd.target_country;
    goal.state = cmd.state;
    goal.annex_country = (cmd.value & 1) != 0;
    goal.puppet = (cmd.value & 2) != 0;
    goals.push_back(goal);
    WarId war = declare_war(g, cmd.country, cmd.target_country, goals);
    return war.valid() ? CommandResult::Applied : CommandResult::InvalidTarget;
}

CommandResult do_offer_peace(Game& g, const Command& cmd) {
    return offer_peace(g, cmd.war, cmd.country) ? CommandResult::Applied
                                                : CommandResult::PrerequisitesMissing;
}

CommandResult do_set_law(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    const LawDef* law = g.content.law(cmd.text);
    if (!law) return CommandResult::UnknownEntity;
    if (c.political_power < law->cost) return CommandResult::InsufficientResources;
    apply_law_change(g, c, law->kind, law->level);
    c.political_power -= law->cost;
    return CommandResult::Applied;
}

CommandResult do_set_trade_policy(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    for (const auto& law : g.content.laws) {
        if (law.kind != TRADE_LAW_KIND || law.level != cmd.value) continue;
        if (c.political_power < law.cost) return CommandResult::InsufficientResources;
        apply_law_change(g, c, law.kind, law.level);
        c.political_power -= law.cost;
        return CommandResult::Applied;
    }
    return CommandResult::InvalidValue;
}

CommandResult do_set_stance(Game& g, const Command& cmd) {
    g.world.armies[cmd.army].stance = static_cast<uint8_t>(cmd.value);
    return CommandResult::Applied;
}

CommandResult do_motorize_supply(Game& g, const Command& cmd) {
    g.world.armies[cmd.army].motorization = cmd.value;
    return CommandResult::Applied;
}

CommandResult do_create_air_wing(Game& g, const Command& cmd) {
    World& w = g.world;
    Country& c = w.countries[cmd.country];
    const EquipmentDef* def = g.content.equipment_def(cmd.equipment);
    AirWing wing;
    wing.country = cmd.country;
    wing.equipment = cmd.equipment;
    wing.base = cmd.province;
    const Province* base = w.province(cmd.province);
    wing.region = base ? base->region : RegionId{};
    wing.max_planes = std::max(1, cmd.value);
    wing.planes = 0;  // filled from the stockpile immediately below
    wing.mission = AirMission::AirSuperiority;
    wing.name = std::string(def ? def->name : "Air") + " wing " + std::to_string(c.wings.size() + 1);
    const AirWingId id = w.air_wings.create(wing);
    AirWing* created = w.air_wings.try_get(id);
    if (!created) return CommandResult::QueueFull;
    c.wings.push_back(id);
    reinforce_air_wing(g, *created);
    g.log_event("air", "wing '" + created->name + "' formed", cmd.country);
    return CommandResult::Applied;
}

CommandResult do_deploy_air_wing(Game& g, const Command& cmd) {
    AirWing* wing = g.world.wing(cmd.wing);
    if (!wing) return CommandResult::UnknownEntity;
    wing->base = cmd.province;
    const Province* p = g.world.province(cmd.province);
    if (p) wing->region = p->region;
    return CommandResult::Applied;
}

CommandResult do_set_air_mission(Game& g, const Command& cmd) {
    AirWing* wing = g.world.wing(cmd.wing);
    if (!wing) return CommandResult::UnknownEntity;
    wing->mission = static_cast<AirMission>(cmd.value);
    wing->region = cmd.region;
    return CommandResult::Applied;
}

CommandResult do_disband_air_wing(Game& g, const Command& cmd) {
    disband_air_wing(g, cmd.wing);
    return CommandResult::Applied;
}

CommandResult do_select_focus(Game& g, const Command& cmd) {
    const uint32_t focus = g.content.focus_id(cmd.text);
    if (focus == INVALID_FOCUS) return CommandResult::UnknownEntity;
    return focus_select(g, cmd.country, focus) ? CommandResult::Applied
                                               : CommandResult::PrerequisitesMissing;
}

CommandResult do_cancel_focus(Game& g, const Command& cmd) {
    focus_cancel(g, cmd.country);
    return CommandResult::Applied;
}

CommandResult do_choose_event_option(Game& g, const Command& cmd) {
    const uint32_t event = g.content.event_id(cmd.text);
    if (event == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
    return choose_event_option(g, cmd.country, event, cmd.value) ? CommandResult::Applied
                                                                 : CommandResult::InvalidValue;
}

CommandResult do_take_decision(Game& g, const Command& cmd) {
    const uint32_t decision = g.content.decision_id(cmd.text);
    if (decision == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
    return decision_take(g, cmd.country, decision) ? CommandResult::Applied
                                                   : CommandResult::PrerequisitesMissing;
}

CommandResult do_cancel_decision(Game& g, const Command& cmd) {
    const uint32_t decision = g.content.decision_id(cmd.text);
    if (decision == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
    decision_cancel(g, cmd.country, decision);
    return CommandResult::Applied;
}

CommandResult do_create_fleet(Game& g, const Command& cmd) {
    Country& c = g.world.countries[cmd.country];
    Fleet fleet;
    fleet.country = cmd.country;
    fleet.name = cmd.text.empty() ? ("Fleet " + std::to_string(c.fleets.size() + 1)) : cmd.text;
    const FleetId id = g.world.fleets.create(fleet);
    c.fleets.push_back(id);
    g.log_event("navy", "fleet '" + fleet.name + "' formed", cmd.country);
    return CommandResult::Applied;
}

CommandResult do_create_task_force(Game& g, const Command& cmd) {
    const TaskForceId id =
        form_task_force(g, cmd.country, cmd.province, cmd.equipment, cmd.value, cmd.text);
    return id.valid() ? CommandResult::Applied : CommandResult::InsufficientResources;
}

CommandResult do_set_naval_mission(Game& g, const Command& cmd) {
    TaskForce* tf = g.world.task_force(cmd.task_force);
    if (!tf) return CommandResult::UnknownEntity;
    tf->mission = static_cast<NavalMission>(cmd.value);
    tf->sea_region = cmd.region;
    return CommandResult::Applied;
}

CommandResult do_assign_ship(Game& g, const Command& cmd) {
    Ship* ship = g.world.ship(cmd.ship_id);
    TaskForce* tf = g.world.task_force(cmd.task_force);
    if (!ship || !tf) return CommandResult::UnknownEntity;
    TaskForce* old = g.world.task_force(ship->task_force);
    if (old && old->id != tf->id) {
        old->ships.erase(std::remove(old->ships.begin(), old->ships.end(), ship->id),
                         old->ships.end());
    }
    if (std::find(tf->ships.begin(), tf->ships.end(), ship->id) == tf->ships.end()) {
        tf->ships.push_back(ship->id);
    }
    ship->task_force = tf->id;
    ship->fleet = tf->fleet;
    return CommandResult::Applied;
}

CommandResult do_launch_invasion(Game& g, const Command& cmd) {
    return start_naval_invasion(g, cmd.army, cmd.province, cmd.province_b)
               ? CommandResult::Applied
               : CommandResult::InvalidTarget;
}

CommandResult do_cancel_invasion(Game& g, const Command& cmd) {
    cancel_naval_invasion(g, cmd.army);
    return CommandResult::Applied;
}

}  // namespace

const char* command_type_name(CommandType t) {
    switch (t) {
        case CommandType::None: return "none";
        case CommandType::SetProductionLine: return "set_production_line";
        case CommandType::RemoveProductionLine: return "remove_production_line";
        case CommandType::StartConstruction: return "start_construction";
        case CommandType::CancelConstruction: return "cancel_construction";
        case CommandType::StartResearch: return "start_research";
        case CommandType::CancelResearch: return "cancel_research";
        case CommandType::CreateTemplate: return "create_template";
        case CommandType::EditTemplate: return "edit_template";
        case CommandType::RecruitDivision: return "recruit_division";
        case CommandType::DeployDivision: return "deploy_division";
        case CommandType::MoveDivision: return "move_division";
        case CommandType::SetDivisionOrder: return "set_division_order";
        case CommandType::CreateArmy: return "create_army";
        case CommandType::AssignDivisionToArmy: return "assign_division_to_army";
        case CommandType::AssignGeneral: return "assign_general";
        case CommandType::DeclareWar: return "declare_war";
        case CommandType::OfferPeace: return "offer_peace";
        case CommandType::JoinFaction: return "join_faction";
        case CommandType::LeaveFaction: return "leave_faction";
        case CommandType::CreateAirWing: return "create_air_wing";
        case CommandType::DeployAirWing: return "deploy_air_wing";
        case CommandType::SetAirMission: return "set_air_mission";
        case CommandType::DisbandAirWing: return "disband_air_wing";
        case CommandType::CreateFleet: return "create_fleet";
        case CommandType::CreateTaskForce: return "create_task_force";
        case CommandType::SetNavalMission: return "set_naval_mission";
        case CommandType::AssignShipToTaskForce: return "assign_ship_to_task_force";
        case CommandType::LaunchNavalInvasion: return "launch_naval_invasion";
        case CommandType::CancelNavalInvasion: return "cancel_naval_invasion";
        case CommandType::SelectFocus: return "select_focus";
        case CommandType::CancelFocus: return "cancel_focus";
        case CommandType::ChooseEventOption: return "choose_event_option";
        case CommandType::TakeDecision: return "take_decision";
        case CommandType::CancelDecision: return "cancel_decision";
        case CommandType::SetLaw: return "set_law";
        case CommandType::SetTradePolicy: return "set_trade_policy";
        case CommandType::SetStance: return "set_stance";
        case CommandType::MotorizeSupply: return "motorize_supply";
        case CommandType::ToggleFuelPriority: return "toggle_fuel_priority";
        default: return "unknown";
    }
}

const char* command_result_name(CommandResult r) {
    switch (r) {
        case CommandResult::Applied: return "applied";
        case CommandResult::InvalidType: return "invalid_type";
        case CommandResult::UnknownEntity: return "unknown_entity";
        case CommandResult::NotOwner: return "not_owner";
        case CommandResult::InsufficientResources: return "insufficient_resources";
        case CommandResult::PrerequisitesMissing: return "prerequisites_missing";
        case CommandResult::InvalidTarget: return "invalid_target";
        case CommandResult::QueueFull: return "queue_full";
        case CommandResult::SlotUnavailable: return "slot_unavailable";
        case CommandResult::AlreadyAtWar: return "already_at_war";
        case CommandResult::AtWar: return "at_war";
        case CommandResult::InvalidValue: return "invalid_value";
        default: return "unknown";
    }
}

CommandResult validate_command(const Game& g, const Command& cmd) {
    const Country* c = g.world.country(cmd.country);
    if (!c || !c->alive) return CommandResult::UnknownEntity;

    switch (cmd.type) {
        case CommandType::SetProductionLine: {
            if (!g.content.equipment_def(cmd.equipment)) return CommandResult::UnknownEntity;
            if (cmd.value < 0) return CommandResult::InvalidValue;
            if (!equipment_unlocked(g, cmd.country, cmd.equipment)) {
                return CommandResult::PrerequisitesMissing;
            }
            // The factory budget is enforced here as well as in application, so a
            // validated command is always one that changes state. Ship and convoy
            // lines draw on dockyards; everything else draws on military factories.
            const FactoryPool pool = equipment_factory_pool(g.content, cmd.equipment);
            int others = 0;
            for (const auto& line : c->lines) {
                if (line.equipment == cmd.equipment && line.factories > 0) continue;
                if (line_factory_pool(g.content, line) != pool) continue;
                others += line.factories;
            }
            int civ = 0, mil = 0, dock = 0;
            count_factories(g.world, cmd.country, &civ, &mil, &dock);
            const int available = pool == FactoryPool::Dockyard ? dock : mil;
            if (others + cmd.value > available) return CommandResult::InsufficientResources;
            return CommandResult::Applied;
        }
        case CommandType::RemoveProductionLine: {
            if (!g.content.equipment_def(cmd.equipment)) return CommandResult::UnknownEntity;
            for (const auto& line : c->lines) {
                if (line.equipment == cmd.equipment && line.factories > 0) return CommandResult::Applied;
            }
            return CommandResult::UnknownEntity;
        }
        case CommandType::StartConstruction: {
            if (cmd.value < 0 || cmd.value >= static_cast<int>(BuildingKind::Count)) {
                return CommandResult::InvalidValue;
            }
            const BuildingKind kind = static_cast<BuildingKind>(cmd.value);
            const BuildingDef* def = find_building(g.content, kind);
            if (!def) return CommandResult::PrerequisitesMissing;
            if (!building_unlocked(g, cmd.country, def->key)) return CommandResult::PrerequisitesMissing;
            if (static_cast<int>(c->construction.queue.size()) >= c->construction.max_queue_size) {
                return CommandResult::QueueFull;
            }
            if (is_state_wide(kind)) {
                const State* s = g.world.state(cmd.state);
                if (!s || s->controller != cmd.country) return CommandResult::NotOwner;
                if (state_used_slots(*s) >= s->building_slots) return CommandResult::QueueFull;
                const int level = current_building_level(g.world, kind, cmd.state, ProvinceId{});
                if (level >= def->max_level) return CommandResult::InvalidValue;
            } else {
                const Province* p = g.world.province(cmd.province);
                if (!p || p->controller != cmd.country) return CommandResult::NotOwner;
                if (p->is_sea) return CommandResult::InvalidTarget;
                const int level = current_building_level(g.world, kind, StateId{}, cmd.province);
                if (level >= def->max_level) return CommandResult::InvalidValue;
            }
            return CommandResult::Applied;
        }
        case CommandType::CancelConstruction: {
            if (cmd.value < 0 || cmd.value >= static_cast<int>(BuildingKind::Count)) {
                return CommandResult::InvalidValue;
            }
            const BuildingKind kind = static_cast<BuildingKind>(cmd.value);
            for (const auto& project : c->construction.queue) {
                if (project.kind == kind && project.state == cmd.state &&
                    (is_state_wide(kind) || project.province == cmd.province)) {
                    return CommandResult::Applied;
                }
            }
            return CommandResult::UnknownEntity;
        }
        case CommandType::StartResearch: {
            if (!g.content.tech_def(cmd.tech)) return CommandResult::UnknownEntity;
            if (!tech_available(g, cmd.country, cmd.tech)) return CommandResult::PrerequisitesMissing;
            for (const auto& slot : c->research.slots) {
                if (!slot.active) return CommandResult::Applied;
            }
            return CommandResult::SlotUnavailable;
        }
        case CommandType::CancelResearch: {
            for (const auto& slot : c->research.slots) {
                if (slot.active && slot.tech == cmd.tech) return CommandResult::Applied;
            }
            return CommandResult::UnknownEntity;
        }
        case CommandType::CreateTemplate: {
            if (cmd.text.empty()) return CommandResult::InvalidValue;
            if (cmd.battalions.empty()) return CommandResult::InvalidValue;
            int line_battalions = 0;
            int support = 0;
            for (const auto& b : cmd.battalions) {
                if (!g.content.equipment_def(b.equipment)) return CommandResult::UnknownEntity;
                if (b.count < 0 || b.count > 25) return CommandResult::InvalidValue;
                if (b.support) {
                    support += 1;
                } else {
                    line_battalions += b.count;
                }
            }
            if (line_battalions < 1 || line_battalions > 25) return CommandResult::InvalidValue;
            if (support > 5) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::EditTemplate: {
            const DivisionTemplate* t = g.content.template_def(cmd.template_id);
            if (!t) return CommandResult::UnknownEntity;
            if (t->country != cmd.country) return CommandResult::NotOwner;
            if (cmd.battalions.empty()) return CommandResult::InvalidValue;
            for (const auto& b : cmd.battalions) {
                if (!g.content.equipment_def(b.equipment)) return CommandResult::UnknownEntity;
            }
            return CommandResult::Applied;
        }
        case CommandType::RecruitDivision: {
            const DivisionTemplate* t = g.content.template_def(cmd.template_id);
            if (!t) return CommandResult::UnknownEntity;
            if (t->country != cmd.country) return CommandResult::NotOwner;
            if (cmd.value < 1 || cmd.value > 10) return CommandResult::InvalidValue;
            if (c->manpower < t->manpower * cmd.value) return CommandResult::InsufficientResources;
            return CommandResult::Applied;
        }
        case CommandType::DeployDivision: {
            const Division* d = g.world.division(cmd.division);
            if (!d || d->country != cmd.country) return CommandResult::UnknownEntity;
            if (!d->in_training()) return CommandResult::InvalidTarget;
            if (d->training_days_left > 0.0) return CommandResult::PrerequisitesMissing;
            const Province* p = g.world.province(cmd.province);
            if (!p || p->is_sea || p->controller != cmd.country) return CommandResult::InvalidTarget;
            return CommandResult::Applied;
        }
        case CommandType::MoveDivision: {
            const Division* d = g.world.division(cmd.division);
            if (!d || d->country != cmd.country) return CommandResult::UnknownEntity;
            if (d->in_training()) return CommandResult::InvalidTarget;
            if (d->in_combat()) return CommandResult::InvalidTarget;
            const Province* target = g.world.province(cmd.province);
            if (!target || target->is_sea) return CommandResult::InvalidTarget;
            if (cmd.province == d->location) return CommandResult::InvalidTarget;
            PathRequest req;
            req.country = d->country;
            req.require_controlled = false;
            req.allow_hostile = g.world.at_war(d->country, target->controller);
            if (find_path(g.world, d->location, cmd.province, req).empty()) {
                return CommandResult::InvalidTarget;
            }
            return CommandResult::Applied;
        }
        case CommandType::SetDivisionOrder: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            if (cmd.value < 0 || cmd.value >= static_cast<int>(OrderKind::Count)) {
                return CommandResult::InvalidValue;
            }
            return CommandResult::Applied;
        }
        case CommandType::CreateArmy: {
            if (cmd.text.empty()) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::AssignDivisionToArmy: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            if (cmd.divisions.empty()) return CommandResult::InvalidValue;
            for (DivisionId did : cmd.divisions) {
                const Division* d = g.world.division(did);
                if (!d || d->country != cmd.country) return CommandResult::UnknownEntity;
            }
            return CommandResult::Applied;
        }
        case CommandType::AssignGeneral: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            const Character* ch = g.world.character(cmd.character);
            if (!ch || ch->country != cmd.country) return CommandResult::UnknownEntity;
            return CommandResult::Applied;
        }
        case CommandType::DeclareWar: {
            const Country* target = g.world.country(cmd.target_country);
            if (!target || !target->alive) return CommandResult::UnknownEntity;
            if (cmd.target_country == cmd.country) return CommandResult::InvalidTarget;
            if (g.world.at_war(cmd.country, cmd.target_country)) return CommandResult::AlreadyAtWar;
            return CommandResult::Applied;
        }
        case CommandType::OfferPeace: {
            const War* w = g.world.war(cmd.war);
            if (!w || !w->active) return CommandResult::UnknownEntity;
            bool participant = false;
            for (const auto& p : w->attackers)
                if (p.country == cmd.country) participant = true;
            for (const auto& p : w->defenders)
                if (p.country == cmd.country) participant = true;
            if (!participant) return CommandResult::NotOwner;
            return CommandResult::Applied;
        }
        case CommandType::JoinFaction: {
            const Country* leader = g.world.country(cmd.target_country);
            if (!leader || !leader->alive) return CommandResult::UnknownEntity;
            if (leader->id == cmd.country) return CommandResult::InvalidTarget;
            if (c->faction != 0) return CommandResult::InvalidValue;
            if (leader->ideology != c->ideology) return CommandResult::PrerequisitesMissing;
            if (g.world.at_war(cmd.country, cmd.target_country)) return CommandResult::AtWar;
            if (faction_of(g.world, cmd.target_country) == 0) {
                return CommandResult::PrerequisitesMissing;
            }
            return CommandResult::Applied;
        }
        case CommandType::LeaveFaction: {
            if (c->faction == 0) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::CreateAirWing: {
            const EquipmentDef* def = g.content.equipment_def(cmd.equipment);
            if (!def || def->is_archetype) return CommandResult::UnknownEntity;
            if (def->category != EquipmentCategory::Aircraft) return CommandResult::InvalidTarget;
            if (!equipment_unlocked(g, cmd.country, cmd.equipment)) {
                return CommandResult::PrerequisitesMissing;
            }
            const Province* base = g.world.province(cmd.province);
            if (!base || base->is_sea || base->controller != cmd.country) {
                return CommandResult::NotOwner;
            }
            if (base->air_base <= 0) return CommandResult::PrerequisitesMissing;
            if (cmd.value < 10 || cmd.value > 1000) return CommandResult::InvalidValue;
            if (planes_stationed_at(g, cmd.province) + cmd.value > air_base_capacity(g, cmd.province)) {
                return CommandResult::QueueFull;
            }
            return CommandResult::Applied;
        }
        case CommandType::DeployAirWing: {
            const AirWing* wing = g.world.wing(cmd.wing);
            if (!wing || wing->country != cmd.country) return CommandResult::UnknownEntity;
            const Province* base = g.world.province(cmd.province);
            if (!base || base->is_sea || base->controller != cmd.country) {
                return CommandResult::NotOwner;
            }
            if (base->air_base <= 0) return CommandResult::PrerequisitesMissing;
            if (planes_stationed_at(g, cmd.province) + wing->max_planes >
                air_base_capacity(g, cmd.province)) {
                return CommandResult::QueueFull;
            }
            return CommandResult::Applied;
        }
        case CommandType::SetAirMission: {
            const AirWing* wing = g.world.wing(cmd.wing);
            if (!wing || wing->country != cmd.country) return CommandResult::UnknownEntity;
            if (cmd.value <= 0 || cmd.value >= static_cast<int>(AirMission::Count)) {
                return CommandResult::InvalidValue;
            }
            if (!g.world.regions.alive(cmd.region)) return CommandResult::UnknownEntity;
            if (!wing_can_reach(g, *wing, cmd.region)) return CommandResult::InvalidTarget;
            return CommandResult::Applied;
        }
        case CommandType::DisbandAirWing: {
            const AirWing* wing = g.world.wing(cmd.wing);
            if (!wing || wing->country != cmd.country) return CommandResult::UnknownEntity;
            return CommandResult::Applied;
        }
        case CommandType::CreateFleet: {
            return cmd.text.empty() ? CommandResult::InvalidValue : CommandResult::Applied;
        }
        case CommandType::CreateTaskForce: {
            const EquipmentDef* def = g.content.equipment_def(cmd.equipment);
            if (!def || def->is_archetype) return CommandResult::UnknownEntity;
            if (def->category != EquipmentCategory::Ship) return CommandResult::InvalidTarget;
            if (!equipment_unlocked(g, cmd.country, cmd.equipment)) {
                return CommandResult::PrerequisitesMissing;
            }
            if (!is_usable_port(g, cmd.country, cmd.province)) return CommandResult::NotOwner;
            if (cmd.value < 1 || cmd.value > 40) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::SetNavalMission: {
            const TaskForce* tf = g.world.task_force(cmd.task_force);
            if (!tf || tf->country != cmd.country) return CommandResult::UnknownEntity;
            // Mission::None is legal: it means "stand down", which sends the force home
            // to repair (the naval phase handles the return).
            if (cmd.value < 0 || cmd.value >= static_cast<int>(NavalMission::Count)) {
                return CommandResult::InvalidValue;
            }
            const Region* region = g.world.regions.try_get(cmd.region);
            if (!region || !region->is_sea) return CommandResult::InvalidTarget;
            return CommandResult::Applied;
        }
        case CommandType::AssignShipToTaskForce: {
            const Ship* ship = g.world.ship(cmd.ship_id);
            const TaskForce* tf = g.world.task_force(cmd.task_force);
            if (!ship || ship->country != cmd.country) return CommandResult::UnknownEntity;
            if (!tf || tf->country != cmd.country) return CommandResult::UnknownEntity;
            return CommandResult::Applied;
        }
        case CommandType::LaunchNavalInvasion: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            const Province* origin = g.world.province(cmd.province);
            const Province* target = g.world.province(cmd.province_b);
            if (!origin || !target) return CommandResult::InvalidTarget;
            if (!is_usable_port(g, cmd.country, cmd.province)) return CommandResult::NotOwner;
            if (target->is_sea || !target->coastal) return CommandResult::InvalidTarget;
            if (target->controller == cmd.country) return CommandResult::InvalidTarget;
            return CommandResult::Applied;
        }
        case CommandType::CancelNavalInvasion: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            return CommandResult::Applied;
        }
        case CommandType::SelectFocus: {
            const uint32_t focus = g.content.focus_id(cmd.text);
            if (focus == INVALID_FOCUS) return CommandResult::UnknownEntity;
            if (!focus_available(g, cmd.country, focus)) return CommandResult::PrerequisitesMissing;
            return CommandResult::Applied;
        }
        case CommandType::CancelFocus: {
            if (c->selected_focus == INVALID_FOCUS) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::ChooseEventOption: {
            const uint32_t event = g.content.event_id(cmd.text);
            if (event == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
            const EventDef* def = g.content.event(event);
            if (!def) return CommandResult::UnknownEntity;
            if (cmd.value < 0 || cmd.value >= static_cast<int>(def->options.size())) {
                return CommandResult::InvalidValue;
            }
            bool pending = false;
            for (uint32_t e : c->pending_events) {
                if (e == event) pending = true;
            }
            if (!pending) return CommandResult::InvalidTarget;
            return CommandResult::Applied;
        }
        case CommandType::TakeDecision: {
            const uint32_t decision = g.content.decision_id(cmd.text);
            if (decision == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
            if (!decision_visible(g, cmd.country, decision)) return CommandResult::InvalidTarget;
            if (!decision_available(g, cmd.country, decision)) {
                return CommandResult::PrerequisitesMissing;
            }
            return CommandResult::Applied;
        }
        case CommandType::CancelDecision: {
            const uint32_t decision = g.content.decision_id(cmd.text);
            if (decision == 0xFFFFFFFFu) return CommandResult::UnknownEntity;
            return CommandResult::Applied;
        }
        case CommandType::SetLaw: {
            const LawDef* law = g.content.law(cmd.text);
            if (!law) return CommandResult::UnknownEntity;
            if (c->political_power < law->cost) return CommandResult::InsufficientResources;
            if (law->kind >= 0 && law->kind < static_cast<int>(c->law_levels.size()) &&
                c->law_levels[law->kind] == law->level) {
                return CommandResult::InvalidValue;
            }
            return CommandResult::Applied;
        }
        case CommandType::SetTradePolicy: {
            for (const auto& law : g.content.laws) {
                if (law.kind != TRADE_LAW_KIND || law.level != cmd.value) continue;
                if (c->political_power < law.cost) return CommandResult::InsufficientResources;
                return CommandResult::Applied;
            }
            return CommandResult::InvalidValue;
        }
        case CommandType::SetStance: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            if (!valid_stance(cmd.value)) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::MotorizeSupply: {
            const Army* a = nullptr;
            if (!owns_army(g.world, cmd, &a)) return CommandResult::UnknownEntity;
            if (cmd.value < 0 || cmd.value > 5) return CommandResult::InvalidValue;
            return CommandResult::Applied;
        }
        case CommandType::ToggleFuelPriority:
            return CommandResult::Applied;
        default:
            return CommandResult::InvalidType;
    }
}

CommandResult apply_command(Game& g, const Command& cmd) {
    switch (cmd.type) {
        case CommandType::SetProductionLine: return do_set_production_line(g, cmd);
        case CommandType::RemoveProductionLine: {
            Command removal = cmd;
            removal.value = 0;
            return do_set_production_line(g, removal);
        }
        case CommandType::StartConstruction: return do_start_construction(g, cmd);
        case CommandType::CancelConstruction: return do_cancel_construction(g, cmd);
        case CommandType::StartResearch: return do_start_research(g, cmd);
        case CommandType::CancelResearch: return do_cancel_research(g, cmd);
        case CommandType::CreateTemplate: return do_create_template(g, cmd);
        case CommandType::EditTemplate: return do_edit_template(g, cmd);
        case CommandType::RecruitDivision: return do_recruit_division(g, cmd);
        case CommandType::DeployDivision: return do_deploy_division(g, cmd);
        case CommandType::MoveDivision: return do_move_division(g, cmd);
        case CommandType::SetDivisionOrder: return do_set_division_order(g, cmd);
        case CommandType::CreateArmy: return do_create_army(g, cmd);
        case CommandType::AssignDivisionToArmy: return do_assign_division_to_army(g, cmd);
        case CommandType::AssignGeneral: return do_assign_general(g, cmd);
        case CommandType::DeclareWar: return do_declare_war(g, cmd);
        case CommandType::OfferPeace: return do_offer_peace(g, cmd);
        case CommandType::JoinFaction:
            return join_faction(g, cmd.country, cmd.target_country) ? CommandResult::Applied
                                                                   : CommandResult::PrerequisitesMissing;
        case CommandType::LeaveFaction:
            return leave_faction(g, cmd.country) ? CommandResult::Applied
                                                 : CommandResult::PrerequisitesMissing;
        case CommandType::CreateAirWing: return do_create_air_wing(g, cmd);
        case CommandType::DeployAirWing: return do_deploy_air_wing(g, cmd);
        case CommandType::SetAirMission: return do_set_air_mission(g, cmd);
        case CommandType::DisbandAirWing: return do_disband_air_wing(g, cmd);
        case CommandType::CreateFleet: return do_create_fleet(g, cmd);
        case CommandType::CreateTaskForce: return do_create_task_force(g, cmd);
        case CommandType::SetNavalMission: return do_set_naval_mission(g, cmd);
        case CommandType::AssignShipToTaskForce: return do_assign_ship(g, cmd);
        case CommandType::LaunchNavalInvasion: return do_launch_invasion(g, cmd);
        case CommandType::CancelNavalInvasion: return do_cancel_invasion(g, cmd);
        case CommandType::SelectFocus: return do_select_focus(g, cmd);
        case CommandType::CancelFocus: return do_cancel_focus(g, cmd);
        case CommandType::ChooseEventOption: return do_choose_event_option(g, cmd);
        case CommandType::TakeDecision: return do_take_decision(g, cmd);
        case CommandType::CancelDecision: return do_cancel_decision(g, cmd);
        case CommandType::SetLaw: return do_set_law(g, cmd);
        case CommandType::SetTradePolicy: return do_set_trade_policy(g, cmd);
        case CommandType::SetStance: return do_set_stance(g, cmd);
        case CommandType::MotorizeSupply: return do_motorize_supply(g, cmd);
        case CommandType::ToggleFuelPriority: {
            Country& c = g.world.countries[cmd.country];
            c.fuel_priority = !c.fuel_priority;
            return CommandResult::Applied;
        }
        default: return CommandResult::InvalidType;
    }
}

void phase_commands(Game& g) {
    if (g.queue.pending.empty()) return;
    std::vector<Command> pending;
    pending.swap(g.queue.pending);
    for (const Command& cmd : pending) {
        const CommandResult validation = validate_command(g, cmd);
        // The log records what actually happened: a command that validated but was
        // refused in application (state changed between the two, or an application
        // rule is stricter) is recorded with its real result.
        CommandResult result = validation;
        if (validation == CommandResult::Applied) {
            result = apply_command(g, cmd);
        }
        g.log.record(g.world.tick, cmd, result);
    }
}

void phase_training(Game& g) {
    const double hours = 1.0;
    for (uint32_t ci = 0; ci < g.world.countries.capacity(); ++ci) {
        CountryId cid(ci);
        Country* c = g.world.countries.try_get(cid);
        if (!c || !c->alive) continue;
        for (size_t i = 0; i < c->training.size();) {
            TrainingDivision& slot = c->training[i];
            Division* d = g.world.divisions.try_get(slot.division);
            if (!d || !d->in_training()) {
                c->training.erase(c->training.begin() + static_cast<long>(i));
                continue;
            }
            const DivisionTemplate* t = g.content.template_def(slot.template_id);
            if (!t) {
                c->training.erase(c->training.begin() + static_cast<long>(i));
                continue;
            }
            if (slot.days_left <= 0.0) {
                ++i;
                continue;
            }

            const double train_days = std::max(1.0, t->train_days);
            const double training_time_mod = 1.0 + c->total_modifiers().get(ModifierKind::TrainingTime);
            const double day_fraction = hours / 24.0 * std::max(0.25, training_time_mod);

            // Equipment arrives gradually: each hour the division draws its share of
            // the template's equipment from the stockpile when available.
            double equipment_ratio = 0.0;
            int equipment_kinds = 0;
            for (const auto& b : t->battalions) {
                if (!b.equipment.valid() || b.count <= 0) continue;
                ++equipment_kinds;
                const double need = static_cast<double>(b.count);
                const double per_hour = need / (train_days * 24.0);
                const size_t idx = b.equipment.v;
                if (idx >= c->equipment_stockpile.size()) continue;
                const double take = std::min(per_hour, c->equipment_stockpile[idx]);
                c->equipment_stockpile[idx] -= take;
                if (idx >= d->equipment.size()) d->equipment.resize(idx + 1, 0.0);
                d->equipment[idx] += take;
                equipment_ratio += d->equipment[idx] / std::max(1.0, need);
            }
            if (equipment_kinds > 0) equipment_ratio /= static_cast<double>(equipment_kinds);

            // Training stalls without equipment; progress is gated on it.
            const double gate = std::min(1.0, equipment_ratio + 0.25);
            slot.days_left = std::max(0.0, slot.days_left - day_fraction * gate);
            d->training_days_left = slot.days_left;

            const double done = 1.0 - (slot.days_left / train_days);
            d->strength = std::max(d->strength, std::min(1.0, equipment_ratio) * done);
            d->manpower = t->manpower * done;
            d->max_organization = t->max_organization;
            d->organization = t->max_organization * done * (0.5 + 0.5 * std::min(1.0, equipment_ratio));
            ++i;
        }
    }
}

void serialize_command(ByteWriter& w, const Command& c) {
    w.u8(static_cast<uint8_t>(c.type));
    w.u32(c.country.v);
    w.u64(c.issued_tick);
    w.u32(c.province.v);
    w.u32(c.province_b.v);
    w.u32(c.state.v);
    w.u32(c.region.v);
    w.u32(c.division.v);
    w.u32(c.army.v);
    w.u32(c.character.v);
    w.u32(c.wing.v);
    w.u32(c.fleet_id.v);
    w.u32(c.ship_id.v);
    w.u32(c.task_force.v);
    w.u32(c.equipment.v);
    w.u32(c.template_id.v);
    w.u32(c.tech.v);
    w.u32(c.target_country.v);
    w.u32(c.war.v);
    w.i32(c.value);
    w.f64(c.value_f);
    w.str(c.text);
    w.u32(static_cast<uint32_t>(c.battalions.size()));
    for (const auto& b : c.battalions) {
        w.u32(b.equipment.v);
        w.i32(b.count);
        w.boolean(b.support);
    }
    w.u32(static_cast<uint32_t>(c.divisions.size()));
    for (DivisionId d : c.divisions) w.u32(d.v);
}

Command deserialize_command(ByteReader& r) {
    Command c;
    uint8_t type = 0;
    r.u8(&type);
    c.type = static_cast<CommandType>(type);
    r.u32(&c.country.v);
    r.u64(&c.issued_tick);
    r.u32(&c.province.v);
    r.u32(&c.province_b.v);
    r.u32(&c.state.v);
    r.u32(&c.region.v);
    r.u32(&c.division.v);
    r.u32(&c.army.v);
    r.u32(&c.character.v);
    r.u32(&c.wing.v);
    r.u32(&c.fleet_id.v);
    r.u32(&c.ship_id.v);
    r.u32(&c.task_force.v);
    r.u32(&c.equipment.v);
    r.u32(&c.template_id.v);
    r.u32(&c.tech.v);
    r.u32(&c.target_country.v);
    r.u32(&c.war.v);
    r.i32(&c.value);
    r.f64(&c.value_f);
    r.str(&c.text);
    uint32_t n = 0;
    r.u32(&n);
    c.battalions.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        r.u32(&c.battalions[i].equipment.v);
        r.i32(&c.battalions[i].count);
        r.boolean(&c.battalions[i].support);
    }
    r.u32(&n);
    c.divisions.resize(n);
    for (uint32_t i = 0; i < n; ++i) r.u32(&c.divisions[i].v);
    return c;
}

}  // namespace hoi
