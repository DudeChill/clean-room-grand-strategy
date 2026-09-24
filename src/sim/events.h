#pragma once
// Events and decisions (spec sections 58, 59): data-driven triggers, options and
// effects, plus the timers and AI evaluation decisions need.

#include <string>
#include <vector>

#include "core/types.h"
#include "data/content.h"

namespace hoi {

struct Game;

// Index into Content::events / Content::decisions.
using EventIndex = uint32_t;
using DecisionIndex = uint32_t;

// Fires an event for a country: the country receives it in its pending list and must
// choose an option (a human by command, the AI by weight). `fire_only_once` events are
// tracked per country.
void fire_event(Game& g, CountryId country, EventIndex event);
void fire_event_delayed(Game& g, CountryId country, EventIndex event, int days);

// Schedules an event by key (used by effects: {"trigger_event": {"key": "...", "days": 7}}).
bool fire_event_by_key(Game& g, CountryId country, const std::string& key, int days);

// Applies an option of a pending event and removes it from the pending list.
bool choose_event_option(Game& g, CountryId country, EventIndex event, int option);

// Decisions: a decision is visible and available when its triggers pass; taking it
// deducts political power, applies its effects and starts its removal timer.
bool decision_visible(const Game& g, CountryId country, DecisionIndex decision);
bool decision_available(const Game& g, CountryId country, DecisionIndex decision);
bool decision_take(Game& g, CountryId country, DecisionIndex decision);
void decision_cancel(Game& g, CountryId country, DecisionIndex decision);

// Per-day work: fire due delayed events, evaluate event triggers for every country,
// tick decision timers and re-evaluate decision availability.
void phase_events(Game& g);

// AI: choose pending event options by weight and take decisions that score well.
void ai_event_layer(Game& g, Country& c);
void ai_decision_layer(Game& g, Country& c);

}  // namespace hoi
