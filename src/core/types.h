#pragma once
// Core identity and time types for the simulation.
// All entity references use strongly-typed 32-bit indices so that serialization
// never depends on pointers or memory layout.

#include <cstdint>
#include <limits>
#include <string>

namespace hoi {

inline constexpr uint32_t INVALID_ID = 0xFFFFFFFFu;

template <typename Tag>
struct Id {
    uint32_t v = INVALID_ID;

    constexpr Id() = default;
    constexpr explicit Id(uint32_t x) : v(x) {}

    [[nodiscard]] constexpr bool valid() const { return v != INVALID_ID; }
    [[nodiscard]] constexpr uint32_t raw() const { return v; }

    friend constexpr bool operator==(Id a, Id b) { return a.v == b.v; }
    friend constexpr bool operator!=(Id a, Id b) { return a.v != b.v; }
    friend constexpr bool operator<(Id a, Id b) { return a.v < b.v; }
    friend constexpr bool operator>(Id a, Id b) { return a.v > b.v; }
    friend constexpr bool operator<=(Id a, Id b) { return a.v <= b.v; }
    friend constexpr bool operator>=(Id a, Id b) { return a.v >= b.v; }
};

struct ProvinceTag {};
struct StateTag {};
struct RegionTag {};
struct CountryTag {};
struct DivisionTag {};
struct ArmyTag {};
struct BattleTag {};
struct WarTag {};
struct FleetTag {};
struct ShipTag {};
struct TaskForceTag {};
struct AirWingTag {};
struct CharacterTag {};
struct EquipmentTag {};
struct TemplateTag {};
struct TechTag {};

using ProvinceId = Id<ProvinceTag>;
using StateId = Id<StateTag>;
using RegionId = Id<RegionTag>;
using CountryId = Id<CountryTag>;
using DivisionId = Id<DivisionTag>;
using ArmyId = Id<ArmyTag>;
using BattleId = Id<BattleTag>;
using WarId = Id<WarTag>;
using FleetId = Id<FleetTag>;
using ShipId = Id<ShipTag>;
using TaskForceId = Id<TaskForceTag>;
using AirWingId = Id<AirWingTag>;
using CharacterId = Id<CharacterTag>;
using EquipmentId = Id<EquipmentTag>;
using TemplateId = Id<TemplateTag>;
using TechId = Id<TechTag>;

// Simulation time: one tick is one game hour. 24 ticks per day.
using Tick = uint64_t;
inline constexpr int TICKS_PER_DAY = 24;
inline constexpr int TICKS_PER_HOUR = 1;

struct GameDate {
    int32_t year = 1936;
    uint8_t month = 1;  // 1..12
    uint8_t day = 1;    // 1..31
    uint8_t hour = 0;   // 0..23

    friend bool operator==(const GameDate& a, const GameDate& b) {
        return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour;
    }
};

// Days since a fixed epoch; used for elapsed-day arithmetic.
int64_t date_to_days(const GameDate& d);
GameDate days_to_date(int64_t days);
GameDate tick_to_date(Tick tick, const GameDate& start);
Tick date_to_tick(const GameDate& d, const GameDate& start);

bool is_leap_year(int32_t year);
int days_in_month(int32_t year, int month);

enum class Resource : uint8_t {
    Oil = 0,
    Steel,
    Aluminium,
    Rubber,
    Chromium,
    Tungsten,
    Count
};
inline constexpr int RESOURCE_COUNT = static_cast<int>(Resource::Count);

const char* resource_name(Resource r);

// Equipment categories drive production, stat aggregation and stockpile grouping.
enum class EquipmentCategory : uint8_t {
    Infantry = 0,
    Support,
    Artillery,
    AntiTank,
    AntiAir,
    Motorized,
    Mechanized,
    Armor,
    Aircraft,
    Ship,
    Convoy,
    Count
};

const char* equipment_category_name(EquipmentCategory c);

enum class Ideology : uint8_t {
    Democratic = 0,
    Fascist,
    Communist,
    Neutrality,
    Count
};

const char* ideology_name(Ideology i);

// Terrain drives movement cost, combat penalties and air/naval modifiers.
enum class Terrain : uint8_t {
    Plains = 0,
    Forest,
    Hills,
    Mountain,
    Urban,
    Marsh,
    Desert,
    Jungle,
    Ocean,
    ShallowSea,
    DeepOcean,
    Lakes,
    Count
};

const char* terrain_name(Terrain t);
bool terrain_is_water(Terrain t);

}  // namespace hoi
