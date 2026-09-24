#pragma once
// Equipment archetypes, division templates, derived combat statistics and the
// modifier vocabulary shared by technology, laws, spirits and national data.

#include <cstdint>
#include <string>
#include <vector>

#include "core/json.h"
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
    // Naval statistics (zero for non-ship categories).
    double naval_attack = 0.0;    // guns: surface damage
    double torpedo_attack = 0.0;  // torpedoes: damage against large hulls
    double sub_detection = 0.0;   // anti-submarine capability
    double detection = 0.0;       // how well this ship spots others
    double visibility = 1.0;      // how easily it is spotted (higher = easier)
    double armor = 0.0;
    double piercing = 0.0;
    double hardness = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;   // 0..1
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

// ------------------------------------------------------------ components ----

// A designer slot: which part of a design a component fills. Categories use the slots
// that make sense for them (armour designs use Armor/Weapon/Engine/Special, aircraft
// Airframe/Engine/Weapon/Special, ships Hull/Weapon/Engine/Armor).
enum class ComponentSlot : uint8_t {
    Armor = 0,
    Weapon,
    Engine,
    Airframe,
    Hull,
    Special,
    Count
};

const char* component_slot_name(ComponentSlot s);
bool component_slot_from_name(const std::string& name, ComponentSlot* out);

// A component a country can fit into a design. Deltas are additive except the
// multipliers, which scale the archetype's value.
struct ComponentDef {
    uint32_t index = 0;
    std::string key;
    std::string name;
    ComponentSlot slot = ComponentSlot::Special;
    EquipmentCategory category = EquipmentCategory::Armor;  // equipment family it fits
    int year = 1936;
    double soft_attack = 0.0;
    double hard_attack = 0.0;
    double air_attack = 0.0;
    double air_defence = 0.0;
    double ground_attack = 0.0;
    double agility = 0.0;
    double armor = 0.0;
    double piercing = 0.0;
    double defense = 0.0;
    double breakthrough = 0.0;
    double hardness = 0.0;
    double max_strength = 0.0;
    double organization = 0.0;
    double speed = 0.0;
    double reliability = 0.0;
    double range = 0.0;
    double detection = 0.0;
    double sub_detection = 0.0;
    double naval_attack = 0.0;
    double torpedo_attack = 0.0;
    double visibility = 0.0;
    double build_cost_add = 0.0;
    double cost_multiplier = 1.0;
    double resources[RESOURCE_COUNT] = {};
    double fuel_use = 0.0;
    double supply_use = 0.0;
    double manpower = 0.0;
    // Optional gate: a script trigger that must pass for the component to be fittable
    // (null = always available), evaluated by the script engine in a country scope.
    // Technologies may also unlock a component by naming its key in unlock_equipment.
    Json available;
};

// A country's variant of an equipment archetype. The engine computes its statistics
// from the archetype plus the fitted components and registers the result as a real
// EquipmentDef, so production, stockpiles and combat treat a design exactly like a
// pre-authored model.
struct EquipmentDesign {
    uint32_t index = 0;
    std::string key;
    std::string name;
    CountryId country;
    EquipmentId archetype;  // base EquipmentDef
    int year = 1936;
    std::vector<std::pair<ComponentSlot, uint32_t>> components;  // slot -> ComponentDef index
    EquipmentId produced;  // EquipmentDef registered for this design
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
