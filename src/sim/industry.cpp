// Industry, resources, production lines and construction (ARCHITECTURE 5.1-5.2).
//
// Units used throughout this file, so that every formula stays dimensionally
// consistent with the data:
//   * `SimConstants::ic_per_*_factory` is industrial capacity produced per DAY,
//   * an hourly capacity rate is therefore `facilities * ic_per_factory / 24`,
//   * `EquipmentDef::build_cost` and `BuildingDef::base_cost` are measured in
//     industrial capacity-DAYS, so `capacity_hours` is already the amount a
//     project's `progress` accrues per hour (progress and cost share units),
//   * `ResourceBalance`/`Country::resources_*` are DAILY totals; the hourly pool a
//     production line draws from is that daily total divided by 24,
//   * a production line's resource requirement is per assigned factory
//     (`EquipmentDef::resources` is the daily draw of one factory on that model),
//     which is what makes the available pool limit how much industry runs.

#include "sim/industry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "game/game.h"

namespace hoi {
namespace {

using R = std::underlying_type_t<Resource>;

bool is_dockyard_category(EquipmentCategory cat) {
    return cat == EquipmentCategory::Ship || cat == EquipmentCategory::Convoy;
}

// Dockyards build ships and convoys, military factories build everything else.
double line_ic_per_factory(const Content& content, const EquipmentDef* def) {
    const SimConstants& k = content.constants;
    return def && is_dockyard_category(def->category) ? k.ic_per_dockyard
                                                      : k.ic_per_military_factory;
}

// Output modifier of the production line's facility kind. Ships draw on
// DockyardOutput, every other line on FactoryOutput (both are additive fractions).
double line_output_modifier(const Country& c, const EquipmentDef* def) {
    const Modifiers m = c.total_modifiers();
    return def && is_dockyard_category(def->category) ? m.get(ModifierKind::DockyardOutput)
                                                      : m.get(ModifierKind::FactoryOutput);
}

// Stockpiles are indexed by EquipmentId, so they are grown to the content size
// before any production. New entries start empty; no signalling is invented.
void ensure_stockpile(Country& c, size_t equipment_count) {
    if (c.equipment_stockpile.size() < equipment_count) {
        c.equipment_stockpile.resize(equipment_count, 0.0);
    }
}

const BuildingDef* find_building(const Content& content, BuildingKind kind) {
    for (const BuildingDef& b : content.buildings) {
        if (b.kind == kind) return &b;
    }
    return nullptr;
}

bool is_factory_kind(BuildingKind kind) {
    return kind == BuildingKind::CivilianFactory || kind == BuildingKind::MilitaryFactory ||
           kind == BuildingKind::Dockyard;
}

// Base cost in capacity-days. Data wins; SimConstants is the fallback so that a
// scenario without building definitions still builds something sane.
double building_base_cost(const Content& content, BuildingKind kind) {
    const BuildingDef* def = find_building(content, kind);
    if (def && def->base_cost > 0.0) return def->base_cost;
    const SimConstants& k = content.constants;
    switch (kind) {
        case BuildingKind::CivilianFactory:
        case BuildingKind::MilitaryFactory:
        case BuildingKind::Dockyard:
            return k.construction_cost_factory;
        case BuildingKind::Infrastructure:
            return k.construction_cost_infrastructure;
        case BuildingKind::Railway:
            return k.construction_cost_railway;
        case BuildingKind::SupplyHub:
            return k.construction_cost_supply_hub;
        case BuildingKind::AirBase:
            return k.construction_cost_air_base;
        case BuildingKind::NavalBase:
            return k.construction_cost_naval_base;
        case BuildingKind::Radar:
            return k.construction_cost_radar;
        case BuildingKind::SyntheticRefinery:
            return k.construction_cost_synthetic;
        case BuildingKind::Fort:
        case BuildingKind::AntiAir:
            return k.construction_cost_fort;
        case BuildingKind::Count:
            break;
    }
    return k.construction_cost_factory;
}

int building_max_level(const Content& content, BuildingKind kind) {
    const BuildingDef* def = find_building(content, kind);
    if (def && def->max_level > 0) return def->max_level;
    switch (kind) {
        case BuildingKind::CivilianFactory:
        case BuildingKind::MilitaryFactory:
        case BuildingKind::Dockyard:
            return 20;  // in practice limited by State::building_slots
        case BuildingKind::Infrastructure:
            return 10;
        case BuildingKind::Railway:
            return 5;
        case BuildingKind::SupplyHub:
            return 1;
        case BuildingKind::AirBase:
        case BuildingKind::NavalBase:
        case BuildingKind::Fort:
            return 10;
        case BuildingKind::Radar:
        case BuildingKind::AntiAir:
        case BuildingKind::SyntheticRefinery:
            return 5;
        case BuildingKind::Count:
            break;
    }
    return 0;
}

// Kinds whose level lives on a State rather than a Province.
bool is_state_scope(BuildingKind kind) {
    return is_factory_kind(kind) || kind == BuildingKind::SyntheticRefinery;
}

int project_current_level(const World& w, const ConstructionProject& p) {
    if (is_state_scope(p.kind)) {
        const State* s = w.state(p.state);
        if (!s) return 0;
        switch (p.kind) {
            case BuildingKind::CivilianFactory:
                return s->civilian_factories;
            case BuildingKind::MilitaryFactory:
                return s->military_factories;
            case BuildingKind::Dockyard:
                return s->dockyards;
            case BuildingKind::SyntheticRefinery:
                return s->synthetic_refineries;
            default:
                return 0;
        }
    }
    const Province* pr = w.province(p.province);
    if (!pr) return 0;
    switch (p.kind) {
        case BuildingKind::Infrastructure:
            return pr->infrastructure;
        case BuildingKind::Railway:
            return pr->railway_level;
        case BuildingKind::SupplyHub:
            return pr->supply_hub ? 1 : 0;
        case BuildingKind::AirBase:
            return pr->air_base;
        case BuildingKind::NavalBase:
            return pr->naval_base;
        case BuildingKind::Radar:
            return pr->radar;
        case BuildingKind::AntiAir:
            return pr->anti_air;
        case BuildingKind::Fort:
            return pr->fort_level;
        default:
            return 0;
    }
}

// Cost in capacity-days: base scaled LINEARLY by the levels already standing in the
// target - `base * (1 + (level_scaling - 1) * current_level)` - which is the same
// formula commands.cpp stamps on a project when a command creates it, so a project
// that arrives without a cost (script, hand-built world, older save) costs exactly
// what the command layer would have charged.
double project_cost(const Content& content, BuildingKind kind, int current_level) {
    const double base = building_base_cost(content, kind);
    const double scaling = content.constants.construction_level_scaling;
    const double cost = base * (1.0 + (scaling - 1.0) * static_cast<double>(current_level));
    return std::isfinite(cost) && cost > 0.0 ? cost : base;
}

enum class ProjectStatus { Ready, Blocked, Invalid };

// Blocked projects stay queued and consume no capacity (their block, e.g. a full
// building-slot row, can clear later). Invalid projects are pruned: the target is
// gone, or the level is already at the maximum the content allows.
ProjectStatus project_status(const World& w, const Content& content, const ConstructionProject& p) {
    if (is_state_scope(p.kind)) {
        const State* s = w.state(p.state);
        if (!s) return ProjectStatus::Invalid;
        // Building slots cap factories only: they are the state's industrial slots,
        // and the command layer accounts slots the same way (civ + mil + dock).
        if (is_factory_kind(p.kind) && s->building_slots > 0 &&
            s->total_factories() >= s->building_slots) {
            return ProjectStatus::Blocked;
        }
    } else if (!w.province(p.province)) {
        return ProjectStatus::Invalid;
    }
    if (p.repair) return ProjectStatus::Ready;  // repairs restore damaged levels
    const int current = project_current_level(w, p);
    const int max_level = building_max_level(content, p.kind);
    if (max_level <= 0 || current >= max_level) return ProjectStatus::Invalid;
    return ProjectStatus::Ready;
}

// Raises the target to the project's level and removes it from the queue. Returns
// false when the target vanished between the status check and the write.
bool complete_project(Game& g, Country& c, const ConstructionProject& p) {
    World& w = g.world;
    if (p.repair) return true;  // level already present; the project only paid for repairs
    const int current = project_current_level(w, p);
    const int max_level = building_max_level(g.content, p.kind);
    int wanted = p.target_level > current ? p.target_level : current + 1;
    if (wanted > max_level) wanted = max_level;
    if (wanted < current) wanted = current;  // never lower a level
    switch (p.kind) {
        case BuildingKind::CivilianFactory: {
            State* s = w.state(p.state);
            if (!s) return false;
            s->civilian_factories = wanted;
            break;
        }
        case BuildingKind::MilitaryFactory: {
            State* s = w.state(p.state);
            if (!s) return false;
            s->military_factories = wanted;
            break;
        }
        case BuildingKind::Dockyard: {
            State* s = w.state(p.state);
            if (!s) return false;
            s->dockyards = wanted;
            break;
        }
        case BuildingKind::SyntheticRefinery: {
            State* s = w.state(p.state);
            if (!s) return false;
            s->synthetic_refineries = wanted;
            break;
        }
        case BuildingKind::Infrastructure: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->infrastructure = clamp(wanted, 0, 10);
            break;
        }
        case BuildingKind::Railway: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->railway_level = clamp(wanted, 0, 5);
            break;
        }
        case BuildingKind::SupplyHub: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->supply_hub = wanted > 0;
            break;
        }
        case BuildingKind::AirBase: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->air_base = wanted;
            break;
        }
        case BuildingKind::NavalBase: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->naval_base = wanted;
            break;
        }
        case BuildingKind::Radar: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->radar = wanted;
            break;
        }
        case BuildingKind::AntiAir: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->anti_air = wanted;
            break;
        }
        case BuildingKind::Fort: {
            Province* pr = w.province(p.province);
            if (!pr) return false;
            pr->fort_level = wanted;
            break;
        }
        case BuildingKind::Count:
            return false;
    }
    HOI_DEBUG("construction: country %u completed %s project (level %d)", c.id.raw(),
              building_kind_name(p.kind), wanted);
    return true;
}

// Efficiency retention on an equipment switch (ARCHITECTURE 5.1).
//
// Switch protocol: whoever changes `line.equipment` (player command, AI, script)
// pushes the model it came from onto the FRONT of `line.previous` and sets the new
// equipment. `previous` is therefore a pending-switch queue, and industry drains it
// here, so every switch path shares one implementation of the retention rule.
void apply_switch_retention(const Content& content, ProductionLine& line) {
    const EquipmentDef* new_def = content.equipment_def(line.equipment);
    // Bounded by `line.previous` itself; the explicit cap keeps a corrupt save from
    // spinning here.
    for (size_t applied = 0; applied < 8 && !line.previous.empty(); ++applied) {
        const EquipmentId from = line.previous.front();
        if (from == line.equipment) break;
        line.previous.erase(line.previous.begin());
        const EquipmentDef* old_def = content.equipment_def(from);
        const bool same_archetype = old_def && new_def && !old_def->archetype.empty() &&
                                    old_def->archetype == new_def->archetype;
        if (same_archetype) {
            line.efficiency *= content.constants.switch_same_archetype_retention;
            line.efficiency_cap *= content.constants.switch_same_archetype_retention;
        } else {
            line.efficiency = content.constants.efficiency_start;
            line.efficiency_cap = content.constants.efficiency_cap_base;
        }
        line.efficiency = clamp01(line.efficiency);
        line.efficiency_cap = clamp01(line.efficiency_cap);
        if (line.efficiency > line.efficiency_cap) line.efficiency = line.efficiency_cap;
        HOI_DEBUG("production line of country switched model: retention applied, efficiency %.4f cap %.4f",
                  line.efficiency, line.efficiency_cap);
    }
}

}  // namespace

void compute_resource_production(const World& w, const Content& c, CountryId country,
                                 double out[RESOURCE_COUNT]) {
    for (int r = 0; r < RESOURCE_COUNT; ++r) out[r] = 0.0;
    if (!country.valid()) return;
    w.provinces.for_each([&](ProvinceId, const Province& p) {
        if (p.is_sea || p.controller != country) return;
        // A province pays out only while its state answers to the same controller:
        // the state controller is the extraction authority, so a province whose
        // controller differs from its state's is excluded.
        const State* st = w.state(p.state);
        if (st && st->controller.valid() && st->controller != country) return;
        const double infra = 1.0 + 0.05 * static_cast<double>(p.infrastructure);
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            const double yield = p.resource_yield[r];
            if (yield > 0.0) out[r] += yield * infra;
        }
    });
    // Synthetic refineries are a second, state-level source of oil and rubber that
    // does not depend on province geology (hence no infrastructure scaling). Each
    // state is counted once, for its controller.
    const double synthetic_oil = c.constants.synthetic_oil_per_refinery_per_day;
    const double synthetic_rubber = c.constants.synthetic_rubber_per_refinery_per_day;
    if (synthetic_oil > 0.0 || synthetic_rubber > 0.0) {
        w.states.for_each([&](StateId, const State& s) {
            if (s.controller != country || s.synthetic_refineries <= 0) return;
            const double refineries = static_cast<double>(s.synthetic_refineries);
            if (synthetic_oil > 0.0) out[static_cast<R>(Resource::Oil)] += refineries * synthetic_oil;
            if (synthetic_rubber > 0.0) {
                out[static_cast<R>(Resource::Rubber)] += refineries * synthetic_rubber;
            }
        });
    }
    for (int r = 0; r < RESOURCE_COUNT; ++r) out[r] = finite_or(out[r], 0.0);
}

void equipment_resource_cost(const EquipmentDef& def, double out[RESOURCE_COUNT]) {
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        const double v = def.resources[r];
        out[r] = (std::isfinite(v) && v > 0.0) ? v : 0.0;
    }
}

double line_hourly_output(const Game& g, const Country& c, const ProductionLine& line) {
    if (line.factories <= 0) return 0.0;
    const EquipmentDef* def = g.content.equipment_def(line.equipment);
    const double efficiency = clamp01(line.efficiency);
    const double ic = line_ic_per_factory(g.content, def);
    const double out =
        static_cast<double>(line.factories) * ic / static_cast<double>(TICKS_PER_DAY) *
        efficiency * (1.0 + line_output_modifier(c, def));
    return std::isfinite(out) && out > 0.0 ? out : 0.0;
}

double equipment_unit_cost(const Game& g, const Country& c, const EquipmentDef& def) {
    // Cost modifiers live on the country; `g` is part of the declared interface.
    (void)g;
    const double cost_factor = 1.0 + c.total_modifiers().get(ModifierKind::EquipmentCostFactor);
    if (!std::isfinite(cost_factor)) return 0.0;
    const double cost = def.build_cost * cost_factor;
    // Real equipment costs are small (rifles cost well under one industrial
    // capacity-day), so nothing is floored here: callers divide with `safe_div`,
    // which yields no output for a free or malformed model instead of inventing a
    // price.
    if (!std::isfinite(cost) || cost <= 0.0) return 0.0;
    return cost;
}

double line_resource_factor(const Game& g, const Country& c, const ProductionLine& line,
                            const double available[RESOURCE_COUNT]) {
    // The requirement depends on the line's assigned factories, not on country
    // state; `c` is part of the declared interface.
    (void)c;
    const EquipmentDef* def = g.content.equipment_def(line.equipment);
    if (!def || line.factories <= 0) return 1.0;  // nothing is demanded
    double price[RESOURCE_COUNT];
    equipment_resource_cost(*def, price);
    bool any_resource = false;
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        if (price[r] > 0.0) any_resource = true;
    }
    if (!any_resource) return 1.0;
    // Requirement is per assigned factory, not per unit built (HOI4 parity): each
    // factory on the line draws the equipment's resource price per day, so the
    // available pool limits how much industry can be kept running. `available` is
    // in the same per-hour units as the requirement computed here.
    const double factory_hours = static_cast<double>(line.factories) /
                                 static_cast<double>(TICKS_PER_DAY);
    const double floor_factor = clamp01(g.content.constants.resource_shortage_floor);
    double factor = 1.0;
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        const double required = price[r] * factory_hours;
        if (!(required > 0.0)) continue;
        factor = std::min(factor, clamp(safe_div(available[r], required), floor_factor, 1.0));
    }
    return clamp01(factor);
}

void phase_industry(Game& g) {
    World& w = g.world;
    const Content& content = g.content;
    const SimConstants& k = content.constants;
    // Country::output_today and the resource totals are daily figures: they are
    // cleared when a new day starts.
    const bool day_start = w.date.hour == 0;

    w.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive) return;
        ensure_stockpile(c, content.equipment.size());

        // (a) Resource production of the states this country controls.
        ResourceBalance balance;
        compute_resource_production(w, content, id, balance.produced);
        for (int r = 0; r < RESOURCE_COUNT; ++r) c.resources_produced[r] = balance.produced[r];

        if (day_start) {
            for (ProductionLine& line : c.lines) line.output_today = 0.0;
        }

        // Facility counts are read once per country and hour: the assignment-release
        // rule, construction, fuel storage and the consumer-goods record all work
        // from the same numbers.
        int civ = 0, mil = 0, dock = 0;
        count_factories(w, id, &civ, &mil, &dock);

        // Broken-state rule (a repair, not a gameplay decision): after territorial
        // loss a country can control fewer military factories than its production
        // lines still claim. Nation loss is not something industry can play back,
        // so the excess is released here, from the LAST line in `Country::lines`
        // order, keeping the earliest (highest-priority) line intact. A line drained
        // to zero is retired exactly as RemoveProductionLine retires it: its model
        // goes onto the pending-switch queue and its equipment is cleared.
        //
        // Reporting is throttled by the release itself, not by a clock: a release is
        // only logged when it changes the country's line set (a line is retired), and
        // one message carries the whole hour's total. A war that shaves a factory per
        // hour off an eight-line country therefore logs at most once per retired line
        // instead of once per hour, and no cross-tick scratch state is needed.
        int assigned = 0;
        for (const ProductionLine& line : c.lines) {
            if (line.factories > 0) assigned += line.factories;
        }
        if (assigned > mil) {
            int excess = assigned - mil;
            const int released = excess;
            int retired_lines = 0;
            for (size_t i = c.lines.size(); i-- > 0 && excess > 0;) {
                ProductionLine& line = c.lines[i];
                if (line.factories <= 0) continue;
                const int taken = std::min(excess, line.factories);
                line.factories -= taken;
                excess -= taken;
                if (line.factories == 0) {
                    if (line.equipment.valid()) {
                        line.previous.insert(line.previous.begin(), line.equipment);
                        if (line.previous.size() > 8) line.previous.resize(8);
                    }
                    line.equipment = EquipmentId{};
                    line.output_today = 0.0;
                    ++retired_lines;
                }
            }
            if (retired_lines > 0) {
                HOI_WARN("industry: country %u released %d production-line factories it no longer "
                         "controls (assigned %d, controlled %d) and retired %d line(s)",
                         id.raw(), released, assigned, mil, retired_lines);
                g.log_event("production",
                            c.name + " lost production lines: " + std::to_string(released) +
                                " factory assignments released (industry lost)",
                            id);
            }
        }

        // The consumer-goods share is policy owned by politics (phase 9); it is
        // read here as the previous tick's value and clamps the civilian
        // construction branch below.

        // Hourly pool a production line may draw from: the day's extraction plus
        // imports, spread evenly over the day.
        double available[RESOURCE_COUNT];
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            const double daily = c.resources_produced[r] + c.resources_imported[r];
            available[r] = daily > 0.0 ? daily / static_cast<double>(TICKS_PER_DAY) : 0.0;
        }

        // (b)+(c) Allocate in `Country::lines` order, then advance each line. A
        // line sees what earlier lines left, which is what makes the order
        // deterministic and priority meaningful.
        double consumed_hourly[RESOURCE_COUNT] = {};
        for (ProductionLine& line : c.lines) {
            const EquipmentDef* def = content.equipment_def(line.equipment);
            if (!def) {
                // Unknown or retired model: the line builds nothing, so it is not
                // short of resources either unless it is staffed.
                line.resource_shortage = line.factories > 0 ? 1.0 : 0.0;
                continue;
            }
            apply_switch_retention(content, line);
            const double factor = line_resource_factor(g, c, line, available);
            line.resource_shortage = clamp01(1.0 - factor);
            const double ideal = line_hourly_output(g, c, line);
            const double unit_cost = equipment_unit_cost(g, c, *def);
            const double units = safe_div(ideal * factor, unit_cost);

            double price[RESOURCE_COUNT];
            equipment_resource_cost(*def, price);
            const double factory_hours = static_cast<double>(line.factories) /
                                         static_cast<double>(TICKS_PER_DAY);
            for (int r = 0; r < RESOURCE_COUNT; ++r) {
                const double want = price[r] * factory_hours;
                if (!(want > 0.0)) continue;
                // A line draws what its assigned factories need, limited by what is
                // left; under the shortage floor it can never consume more than is
                // there.
                const double take = std::min(want, available[r]);
                available[r] -= take;
                consumed_hourly[r] += take;
            }

            if (units > 0.0 && line.equipment.valid()) {
                c.equipment_stockpile[line.equipment.v] += units;
                line.output_today += units;
                line.output_total += units;
            }

            // Efficiency and its ceiling only move while the line is producing;
            // a starved or unstaffed line neither grows nor decays.
            if (line.factories > 0 && units > 0.0) {
                line.efficiency +=
                    (line.efficiency_cap - line.efficiency) * k.efficiency_growth_per_day /
                    static_cast<double>(TICKS_PER_DAY);
                line.efficiency_cap = std::min(
                    1.0, line.efficiency_cap +
                             k.efficiency_cap_growth_per_day / static_cast<double>(TICKS_PER_DAY));
                line.efficiency = clamp01(line.efficiency);
                if (line.efficiency > line.efficiency_cap) line.efficiency = line.efficiency_cap;
            }
        }
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            balance.consumed[r] = consumed_hourly[r] * static_cast<double>(TICKS_PER_DAY);
            c.resources_consumed[r] = balance.consumed[r];
        }

        // (d) Construction. Civilian capacity is split in queue order: a project
        // may hold at most `max_factories_per_project` factories, so a queue of N
        // projects uses up to N * cap factories and anything the whole queue cannot
        // absorb stays idle this hour (it is never concentrated on the head
        // project). Blocked projects hold no factories, so their share stays
        // available to the projects behind them.
        double idle_factories = static_cast<double>(civ);
        double per_project_cap = k.max_factories_per_project;
        if (!(per_project_cap > 0.0)) per_project_cap = idle_factories;  // no cap: old behaviour
        const double speed_factor = 1.0 + c.total_modifiers().get(ModifierKind::ConstructionSpeed);
        const double consumer_factor = 1.0 - clamp01(c.consumer_goods_ratio);
        const double ic_per_factory_hour =
            k.ic_per_civilian_factory / static_cast<double>(TICKS_PER_DAY);
        std::vector<ConstructionProject> remaining;
        remaining.reserve(c.construction.queue.size());
        for (ConstructionProject p : c.construction.queue) {
            const ProjectStatus status = project_status(w, content, p);
            if (status == ProjectStatus::Invalid) {
                HOI_DEBUG("construction: country %u dropped project kind=%d (target missing or "
                          "already at maximum level)",
                          id.raw(), static_cast<int>(p.kind));
                continue;
            }
            if (p.cost <= 0.0) p.cost = project_cost(content, p.kind, project_current_level(w, p));
            if (p.cost > 0.0 && p.progress >= p.cost && status == ProjectStatus::Ready) {
                if (complete_project(g, c, p)) {
                    g.log_event("construction", c.name + " completed construction", id);
                    continue;
                }
            }
            if (status != ProjectStatus::Ready) {
                remaining.push_back(std::move(p));  // blocked: keeps its place, no factories
                continue;
            }
            const double share = std::min(idle_factories, per_project_cap);
            if (share > 0.0) {
                idle_factories -= share;
                p.progress += share * ic_per_factory_hour * speed_factor * consumer_factor;
                if (p.cost > 0.0 && p.progress >= p.cost && complete_project(g, c, p)) {
                    // The factories this project held return to the pool at the
                    // next tick; the projects behind it already have their own
                    // share, so nothing stalls.
                    g.log_event("construction", c.name + " completed construction", id);
                    continue;
                }
            }
            remaining.push_back(std::move(p));
        }
        c.construction.queue.swap(remaining);

        // (e) Fuel: storage scales with the industry that supports it, and oil
        // extraction from this hour is added up to the storage ceiling.
        const double fuel_mods = 1.0 + c.total_modifiers().get(ModifierKind::FuelGain);
        const double capacity_fuel =
            static_cast<double>(civ + mil + dock) * k.fuel_storage_per_factory * fuel_mods;
        c.fuel_capacity = std::isfinite(capacity_fuel) && capacity_fuel > 0.0 ? capacity_fuel : 0.0;
        if (!(c.fuel > 0.0)) c.fuel = 0.0;
        const double oil_per_hour =
            c.resources_produced[static_cast<R>(Resource::Oil)] * k.fuel_per_oil /
            static_cast<double>(TICKS_PER_DAY);
        if (oil_per_hour > 0.0 && c.fuel < c.fuel_capacity) {
            c.fuel = std::min(c.fuel_capacity, c.fuel + oil_per_hour);
        }

        // (f) Record the consumer-goods share of civilian industry that was used
        // this hour: the required consumer-goods factories over all civilian
        // factories, i.e. the value the clamp above produced. With no civilian
        // factories the ratio has no industrial meaning, so the policy value owned
        // by politics is left untouched rather than zeroed.
        const double tied_up = consumer_goods_factories(g, c);
        if (civ > 0) {
            c.consumer_goods_ratio = clamp01(tied_up / static_cast<double>(civ));
        }
    });
}

double construction_output(const Game& g, const Country& c) {
    int civ = 0;
    count_factories(g.world, c.id, &civ, nullptr, nullptr);
    const double speed = c.total_modifiers().get(ModifierKind::ConstructionSpeed);
    const double out = static_cast<double>(civ) * g.content.constants.ic_per_civilian_factory /
                       static_cast<double>(TICKS_PER_DAY) * (1.0 + speed) *
                       (1.0 - clamp01(c.consumer_goods_ratio));
    return std::isfinite(out) && out > 0.0 ? out : 0.0;
}

double consumer_goods_factories(const Game& g, const Country& c) {
    int civ = 0;
    count_factories(g.world, c.id, &civ, nullptr, nullptr);
    if (civ <= 0) return 0.0;
    return static_cast<double>(civ) * clamp01(c.consumer_goods_ratio);
}

void count_factories(const World& w, CountryId country, int* civ, int* mil, int* dock) {
    int c = 0, m = 0, d = 0;
    if (country.valid()) {
        w.states.for_each([&](StateId, const State& s) {
            if (s.controller != country) return;
            c += s.civilian_factories;
            m += s.military_factories;
            d += s.dockyards;
        });
    }
    if (civ) *civ = c;
    if (mil) *mil = m;
    if (dock) *dock = d;
}

void compute_equipment_demand(const Game& g, CountryId country,
                              std::vector<double>* demand_by_equipment) {
    if (!demand_by_equipment) return;
    demand_by_equipment->assign(g.content.equipment.size(), 0.0);
    const Country* c = g.world.country(country);
    if (!c) return;
    std::vector<double> required(demand_by_equipment->size(), 0.0);

    // Template equipment needs, accumulated per equipment index.
    auto template_needs = [&](TemplateId template_id) {
        for (double& v : required) v = 0.0;
        const DivisionTemplate* t = g.content.template_def(template_id);
        if (!t) return false;
        for (const BattalionSlot& b : t->battalions) {
            if (!b.equipment.valid() || b.equipment.v >= required.size()) continue;
            if (b.count <= 0) continue;
            required[b.equipment.v] += static_cast<double>(b.count);
        }
        return true;
    };

    g.world.divisions.for_each([&](DivisionId, const Division& d) {
        if (d.country != country) return;
        if (!template_needs(d.template_id)) return;
        const double strength = clamp01(d.strength);
        const double strength_deficit = 1.0 - strength;
        for (size_t e = 0; e < required.size(); ++e) {
            const double need = required[e];
            if (!(need > 0.0)) continue;
            const double present = e < d.equipment.size() ? d.equipment[e] : 0.0;
            // The larger of "gear missing outright" and "gear missing to reach the
            // division's strength share" - either one justifies replacement.
            double missing = need - present;
            const double to_full_strength = need * strength_deficit;
            if (to_full_strength > missing) missing = to_full_strength;
            if (missing > 0.0) (*demand_by_equipment)[e] += missing;
        }
    });

    // Training queue. Trained divisions already exist as entities and were counted
    // by the walk above, so only entries whose division is gone (an order queued
    // before the entity exists) contribute here, at full template strength.
    for (const TrainingDivision& t : c->training) {
        if (g.world.division(t.division)) continue;
        if (!template_needs(t.template_id)) continue;
        for (size_t e = 0; e < required.size(); ++e) {
            if (required[e] > 0.0) (*demand_by_equipment)[e] += required[e];
        }
    }
}

double reinforce_division(Game& g, Division& d, EquipmentId equipment, double count) {
    if (!equipment.valid() || !(count > 0.0) || !std::isfinite(count)) return 0.0;
    const EquipmentDef* def = g.content.equipment_def(equipment);
    Country* c = g.world.country(d.country);
    if (!def || !c) return 0.0;
    const DivisionTemplate* t = g.content.template_def(d.template_id);
    if (!t) return 0.0;
    double required = 0.0;
    for (const BattalionSlot& b : t->battalions) {
        if (b.equipment == equipment && b.count > 0) required += static_cast<double>(b.count);
    }
    if (!(required > 0.0)) return 0.0;  // the template does not use this equipment

    ensure_stockpile(*c, g.content.equipment.size());
    if (d.equipment.size() < g.content.equipment.size()) {
        d.equipment.resize(g.content.equipment.size(), 0.0);
    }
    double& present = d.equipment[equipment.v];
    if (!(present > 0.0)) present = 0.0;
    const double missing = required - present;
    if (!(missing > 0.0)) return 0.0;
    const double moved = std::min(count, std::min(missing, c->equipment_stockpile[equipment.v]));
    if (!(moved > 0.0)) return 0.0;

    c->equipment_stockpile[equipment.v] -= moved;
    present += moved;

    // Strength and manpower follow the equipment moved in: strength as a share of
    // the template's total hit points, manpower as persons carried by the gear.
    const double hp_per_unit = def->max_strength > 0.0 ? def->max_strength : 1.0;
    const double template_hp = t->max_strength > 0.0 ? t->max_strength : 1.0;
    d.strength = clamp01(d.strength + safe_div(moved * hp_per_unit, template_hp));
    if (t->manpower > 0.0) {
        d.manpower = std::min(t->manpower, std::max(d.manpower, t->manpower * d.strength));
    }
    return moved;
}

bool industry_intact(const World& w, CountryId country) {
    int civ = 0, mil = 0, dock = 0;
    count_factories(w, country, &civ, &mil, &dock);
    return (civ + mil + dock) > 0;
}

}  // namespace hoi
