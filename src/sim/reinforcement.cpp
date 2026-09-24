// Continuous replacement (MIL-021).
//
// A division below strength draws the model the country issues for each slot's family
// from the stockpile. This is the link that turns production into fielded capability:
// without it a newly designed model piles up in the depot while the army keeps the
// 1936 rifle, and "research -> design -> produce -> better divisions" never completes.
//
// Rules:
//  * The phase runs after production, so a line's output of this tick is already in the
//    depot and the preferred model is resolved against a settled stockpile.
//  * A slot is satisfied by any model of its family: the model the country is building
//    when it has a live line (that is what an army issues), otherwise the best model in
//    the depot, otherwise the best fieldable member. `preferred_slot_model` owns that
//    order; this phase only decides how much may move.
//  * Replacement is gradual, not teleportation: a division draws at most a share of its
//    shortfall per hour (kReinforcementSharePerHour, floor kReinforcementMinPerHour).
//  * Supply gates replacement, so a cut-off or encircled division does not refill - that
//    is what makes encirclement bite.
//  * No new state: the phase only moves equipment and strength, so the save format and
//    the world hash layout are unchanged.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "game/game.h"
#include "core/math.h"
#include "sim/industry.h"
#include "sim/phases.h"
#include "sim/units.h"

namespace hoi {
namespace {

// Share of a slot's requirement a division may draw in one simulated hour.
constexpr double kReinforcementSharePerHour = 0.02;
// Smallest draw per hour, so a division one rifle short still gets it that day.
constexpr double kReinforcementMinPerHour = 0.25;

}  // namespace

void phase_reinforcement(Game& g) {
    World& w = g.world;
    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        if (c.divisions.empty() && c.training.empty()) return;
        // Content can grow at runtime (a design registers a new model), so the
        // stockpile table is grown before anything indexes it.
        if (c.equipment_stockpile.size() < g.content.equipment.size()) {
            c.equipment_stockpile.resize(g.content.equipment.size(), 0.0);
        }

        // The model a country issues per family is a property of the country and its
        // live lines, not of a division: resolve it once per family per tick.
        std::vector<std::pair<std::string, EquipmentId>> resolved;

        for (DivisionId did : c.divisions) {
            Division* d = w.division(did);
            if (!d || d->in_training()) continue;
            if (!(d->strength < 1.0)) continue;  // at full strength: nothing to replace
            const DivisionTemplate* t = g.content.template_def(d->template_id);
            if (!t) continue;
            const double supply = clamp01(d->supply);
            if (!(supply > 0.0)) continue;

            for (const BattalionSlot& b : t->battalions) {
                if (!b.equipment.valid() || b.count <= 0) continue;
                const std::string family = slot_family(g.content, b.equipment);
                if (family.empty()) continue;
                if (d->equipment.size() < g.content.equipment.size()) {
                    d->equipment.resize(g.content.equipment.size(), 0.0);
                }

                // Requirement and holdings of the whole family: a division may not end
                // up with more gear than its template asks for, however many models
                // fill that family.
                double required = 0.0;
                for (const BattalionSlot& other : t->battalions) {
                    if (!other.equipment.valid() || other.count <= 0) continue;
                    if (slot_family(g.content, other.equipment) != family) continue;
                    required += static_cast<double>(other.count);
                }
                if (!(required > 0.0)) continue;
                double present = 0.0;
                for (size_t i = 0; i < d->equipment.size() && i < g.content.equipment.size(); ++i) {
                    if (!(d->equipment[i] > 0.0)) continue;
                    const EquipmentDef* def = g.content.equipment_def(
                        EquipmentId(static_cast<uint32_t>(i)));
                    if (!def || def->is_archetype || def->archetype != family) continue;
                    present += d->equipment[i];
                }
                const double missing = required - present;
                if (!(missing > 0.0)) continue;

                EquipmentId model;
                for (const std::pair<std::string, EquipmentId>& entry : resolved) {
                    if (entry.first == family) {
                        model = entry.second;
                        break;
                    }
                }
                if (!model.valid()) {
                    const EquipmentId producing = family_production_model(g, cid, b.equipment);
                    model = preferred_slot_model(g, cid, b.equipment, producing);
                    resolved.emplace_back(family, model);
                }
                if (!model.valid()) continue;
                double draw = missing * kReinforcementSharePerHour * supply;
                if (draw < kReinforcementMinPerHour) draw = kReinforcementMinPerHour;
                if (draw > missing) draw = missing;

                // The preferred model is what the country issues, but a line ramping up
                // cannot feed a whole army by itself. Take what it has and make up the
                // rest from whatever else of that family is in the depot, so a division
                // never starves next to usable gear. The preference itself stays
                // unchanged, so this cannot oscillate between models.
                double remaining = draw;
                const double preferred_stock =
                    model.v < c.equipment_stockpile.size() ? c.equipment_stockpile[model.v] : 0.0;
                const double taken = std::min(remaining, std::max(0.0, preferred_stock));
                // reinforce_division moves the gear, credits the division and adjusts
                // strength and manpower; this phase only decides who draws and how much.
                if (taken > 0.0) {
                    remaining -= reinforce_division(g, *d, model, taken);
                }
                if (remaining > 0.0) {
                    const EquipmentId stocked =
                        preferred_slot_model(g, cid, b.equipment, EquipmentId{});
                    if (stocked.valid() && !(stocked == model)) {
                        reinforce_division(g, *d, stocked, remaining);
                    } else if (taken <= 0.0) {
                        reinforce_division(g, *d, model, remaining);
                    }
                }
            }
        }
    });
}

}  // namespace hoi
