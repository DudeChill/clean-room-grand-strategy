// Core time arithmetic and enum names.
//
// Epoch contract: day 0 is 1900-01-01. The proleptic Gregorian calendar is used
// for all years, including those before 1900, so date arithmetic is total and
// reversible across the scenario date range.

#include "core/types.h"

namespace hoi {

bool is_leap_year(int32_t year) {
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int32_t year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month - 1];
}

int64_t date_to_days(const GameDate& d) {
    // Whole years first (signed, so pre-1900 dates stay consistent), then
    // months and days within the year. The day part is 0-based for day 1.
    int64_t days = 0;
    for (int32_t y = 1900; y < d.year; ++y) days += is_leap_year(y) ? 366 : 365;
    for (int32_t y = d.year; y < 1900; ++y) days -= is_leap_year(y) ? 366 : 365;
    for (int m = 1; m < static_cast<int>(d.month); ++m) days += days_in_month(d.year, m);
    days += static_cast<int64_t>(d.day) - 1;
    return days;
}

GameDate days_to_date(int64_t days) {
    GameDate d;
    int32_t year = 1900;
    int64_t rem = days;
    while (rem < 0) {
        --year;
        rem += is_leap_year(year) ? 366 : 365;
    }
    while (rem >= (is_leap_year(year) ? 366 : 365)) {
        rem -= is_leap_year(year) ? 366 : 365;
        ++year;
    }
    int month = 1;
    while (rem >= days_in_month(year, month)) {
        rem -= days_in_month(year, month);
        ++month;
    }
    d.year = year;
    d.month = static_cast<uint8_t>(month);
    d.day = static_cast<uint8_t>(rem + 1);
    d.hour = 0;
    return d;
}

GameDate tick_to_date(Tick tick, const GameDate& start) {
    // Total hours since the epoch day, offset by the start's hour so a start date
    // with hour != 0 advances the clock correctly.
    const int64_t total_hours = date_to_days(start) * 24 + static_cast<int64_t>(start.hour) +
                                static_cast<int64_t>(tick);
    GameDate d = days_to_date(total_hours / 24);
    d.hour = static_cast<uint8_t>(((total_hours % 24) + 24) % 24);
    return d;
}

Tick date_to_tick(const GameDate& d, const GameDate& start) {
    const int64_t target = date_to_days(d) * 24 + static_cast<int64_t>(d.hour);
    const int64_t origin = date_to_days(start) * 24 + static_cast<int64_t>(start.hour);
    // Dates before the scenario start have no tick; clamp rather than wrap a
    // uint64 into a huge forward tick.
    if (target <= origin) return 0;
    return static_cast<Tick>(target - origin);
}

const char* resource_name(Resource r) {
    switch (r) {
        case Resource::Oil: return "oil";
        case Resource::Steel: return "steel";
        case Resource::Aluminium: return "aluminium";
        case Resource::Rubber: return "rubber";
        case Resource::Chromium: return "chromium";
        case Resource::Tungsten: return "tungsten";
        case Resource::Count: break;
    }
    return "unknown";
}

const char* equipment_category_name(EquipmentCategory c) {
    switch (c) {
        case EquipmentCategory::Infantry: return "infantry";
        case EquipmentCategory::Support: return "support";
        case EquipmentCategory::Artillery: return "artillery";
        case EquipmentCategory::AntiTank: return "anti_tank";
        case EquipmentCategory::AntiAir: return "anti_air";
        case EquipmentCategory::Motorized: return "motorized";
        case EquipmentCategory::Mechanized: return "mechanized";
        case EquipmentCategory::Armor: return "armor";
        case EquipmentCategory::Aircraft: return "aircraft";
        case EquipmentCategory::Ship: return "ship";
        case EquipmentCategory::Convoy: return "convoy";
        case EquipmentCategory::Count: break;
    }
    return "unknown";
}

const char* ideology_name(Ideology i) {
    switch (i) {
        case Ideology::Democratic: return "democratic";
        case Ideology::Fascist: return "fascist";
        case Ideology::Communist: return "communist";
        case Ideology::Neutrality: return "neutrality";
        case Ideology::Count: break;
    }
    return "unknown";
}

const char* terrain_name(Terrain t) {
    switch (t) {
        case Terrain::Plains: return "plains";
        case Terrain::Forest: return "forest";
        case Terrain::Hills: return "hills";
        case Terrain::Mountain: return "mountain";
        case Terrain::Urban: return "urban";
        case Terrain::Marsh: return "marsh";
        case Terrain::Desert: return "desert";
        case Terrain::Jungle: return "jungle";
        case Terrain::Ocean: return "ocean";
        case Terrain::ShallowSea: return "shallow_sea";
        case Terrain::DeepOcean: return "deep_ocean";
        case Terrain::Lakes: return "lakes";
        case Terrain::Count: break;
    }
    return "unknown";
}

bool terrain_is_water(Terrain t) {
    switch (t) {
        case Terrain::Ocean:
        case Terrain::ShallowSea:
        case Terrain::DeepOcean:
        case Terrain::Lakes:
            return true;
        default:
            return false;
    }
}

}  // namespace hoi
