// Research progress and technology effects (ARCHITECTURE 5.3, spec section 49).
//
// Progress, `TechDef::cost_days` and the slot's accumulator are all measured in
// research-DAYS, so one tick (one hour) advances a slot by `TICKS_PER_HOUR /
// TICKS_PER_DAY` of a day multiplied by the country's research speed. Across
// `TICKS_PER_DAY` ticks a slot therefore advances exactly one day of work, which
// keeps `days_needed` meaning what its name says.
//
// Unlocks are derived, never stored: `equipment_unlocked` and `building_unlocked`
// scan the country's completed list against the content database, so a completed
// technology never needs a parallel registry that could desynchronize from save
// files.

#include "sim/research.h"

#include <cmath>
#include <string>
#include <vector>

#include "core/math.h"
#include "game/game.h"

namespace hoi {
namespace {

// Progressive-modelling penalty for researching ahead of a technology's year.
// ARCHITECTURE 5.3 fixes 0.5 per year ahead; SimConstants::research_year_penalty
// overrides it when data sets a non-zero value (its shipped default is 0.0, i.e.
// "use the documented constant").
constexpr double kAheadOfTimePenaltyPerYear = 0.5;

double ahead_of_time_penalty_per_year(const Content& content) {
    const double configured = content.constants.research_year_penalty;
    return configured > 0.0 ? configured : kAheadOfTimePenaltyPerYear;
}

// TechId of an entry of `Content::techs`: the loaded id when the loader assigned
// one, otherwise the index, which is the documented layout of the table.
TechId tech_id_at(const Content& content, size_t index) {
    const TechDef& t = content.techs[index];
    if (t.id.valid()) return t.id;
    return TechId(static_cast<uint32_t>(index));
}

}  // namespace

bool tech_available(const Game& g, CountryId country, TechId tech) {
    const Country* c = g.world.country(country);
    const TechDef* t = g.content.tech_def(tech);
    if (!c || !t) return false;
    if (c->research.has_tech(tech)) return false;
    // No year gate: researching ahead of time is legal and paid for with the
    // ahead-of-time penalty in `tech_cost_days`. The gate is the prerequisite
    // chain only, so a technology whose whole chain is finished is always
    // researchable.
    for (TechId prereq : t->prerequisites) {
        if (!prereq.valid()) continue;
        if (!g.content.tech_def(prereq)) return false;  // dangling prerequisite
        if (!c->research.has_tech(prereq)) return false;
    }
    return true;
}

double tech_cost_days(const Game& g, CountryId country, const TechId tech) {
    const TechDef* t = g.content.tech_def(tech);
    if (!t) return 0.0;
    const Country* c = g.world.country(country);
    const double base = t->cost_days > 0.0 ? t->cost_days : g.content.constants.research_base_days;
    const int years_ahead = t->year - g.world.date.year;
    const double penalty =
        ahead_of_time_penalty_per_year(g.content) * static_cast<double>(years_ahead > 0 ? years_ahead : 0);
    const double speed = c ? c->total_modifiers().get(ModifierKind::ResearchSpeed) : 0.0;
    const double days = safe_div(base * (1.0 + penalty), 1.0 + speed);
    if (!std::isfinite(days) || days <= 0.0) return base;
    return days;
}

void apply_tech_effects(Game& g, Country& country, TechId tech) {
    const TechDef* t = g.content.tech_def(tech);
    if (!t) return;
    if (country.research.has_tech(tech)) return;  // idempotent: modifiers apply once
    country.tech_modifiers.add(t->modifiers);
    country.research.completed.push_back(tech);
    // The unlocked equipment and building keys are not copied into country state:
    // equipment_unlocked()/building_unlocked() read them back from this list.
}

void phase_research(Game& g) {
    // Base research rate: one day of work per day, i.e. 1/24 of a day per tick.
    // ResearchSpeed is NOT applied here - ARCHITECTURE 5.3 puts it in the
    // denominator of `tech_cost_days`, and applying it in both places would square
    // the modifier.
    const double day_fraction = g.content.constants.research_speed_base *
                                static_cast<double>(TICKS_PER_HOUR) /
                                static_cast<double>(TICKS_PER_DAY);
    g.world.countries.for_each([&](CountryId id, Country& c) {
        if (!c.alive) return;
        for (ResearchSlot& slot : c.research.slots) {
            if (!slot.active) continue;
            const TechDef* t = g.content.tech_def(slot.tech);
            if (!t || c.research.has_tech(slot.tech)) {
                // Stale slot (unknown tech, or completed elsewhere): release it
                // rather than burning a research slot forever.
                slot.active = false;
                slot.tech = TechId{};
                slot.progress = 0.0;
                continue;
            }
            // The cost is re-read every hour, so a technology completed by an
            // earlier slot in this same loop already speeds up the later ones.
            const double needed = tech_cost_days(g, id, slot.tech);
            slot.progress += day_fraction;
            if (!std::isfinite(slot.progress)) slot.progress = 0.0;
            if (needed > 0.0 && slot.progress >= needed) {
                const std::string text = c.name + " completed " + t->name;
                apply_tech_effects(g, c, slot.tech);
                g.log_event("research", text, id);
                slot.active = false;
                slot.tech = TechId{};
                slot.progress = 0.0;
            }
        }
    });
}

bool building_unlocked(const Game& g, CountryId country, const std::string& building_key) {
    const Country* c = g.world.country(country);
    if (!c) return false;
    bool gated = false;
    for (size_t i = 0; i < g.content.techs.size(); ++i) {
        const TechDef& t = g.content.techs[i];
        for (const std::string& key : t.unlock_buildings) {
            if (key != building_key) continue;
            gated = true;
            if (c->research.has_tech(tech_id_at(g.content, i))) return true;
        }
    }
    // Nothing in the content database gates this building: it is available from
    // the start of the scenario (base infrastructure, factories, ...).
    return !gated;
}

bool equipment_unlocked(const Game& g, CountryId country, EquipmentId equipment) {
    const Country* c = g.world.country(country);
    const EquipmentDef* def = g.content.equipment_def(equipment);
    if (!c || !def) return false;
    // Archetypes are the abstract parent of the producible models, never built.
    if (def->is_archetype) return true;
    bool gated = false;
    for (size_t i = 0; i < g.content.techs.size(); ++i) {
        const TechDef& t = g.content.techs[i];
        for (const std::string& key : t.unlock_equipment) {
            if (key != def->key) continue;
            gated = true;
            if (c->research.has_tech(tech_id_at(g.content, i))) return true;
        }
    }
    return !gated;
}

}  // namespace hoi
