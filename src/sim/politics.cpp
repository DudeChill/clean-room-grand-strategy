// Politics: political power, laws, stability, war support, manpower
// (spec section 56, ARCHITECTURE 5.7).
//
// Everything here is driven by content data (laws, constants) and the war state, so
// balance is tunable without touching code. All iteration is ascending id.

#include "sim/politics.h"

#include <cmath>
#include <vector>

#include "core/math.h"
#include "data/content.h"
#include "game/game.h"
#include "sim/diplomacy.h"

namespace hoi {
namespace {

// Stability and war support fall while the war goes badly, so the two modifiers of
// the losing side's home front are explicitly represented instead of implied.
constexpr double kWarStabilityTargetPenalty = 0.10;
constexpr double kWarSupportTargetBonus = 0.10;
constexpr double kCapitalLostStabilityPenalty = 0.20;
// A war economy releases civilian output: consumer goods demand drops.
constexpr double kWarConsumerGoodsRelief = 0.10;

bool participates(const War& war, CountryId c) {
    for (const WarParticipant& p : war.attackers)
        if (p.country == c) return true;
    for (const WarParticipant& p : war.defenders)
        if (p.country == c) return true;
    return false;
}

bool country_at_war(const World& w, CountryId c) {
    bool found = false;
    w.wars.for_each([&](WarId, const War& war) {
        if (found || !war.active) return;
        if (participates(war, c)) found = true;
    });
    return found;
}

// True when an enemy of `c` holds the country's capital province.
bool capital_occupied(const World& w, CountryId c) {
    const Country* country = w.country(c);
    if (country == nullptr || !country->capital.valid()) return false;
    const State* cap = w.state(country->capital);
    if (cap == nullptr) return false;
    for (ProvinceId pid : cap->provinces) {
        const Province* p = w.province(pid);
        if (p == nullptr || p->is_sea) continue;
        if (!p->is_capital) continue;
        if (!p->controller.valid() || p->controller == c) return false;
        return countries_at_war(w, c, p->controller);
    }
    return false;
}

double state_population(const World& w, const State& s) {
    double total = 0.0;
    for (ProvinceId pid : s.provinces) {
        const Province* p = w.province(pid);
        if (p != nullptr && std::isfinite(p->population) && p->population > 0.0) {
            total += p->population;
        }
    }
    return total;
}

// Recruitable share of the population for `c`: the data baseline scaled by the
// RecruitablePopulation modifier sum (ARCHITECTURE 5.7).
double recruitable_share(const Country& c, const SimConstants& k) {
    const double mod = c.total_modifiers().get(ModifierKind::RecruitablePopulation);
    const double share = k.recruitable_base * (1.0 + mod);
    return share > 0.0 ? share : 0.0;
}

// Daily manpower the given population yields for `c`.
double manpower_from_population(const Country& c, const SimConstants& k, double population) {
    const double growth = 1.0 + c.total_modifiers().get(ModifierKind::ManpowerGrowth);
    const double recruit = recruitable_share(c, k);
    return safe_div(population * recruit * (growth > 0.0 ? growth : 0.0), 365.0);
}

}  // namespace

double daily_manpower_gain(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return 0.0;

    double total = 0.0;
    g.world.states.for_each([&](StateId, const State& s) {
        if (s.controller != country) return;
        total += manpower_from_population(*c, g.content.constants, state_population(g.world, s));
    });
    return std::isfinite(total) ? total : 0.0;
}

double daily_political_power(const Game& g, CountryId country) {
    const Country* c = g.world.country(country);
    if (c == nullptr || !c->alive) return 0.0;
    const double gain = 1.0 + c->total_modifiers().get(ModifierKind::PoliticalPowerGain);
    const double pp = g.content.constants.political_power_per_day * (gain > 0.0 ? gain : 0.0);
    return std::isfinite(pp) ? pp : 0.0;
}

void refresh_law_modifiers(Game& g, Country& country) {
    country.law_modifiers = Modifiers{};
    const int kinds = static_cast<int>(country.law_levels.size());
    for (const LawDef& law : g.content.laws) {  // content order: deterministic
        if (law.kind < 0 || law.kind >= kinds) continue;
        if (static_cast<int>(country.law_levels[law.kind]) != law.level) continue;
        country.law_modifiers.add(law.modifiers);
    }
}

void apply_law_change(Game& g, Country& country, int law_kind, int level) {
    if (law_kind < 0 || level < 0) return;
    if (country.law_levels.size() <= static_cast<size_t>(law_kind)) {
        country.law_levels.resize(static_cast<size_t>(law_kind) + 1, 0);
    }
    country.law_levels[static_cast<size_t>(law_kind)] = static_cast<uint8_t>(level);
    // Political power is charged by command validation/application (validate_command
    // owns the cost check and the rejection path). Charging it here as well would
    // double-charge command-driven changes and charge the AI's free law flips.
    refresh_law_modifiers(g, country);
}

void phase_politics(Game& g) {
    World& w = g.world;
    const SimConstants& k = g.content.constants;
    const double hour_fraction = 1.0 / static_cast<double>(TICKS_PER_DAY);
    const double stability_rate = clamp(k.stability_drift, 0.0, 1.0) * hour_fraction;
    const double war_support_rate = clamp(k.war_support_drift, 0.0, 1.0) * hour_fraction;

    w.countries.for_each([&](CountryId cid, Country& c) {
        if (!c.alive) return;
        const bool at_war = country_at_war(w, cid);
        c.at_war = at_war;

        const Modifiers mods = c.total_modifiers();

        // Political power: base plus gain modifiers, accumulated hourly.
        const double pp_gain = daily_political_power(g, cid);
        c.political_power += pp_gain * hour_fraction;
        if (!std::isfinite(c.political_power) || c.political_power < 0.0) c.political_power = 0.0;

        // Manpower: the recruitable pool of every controlled state, and the states'
        // own pools where recruitment actually draws from.
        const double daily = daily_manpower_gain(g, cid);
        c.manpower += daily * hour_fraction;
        if (!std::isfinite(c.manpower) || c.manpower < 0.0) c.manpower = 0.0;
        w.states.for_each([&](StateId, State& s) {
            if (s.controller != cid) return;
            const double per_day =
                manpower_from_population(c, k, state_population(w, s));
            s.manpower_pool += per_day * hour_fraction;
            if (!std::isfinite(s.manpower_pool) || s.manpower_pool < 0.0) s.manpower_pool = 0.0;
        });

        // Stability / war support drift toward their war-state targets.
        double stability_target = 0.5 + mods.get(ModifierKind::Stability);
        double war_support_target = 0.5 + mods.get(ModifierKind::WarSupport);
        if (at_war) {
            stability_target -= kWarStabilityTargetPenalty;
            war_support_target += kWarSupportTargetBonus;
        }
        if (at_war && capital_occupied(w, cid)) stability_target -= kCapitalLostStabilityPenalty;
        stability_target = clamp01(stability_target);
        war_support_target = clamp01(war_support_target);
        c.stability = clamp01(c.stability + (stability_target - c.stability) * stability_rate);
        c.war_support = clamp01(c.war_support + (war_support_target - c.war_support) * war_support_rate);

        // Consumer goods: base demand, relieved by a war economy. No law modifier
        // exists for consumer goods in the content schema (see report).
        c.consumer_goods_ratio =
            clamp01(k.consumer_goods_base - (at_war ? kWarConsumerGoodsRelief : 0.0));
    });
}

}  // namespace hoi
