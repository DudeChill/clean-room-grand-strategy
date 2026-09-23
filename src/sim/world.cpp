// World-level queries that are shared by every subsystem.

#include "sim/world.h"

#include <algorithm>

#include "data/content.h"

namespace hoi {

namespace {

bool in_side(const std::vector<WarParticipant>& side, CountryId c) {
    for (const auto& p : side)
        if (p.country == c) return true;
    return false;
}

}  // namespace

const char* order_kind_name(OrderKind k) {
    switch (k) {
        case OrderKind::None: return "none";
        case OrderKind::FrontLine: return "front_line";
        case OrderKind::Offensive: return "offensive";
        case OrderKind::Fallback: return "fallback";
        case OrderKind::Garrison: return "garrison";
        case OrderKind::NavalInvasion: return "naval_invasion";
        case OrderKind::Paradrop: return "paradrop";
        default: return "unknown";
    }
}

const char* building_kind_name(BuildingKind k) {
    switch (k) {
        case BuildingKind::CivilianFactory: return "civilian_factory";
        case BuildingKind::MilitaryFactory: return "military_factory";
        case BuildingKind::Dockyard: return "dockyard";
        case BuildingKind::Infrastructure: return "infrastructure";
        case BuildingKind::Railway: return "railway";
        case BuildingKind::SupplyHub: return "supply_hub";
        case BuildingKind::AirBase: return "air_base";
        case BuildingKind::NavalBase: return "naval_base";
        case BuildingKind::Radar: return "radar";
        case BuildingKind::Fort: return "fort";
        case BuildingKind::AntiAir: return "anti_air";
        case BuildingKind::SyntheticRefinery: return "synthetic_refinery";
        default: return "unknown";
    }
}

const char* air_mission_name(AirMission m) {
    switch (m) {
        case AirMission::None: return "none";
        case AirMission::AirSuperiority: return "air_superiority";
        case AirMission::Interception: return "interception";
        case AirMission::CloseAirSupport: return "close_air_support";
        case AirMission::StrategicBombing: return "strategic_bombing";
        case AirMission::LogisticsStrike: return "logistics_strike";
        case AirMission::Reconnaissance: return "reconnaissance";
        default: return "unknown";
    }
}

bool air_mission_is_offensive(AirMission m) {
    switch (m) {
        case AirMission::CloseAirSupport:
        case AirMission::StrategicBombing:
        case AirMission::LogisticsStrike:
            return true;
        default:
            return false;
    }
}

FactoryPool equipment_factory_pool(const Content& content, EquipmentId equipment) {
    const EquipmentDef* def = content.equipment_def(equipment);
    if (!def) return FactoryPool::Military;
    if (def->category == EquipmentCategory::Ship || def->category == EquipmentCategory::Convoy) {
        return FactoryPool::Dockyard;
    }
    return FactoryPool::Military;
}

FactoryPool line_factory_pool(const Content& content, const ProductionLine& line) {
    return equipment_factory_pool(content, line.equipment);
}

const char* naval_mission_name(NavalMission m) {
    switch (m) {
        case NavalMission::None: return "none";
        case NavalMission::Patrol: return "patrol";
        case NavalMission::StrikeForce: return "strike_force";
        case NavalMission::ConvoyEscort: return "convoy_escort";
        case NavalMission::ConvoyRaid: return "convoy_raid";
        case NavalMission::InvasionSupport: return "invasion_support";
        case NavalMission::Training: return "training";
        default: return "unknown";
    }
}

bool naval_mission_is_offensive(NavalMission m) {
    return m == NavalMission::ConvoyRaid || m == NavalMission::StrikeForce;
}

bool World::at_war(CountryId a, CountryId b) const {
    if (!a.valid() || !b.valid() || a == b) return false;
    bool result = false;
    wars.for_each([&](WarId, const War& w) {
        if (result || !w.active) return;
        const bool a_att = in_side(w.attackers, a);
        const bool b_att = in_side(w.attackers, b);
        const bool a_def = in_side(w.defenders, a);
        const bool b_def = in_side(w.defenders, b);
        if ((a_att && b_def) || (a_def && b_att)) result = true;
    });
    return result;
}

bool World::has_access(CountryId c, ProvinceId p) const {
    const Province* prov = province(p);
    if (!prov || !c.valid()) return false;
    const CountryId ctl = prov->controller;
    if (!ctl.valid()) return false;
    if (ctl == c) return true;
    if (at_war(c, ctl)) return false;

    const Country* controller = country(ctl);
    const Country* self = country(c);
    if (controller && controller->overlord == c) return true;  // own puppet
    if (self && self->overlord == ctl) return true;            // own overlord
    if (self && controller && self->faction != 0 && self->faction == controller->faction) {
        return true;
    }
    const Relation* r = find_relation(c, ctl);
    if (r && r->military_access) return true;
    return false;
}

}  // namespace hoi
