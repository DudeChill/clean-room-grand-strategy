// Intelligence: what a country knows about another, what it can do about it, and how
// that knowledge changes a battle.
//
// The model is small on purpose and every number is data (Content::operations and
// Content::agency_upgrades). A country owns an agency (upgrades), keeps a spy network in
// other countries (strength 0..100), runs operations against them, and decrypts their
// ciphers. Knowledge is never perfect: intel and decryption are both capped below 1, so a
// player always has something left to guess.
//
// Everything here is derived from state - nothing caches a level, so a phase cannot
// disagree with what the UI shows.

#pragma once

#include <cstdint>

#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

// How much `viewer` knows about `target`, 0..1. Built from the spy network's strength and
// decrypted ciphers, with the network worth more (kIntelNetworkWeight vs
// kIntelCryptoWeight), and never reaching 1.0.
double intel_level(const Game& g, CountryId viewer, CountryId target);

// Decryption progress against `target`'s ciphers, 0..1.
double decryption_level(const Game& g, CountryId viewer, CountryId target);

// How hard this country is to spy on, 0..1: its own counter-intelligence (agency upgrades
// plus any national modifier), which cuts an enemy's network growth at home.
double counter_intel_level(const Game& g, CountryId country);

// Strength of `owner`'s network inside `target`, 0 when there is none.
double network_strength(const Game& g, CountryId owner, CountryId target);

// Attack bonus an attacker gets from knowing the defender, as a fraction (0.05 = +5%).
// Applied where a battle is resolved; a country that knows nothing gets 0.
double intel_attack_bonus(const Game& g, CountryId attacker, CountryId defender);

// Fraction of the attacker's planning bonus that broken ciphers deny (0..1): reading the
// enemy's traffic is what makes a prepared offensive lose its surprise.
double decryption_planning_penalty(const Game& g, CountryId attacker, CountryId defender);

// Command entry points. They validate and mutate only on success, so a rejected command
// leaves no trace. `operation_key` / `upgrade_key` name content entries.
bool intel_start_operation(Game& g, CountryId country, CountryId target,
                           const std::string& operation_key);
bool intel_cancel_operation(Game& g, CountryId country, CountryId target,
                            const std::string& operation_key);
bool intel_buy_upgrade(Game& g, CountryId country, const std::string& upgrade_key);
// Whether this operation may be started against this target right now (availability
// trigger plus kind-specific prerequisites).
bool intel_operation_available(const Game& g, CountryId country, CountryId target,
                               uint32_t operation);
// Whether this agency upgrade may be bought now (year, technology, trigger, prerequisites).
bool intel_upgrade_available(const Game& g, CountryId country, uint32_t upgrade);

// Advance networks, decay exposure, progress operations, apply completed effects and, progress operations, apply completed effects and
// accrue decryption. Deterministic: countries and targets ascend, and every roll uses the
// country's intel RNG stream.
void phase_intelligence(Game& g);

// The AI's agency policy: buy upgrades it can afford, keep networks in the countries it
// worries about, run operations when a network is strong enough. Acts only through the
// command queue and records AiReason factors.
void ai_intelligence_layer(Game& g, Country& c);

}  // namespace hoi