#pragma once
// Equipment archetypes, division templates, derived combat statistics and the
// modifier vocabulary shared by technology, laws, spirits and national data.

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"

namespace hoi {

// ------------------------------------------------------------- modifiers -----
//
// All modifiers are additive fractions applied as (1 + sum). They are summed per
// scope (country, division, battle) before being applied, so ordering never
// matters and results are reproducible.

enum class ModifierKind : uint8_t {
    FactoryOutput = 0,
    DockyardOutput,
    ConstructionSpeed,
    ResearchSpeed,
    DivisionOrganization,
    DivisionAttack,
    DivisionDefense,
    DivisionBreakthrough,
    DivisionRecoveryRate,
    SupplyConsumption,
    MaxPlanning,
    PlanningSpeed,
    ManpowerGrowth,
    PoliticalPowerGain,
    FuelGain,
    TrainingTime,
    EquipmentCostFactor,
    RecruitablePopulation,
    Stability,
    WarSupport,
    EntrenchmentSpeed,
    CombatWidth,
    ConvoyDefense,
    Count
};

const char* modifier_kind_name(ModifierKind k);

struct Modifiers {
    double v[static_cast<int>(ModifierKind::Count)] = {};

    void add(const Modifiers& o) {
        for (int i = 0; i < static_cast<int>(ModifierKind::Count); ++i) v[i] += o.v[i];
    }
    void set(ModifierKind k, double value) { v[static_cast<int>(k)] = value; }
    void add(ModifierKind k, double value) { v[static_cast<int>(k)] += value; }
    [[nodiscard]] double get(ModifierKind k) const { return v[static_cast<int>(k)]; }
    [[nodiscard]] double factor(ModifierKind k) const { return 1.0 + v[static_cast<int>(k)]; }
    [[nodiscard]] bool empty() const;
};

// ------------------------------------------------------------- equipment -----

struct EquipmentDef {
    EquipmentId id;
    std::string key;   // stable data key, e.g. "infantry_equipment_1"
    std::string name;
    EquipmentCategory category = EquipmentCategory::Infantry;
    int year = 1936;
    std::string archetype;  // parent archetype key ("infantry_equipment")

    // Combat statistics (zero for categories that do not use them).
    double soft_attack = 0.0;
    double hard_attack = 0.0;
    double air_attack = 0.0;
    double air_defence = 0.0;
    double ground_attack = 0.0;  // damage dealt to land divisions (CAS, bombers)
    double agility = 0.0;
    double range = 0.0;  // strategic-region hops a wing can operate from its base
    double defense = 0.0;
    double breakthrough = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double hardness = 0.0;   // 0..1
    double reliability = 1.0;  // 1.0 = no attrition
    double speed = 0.0;      // km/h, division speed is the minimum over battalions
    double max_strength = 1.0;  // HP contributed per unit
    double organization = 0.0;  // organisation contributed per battalion

    // Economics.
    double build_cost = 0.0;  // industrial capacity per unit
    double resources[RESOURCE_COUNT] = {};
    double fuel_use = 0.0;
    double supply_use = 0.0;
    double manpower = 0.0;  // persons per unit
    bool is_archetype = false;
};

// -------------------------------------------------------------- templates ----

struct BattalionSlot {
    EquipmentId equipment;  // line equipment for this battalion
    int count = 0;
    bool support = false;  // support company (no width, 1 per template)
};

struct DivisionTemplate {
    TemplateId id;
    std::string key;
    std::string name;
    CountryId country;  // owner (templates are country data)
    std::vector<BattalionSlot> battalions;

    // Derived statistics (recomputed whenever composition or equipment changes).
    double combat_width = 0.0;
    double max_organization = 0.0;
    double max_strength = 0.0;  // HP
    double soft_attack = 0.0;
    double hard_attack = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double hardness = 0.0;
    double speed = 0.0;
    double supply_use = 0.0;
    double fuel_use = 0.0;
    double manpower = 0.0;
    double build_cost = 0.0;
    double train_days = 90.0;

    [[nodiscard]] int battalion_count() const {
        int n = 0;
        for (const auto& b : battalions) n += b.support ? 0 : b.count;
        return n;
    }
};

// Recomputes all derived template statistics from its battalion composition and
// the current equipment definitions. Called on creation and on any edit.
void recompute_template_stats(DivisionTemplate& t, const std::vector<EquipmentDef>& equipment);

// Current statistics of a division instance: template stats scaled by equipment
// actually present, plus organisation modifiers. Written into `out`.
struct DivisionStats {
    double soft_attack = 0.0;
    double hard_attack = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double hardness = 0.0;
    double combat_width = 0.0;
    double max_organization = 0.0;
    double max_strength = 0.0;
    double speed = 0.0;
    double supply_use = 0.0;
    double fuel_use = 0.0;
    double max_manpower = 0.0;
};

}  // namespace hoi
