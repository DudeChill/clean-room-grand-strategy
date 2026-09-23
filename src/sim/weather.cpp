// Weather per strategic region (spec section 65, ARCHITECTURE 5.8).
//
// Weather is re-evaluated once per game day from the RNG_WEATHER stream. The result
// lives in Region fields (temperature, rain, snow, mud, sandstorm); the movement and
// combat phases read those fields, and the pure multiplier helpers at the bottom of
// this file are the canonical numbers for doing so. Weather never mutates anything
// outside Region, so it can be replayed or skipped without touching world state.

#include <cmath>

#include "core/math.h"
#include "core/rng.h"
#include "core/types.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/politics.h"

namespace hoi {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSnowTemperature = 0.5;   // at or below this, precipitation falls as snow
constexpr double kFrozenTemperature = 0.0; // ground freezes: rain stops making mud
constexpr double kMinTemperature = -60.0;
constexpr double kMaxTemperature = 50.0;

constexpr double kRainChance = 0.35;
constexpr double kSnowChance = 0.45;
constexpr double kSandstormThreshold = 0.90;

int days_in_month_of(int32_t year, int month) {
    static const int table[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return table[month - 1];
}

// Day of the year, 0-based.
double day_of_year(const GameDate& d) {
    double doy = static_cast<double>(d.day) - 1.0;
    for (int m = 1; m < d.month; ++m) doy += static_cast<double>(days_in_month_of(d.year, m));
    return doy;
}

// Latitude proxy: region slots map linearly from +70 (first) to -70 (last). The map
// format has no latitude field, so the slot order in the map file is the proxy.
double latitude_proxy(RegionId id, size_t region_slots) {
    if (region_slots <= 1) return 0.0;
    const double t = static_cast<double>(id.v) / static_cast<double>(region_slots - 1);
    return 70.0 - 140.0 * clamp(t, 0.0, 1.0);
}

// Seasonal temperature: sinusoidal year with a hemisphere sign so the southern
// hemisphere is cold in June. std::cos is the one transcendental on this path; it is
// deterministic for a fixed libm and the world hash catches any drift.
double seasonal_temperature(double latitude, const GameDate& date) {
    const double doy = day_of_year(date);
    const double phase = std::cos(2.0 * kPi * (doy - 172.0) / 365.0);  // +1 at June solstice
    const double abs_lat = std::fabs(latitude);
    const double base = 28.0 - 0.45 * abs_lat;
    const double amplitude = 4.0 + 0.22 * abs_lat;
    const double hemisphere = latitude >= 0.0 ? 1.0 : -1.0;
    return clamp(base + amplitude * phase * hemisphere, kMinTemperature, kMaxTemperature);
}

bool region_has_terrain(const World& w, const Region& r, Terrain a, Terrain b, Terrain c) {
    for (ProvinceId pid : r.provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr) continue;
        if (p->terrain == a || p->terrain == b || p->terrain == c) return true;
    }
    return false;
}

bool muddy_terrain(const World& w, const Region& r) {
    return region_has_terrain(w, r, Terrain::Marsh, Terrain::Jungle, Terrain::Forest);
}

bool desert_terrain(const World& w, const Region& r) {
    return region_has_terrain(w, r, Terrain::Desert, Terrain::Desert, Terrain::Desert);
}

}  // namespace

// ------------------------------------------------------------ effect helpers --
//
// Movement and combat read Region::rain/snow/mud/sandstorm directly; these helpers
// are the canonical multipliers for those fields (movement cost multiplier,
// additive attack penalty, daily attrition share). They are pure and have no state.

double weather_movement_multiplier(const Region& r) {
    double m = 1.0;
    if (r.rain) m += 0.15;
    if (r.snow) m += 0.25;
    if (r.mud) m += 0.35;
    if (r.sandstorm) m += 0.25;
    return clamp(m, 1.0, 1.75);
}

double weather_attack_penalty(const Region& r) {
    double p = 0.0;
    if (r.rain) p += 0.05;
    if (r.snow) p += 0.10;
    if (r.mud) p += 0.15;
    if (r.sandstorm) p += 0.10;
    // Extreme cold degrades attackers without any precipitation.
    if (r.temperature <= -20.0) p += 0.10;
    return clamp(p, 0.0, 0.50);
}

double weather_attrition_per_day(const Region& r) {
    if (r.mud) return 0.010;
    if (r.snow) return 0.015;
    if (r.sandstorm) return 0.005;
    return 0.0;
}

void phase_weather(Game& g) {
    World& w = g.world;
    // Daily granularity: the phase is a no-op outside the first hour of a day.
    if (w.tick % static_cast<Tick>(TICKS_PER_DAY) != 0) return;

    const SimConstants& k = g.content.constants;
    const double change_chance = clamp(k.weather_change_chance, 0.0, 1.0);
    Rng& rng = g.rng.get(RngStream::Weather);
    const size_t slots = w.regions.capacity();

    w.regions.for_each([&](RegionId rid, Region& r) {
        // Sea regions carry no land weather; they take no draws either.
        if (r.is_sea) {
            r.temperature = 15.0;
            r.rain = false;
            r.snow = false;
            r.mud = false;
            r.sandstorm = false;
            return;
        }

        const double temperature = seasonal_temperature(latitude_proxy(rid, slots), w.date);
        r.temperature = std::isfinite(temperature) ? temperature : 15.0;

        // Exactly two draws per land region per day, taken before any branch, so the
        // stream stays aligned regardless of which weather is chosen.
        double roll_change = rng.next_double();
        double roll_type = rng.next_double();
        if (!std::isfinite(roll_change)) roll_change = 0.0;
        if (!std::isfinite(roll_type)) roll_type = 0.0;

        bool raining = r.rain;
        bool snowing = r.snow;
        if (roll_change < change_chance) {
            if (r.temperature <= kSnowTemperature) {
                snowing = roll_type < kSnowChance;
                raining = false;
            } else {
                raining = roll_type < kRainChance;
                snowing = false;
            }
        } else if (r.temperature <= kSnowTemperature && raining) {
            raining = false;
            snowing = true;  // the front moved below freezing
        } else if (r.temperature > kSnowTemperature && snowing) {
            snowing = false;
            raining = true;
        }

        r.rain = raining;
        r.snow = snowing;
        r.mud = raining && r.temperature > kFrozenTemperature && muddy_terrain(w, r);
        r.sandstorm = desert_terrain(w, r) && !raining && !snowing &&
                      roll_type >= kSandstormThreshold;
    });
}

}  // namespace hoi
