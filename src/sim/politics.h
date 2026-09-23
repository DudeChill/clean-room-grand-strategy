#pragma once
// Politics: political power, laws, stability, war support, manpower (spec section 56).

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// Manpower pool growth for one day from controlled states.
double daily_manpower_gain(const Game& g, CountryId country);

// Political power gained per day (base plus modifiers).
double daily_political_power(const Game& g, CountryId country);

// Applies a law level change: cost check happens in command validation; this
// recomputes the country's law modifiers.
void apply_law_change(Game& g, Country& country, int law_kind, int level);

// Recomputes law_modifiers from law_levels.
void refresh_law_modifiers(Game& g, Country& country);

// Per-tick politics update: PP, stability, war support, manpower growth,
// consumer goods ratio from laws and war state.
void phase_politics(Game& g);

// Weather per strategic region (spec section 65): affects movement, combat and
// attrition. Re-evaluated daily with the RNG_WEATHER stream.
void phase_weather(Game& g);

// Weather effects consumed by movement and combat (implemented in weather.cpp).
double weather_movement_multiplier(const Region& r);
double weather_attack_penalty(const Region& r);
double weather_attrition_per_day(const Region& r);

}  // namespace hoi
