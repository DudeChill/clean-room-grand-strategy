// HTTP server + JSON API. Single-threaded: the simulation advances between request
// polls, so there are no locks and the tick order stays deterministic.

#include "game/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/math.h"
#include "save/save.h"
#include "sim/commands.h"
#include "sim/industry.h"
#include "sim/map.h"
#include "sim/navy.h"
#include "sim/politics.h"
#include "sim/research.h"
#include "sim/supply.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {

namespace {

using Clock = std::chrono::steady_clock;

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string read_file(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *ok = true;
    return ss.str();
}

// Extracts a query parameter from a request target such as "/api/supply?province=42".
std::string query_param(const std::string& target, const std::string& key) {
    const size_t q = target.find('?');
    if (q == std::string::npos) return {};
    size_t pos = q + 1;
    while (pos < target.size()) {
        const size_t eq = target.find('=', pos);
        if (eq == std::string::npos) break;
        const size_t amp = target.find('&', eq);
        const std::string name = target.substr(pos, eq - pos);
        if (name == key) {
            return amp == std::string::npos ? target.substr(eq + 1)
                                            : target.substr(eq + 1, amp - eq - 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string content_type_for(const std::string& path) {
    auto ends = [&](const char* suffix) {
        const size_t n = std::strlen(suffix);
        return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "application/javascript; charset=utf-8";
    if (ends(".css")) return "text/css; charset=utf-8";
    if (ends(".json")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    return "application/octet-stream";
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool parse_request(int fd, Request* out) {
    std::string buffer;
    char chunk[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
        header_end = buffer.find("\r\n\r\n");
        if (buffer.size() > 1 << 20) return false;
    }
    const std::string headers = buffer.substr(0, header_end);
    std::istringstream hs(headers);
    std::string line;
    std::getline(hs, line);
    {
        std::istringstream rl(line);
        std::string version;
        rl >> out->method >> out->path >> version;
    }
    std::string rest = buffer.substr(header_end + 4);
    size_t content_length = 0;
    while (std::getline(hs, line)) {
        if (line.empty() || line == "\r") continue;
        std::string key, value;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        key = line.substr(0, colon);
        value = line.substr(colon + 1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "content-length") content_length = std::strtoul(value.c_str(), nullptr, 10);
    }
    while (rest.size() < content_length) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        rest.append(chunk, static_cast<size_t>(n));
    }
    out->body = rest.substr(0, content_length);
    return true;
}

std::string response_bytes(const Response& r) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << r.status << " " << status_text(r.status) << "\r\n"
       << "Content-Type: " << r.content_type << "\r\n"
       << "Content-Length: " << r.body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << r.body;
    return ss.str();
}

// --------------------------------------------------------------- snapshots ----

}  // namespace

bool parse_command_type(const std::string& name, CommandType* out) {
    for (int i = 0; i < static_cast<int>(CommandType::Count); ++i) {
        const CommandType t = static_cast<CommandType>(i);
        if (name == command_type_name(t)) {
            *out = t;
            return true;
        }
    }
    return false;
}

bool apply_client_command(Game& g, const Json& payload, std::string* err) {
    if (!payload.is_object()) {
        if (err) *err = "payload must be a JSON object";
        return false;
    }
    CommandType type = CommandType::None;
    const std::string name = payload.at("type").as_string();
    if (!parse_command_type(name, &type)) {
        if (err) *err = "unknown command type: " + name;
        return false;
    }
    Command cmd;
    cmd.type = type;
    cmd.issued_tick = g.world.tick;
    cmd.country = g.player_country;
    if (payload.has("country") && payload.at("country").is_number()) {
        cmd.country = CountryId(static_cast<uint32_t>(payload.at("country").as_int()));
    }
    auto id_field = [&](const char* key, uint32_t fallback) {
        return payload.has(key) ? static_cast<uint32_t>(payload.at(key).as_int())
                                : fallback;
    };
    cmd.province = ProvinceId(id_field("province", INVALID_ID));
    cmd.state = StateId(id_field("state", INVALID_ID));
    cmd.division = DivisionId(id_field("division", INVALID_ID));
    cmd.army = ArmyId(id_field("army", INVALID_ID));
    cmd.character = CharacterId(id_field("character", INVALID_ID));
    cmd.target_country = CountryId(id_field("target_country", INVALID_ID));
    cmd.war = WarId(id_field("war", INVALID_ID));
    cmd.wing = AirWingId(id_field("wing", INVALID_ID));
    cmd.region = RegionId(id_field("region", INVALID_ID));
    cmd.fleet_id = FleetId(id_field("fleet", INVALID_ID));
    cmd.ship_id = ShipId(id_field("ship", INVALID_ID));
    cmd.task_force = TaskForceId(id_field("task_force", INVALID_ID));
    if (payload.has("equipment")) {
        const Json& e = payload.at("equipment");
        if (e.is_string()) {
            cmd.equipment = g.content.equipment_id(e.as_string());
        } else {
            cmd.equipment = EquipmentId(static_cast<uint32_t>(e.as_int()));
        }
    }
    if (payload.has("template")) {
        const Json& t = payload.at("template");
        cmd.template_id = t.is_string() ? g.content.template_id(t.as_string())
                                        : TemplateId(static_cast<uint32_t>(t.as_int()));
    }
    if (payload.has("template_id")) {
        cmd.template_id = TemplateId(static_cast<uint32_t>(payload.at("template_id").as_int()));
    }
    if (payload.has("tech")) {
        const Json& t = payload.at("tech");
        cmd.tech = t.is_string() ? g.content.tech_id(t.as_string())
                                 : TechId(static_cast<uint32_t>(t.as_int()));
    }
    cmd.value = static_cast<int32_t>(payload.at("value").as_int(0));
    cmd.value_f = payload.at("value_f").as_double(0.0);
    cmd.text = payload.at("text").as_string();
    if (payload.has("law")) cmd.text = payload.at("law").as_string();
    if (payload.has("kind")) cmd.value = static_cast<int32_t>(payload.at("kind").as_int(0));
    if (payload.has("order_kind")) cmd.value = static_cast<int32_t>(payload.at("order_kind").as_int(0));
    if (payload.has("name") && cmd.text.empty()) cmd.text = payload.at("name").as_string();
    if (payload.has("level")) cmd.value = static_cast<int32_t>(payload.at("level").as_int(0));
    if (payload.has("factories")) cmd.value = static_cast<int32_t>(payload.at("factories").as_int(0));
    if (payload.has("count")) cmd.value = static_cast<int32_t>(payload.at("count").as_int(1));
    if (payload.has("stance")) cmd.value = static_cast<int32_t>(payload.at("stance").as_int(0));
    if (payload.has("motorization")) {
        cmd.value = static_cast<int32_t>(payload.at("motorization").as_int(0));
    }
    if (payload.has("division_ids")) {
        for (const Json& v : payload.at("division_ids").array_items()) {
            cmd.divisions.push_back(DivisionId(static_cast<uint32_t>(v.as_int())));
        }
    }
    if (payload.has("battalions")) {
        for (const Json& b : payload.at("battalions").array_items()) {
            BattalionSlot slot;
            if (b.at("equipment").is_string()) {
                slot.equipment = g.content.equipment_id(b.at("equipment").as_string());
            } else {
                slot.equipment = EquipmentId(static_cast<uint32_t>(b.at("equipment").as_int()));
            }
            slot.count = static_cast<int>(b.at("count").as_int(1));
            slot.support = b.at("support").as_bool(false);
            cmd.battalions.push_back(slot);
        }
    }

    if (!cmd.country.valid()) {
        if (err) *err = "no player country: start the server with --player TAG or pass country";
        return false;
    }
    const CommandResult result = validate_command(g, cmd);
    if (result != CommandResult::Applied) {
        if (err) {
            *err = std::string("command rejected: ") + command_result_name(result);
        }
        return false;
    }
    g.queue.push(cmd);
    return true;
}

std::string map_static_json(const Game& g) {
    Json root = Json::object();
    root.set("name", Json(std::string("world")));
    Json provinces = Json::array();
    g.world.provinces.for_each([&](ProvinceId pid, const Province& p) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(pid.v)));
        j.set("name", Json(p.name));
        j.set("x", Json(p.x));
        j.set("y", Json(p.y));
        j.set("terrain", Json(std::string(terrain_name(p.terrain))));
        j.set("sea", Json(p.is_sea));
        j.set("coastal", Json(p.coastal));
        j.set("state", Json(static_cast<uint32_t>(p.state.v)));
        j.set("region", Json(static_cast<uint32_t>(p.region.v)));
        j.set("vp", Json(p.victory_points));
        j.set("infra", Json(p.infrastructure));
        j.set("hub", Json(p.supply_hub));
        j.set("air_base", Json(p.air_base));
        Json adj = Json::array();
        for (ProvinceId a : p.adj) adj.push_back(Json(static_cast<uint32_t>(a.v)));
        j.set("adj", adj);
        provinces.push_back(j);
    });
    root.set("provinces", provinces);

    Json states = Json::array();
    g.world.states.for_each([&](StateId sid, const State& s) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(sid.v)));
        j.set("name", Json(s.name));
        j.set("slots", Json(s.building_slots));
        states.push_back(j);
    });
    root.set("states", states);
    return root.dump();
}

std::string world_snapshot_json(const Game& g, CountryId viewer) {
    const World& w = g.world;
    Json root = Json::object();
    const GameDate& d = w.date;
    char date_buf[64];
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02u-%02u %02u:00", d.year, d.month, d.day,
                  d.hour);
    root.set("tick", Json(static_cast<double>(w.tick)));
    root.set("date", Json(std::string(date_buf)));

    // Per-province dynamic state as parallel arrays indexed by province id.
    Json owner = Json::array();
    Json controller = Json::array();
    Json supply = Json::array();
    for (uint32_t i = 0; i < w.provinces.capacity(); ++i) {
        const Province* p = w.provinces.try_get(ProvinceId(i));
        if (!p) {
            owner.push_back(Json(-1));
            controller.push_back(Json(-1));
            supply.push_back(Json(0));
            continue;
        }
        owner.push_back(Json(static_cast<int>(p->owner.valid() ? p->owner.v : -1)));
        controller.push_back(Json(static_cast<int>(p->controller.valid() ? p->controller.v : -1)));
        supply.push_back(Json(static_cast<int>(p->supply_level * 100.0)));
    }
    root.set("owner", owner);
    root.set("controller", controller);
    root.set("supply", supply);

    Json countries = Json::array();
    w.countries.for_each([&](CountryId cid, const Country& c) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(cid.v)));
        j.set("tag", Json(c.tag));
        j.set("name", Json(c.name));
        j.set("alive", Json(c.alive));
        j.set("ideology", Json(std::string(ideology_name(c.ideology))));
        int civ = 0, mil = 0, dock = 0;
        count_factories(w, cid, &civ, &mil, &dock);
        j.set("civ", Json(civ));
        j.set("mil", Json(mil));
        j.set("dock", Json(dock));
        j.set("divisions", Json(static_cast<uint32_t>(c.divisions.size())));
        j.set("manpower", Json(c.manpower));
        j.set("pp", Json(c.political_power));
        j.set("stability", Json(c.stability));
        j.set("war_support", Json(c.war_support));
        j.set("fuel", Json(c.fuel));
        j.set("at_war", Json(c.at_war));
        int controlled_states = 0;
        w.states.for_each([&](StateId, const State& s) {
            if (s.controller == cid) ++controlled_states;
        });
        j.set("states", Json(controlled_states));
        j.set("capital_state", Json(static_cast<int>(c.capital.valid() ? c.capital.v : 0)));
        // The client needs a province to centre the map on, so resolve the capital
        // state to the province flagged as the capital (or its highest-VP province).
        ProvinceId capital_province;
        int best_vp = -1;
        if (const State* cap = w.state(c.capital)) {
            for (ProvinceId pid : cap->provinces) {
                const Province* p = w.province(pid);
                if (!p || p->is_sea) continue;
                if (p->is_capital) {
                    capital_province = pid;
                    break;
                }
                if (p->victory_points > best_vp) {
                    best_vp = p->victory_points;
                    capital_province = pid;
                }
            }
        }
        j.set("capital", Json(static_cast<int>(capital_province.valid() ? capital_province.v : 0)));
        countries.push_back(j);
    });
    root.set("countries", countries);

    Json divisions = Json::array();
    w.divisions.for_each([&](DivisionId did, const Division& dv) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(did.v)));
        j.set("country", Json(static_cast<uint32_t>(dv.country.v)));
        j.set("province", Json(static_cast<int>(dv.location.valid() ? static_cast<int>(dv.location.v) : -1)));
        j.set("org", Json(dv.organization));
        j.set("max_org", Json(dv.max_organization));
        j.set("strength", Json(dv.strength));
        j.set("supply", Json(dv.supply));
        j.set("moving", Json(dv.moving));
        j.set("training", Json(dv.in_training()));
        j.set("battle", Json(static_cast<int>(dv.battle.valid() ? static_cast<int>(dv.battle.v) : -1)));
        j.set("army", Json(static_cast<int>(dv.army.valid() ? static_cast<int>(dv.army.v) : -1)));
        j.set("name", Json(dv.name));
        divisions.push_back(j);
    });
    root.set("divisions", divisions);

    Json battles = Json::array();
    w.battles.for_each([&](BattleId bid, const Battle& b) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(bid.v)));
        j.set("province", Json(static_cast<uint32_t>(b.province.v)));
        j.set("attackers", Json(static_cast<uint32_t>(b.attacker.divisions.size())));
        j.set("defenders", Json(static_cast<uint32_t>(b.defender.divisions.size())));
        j.set("progress", Json(b.progress));
        j.set("attacker", Json(static_cast<int>(b.attacker_lead.valid() ? static_cast<int>(b.attacker_lead.v) : -1)));
        j.set("defender", Json(static_cast<int>(b.defender_lead.valid() ? static_cast<int>(b.defender_lead.v) : -1)));
        j.set("terrain", Json(std::string(terrain_name(b.terrain))));
        battles.push_back(j);
    });
    root.set("battles", battles);

    Json wars = Json::array();
    w.wars.for_each([&](WarId wid, const War& war) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(wid.v)));
        j.set("start", Json(static_cast<double>(war.start_tick)));
        Json att = Json::array();
        for (const auto& p : war.attackers) att.push_back(Json(static_cast<uint32_t>(p.country.v)));
        Json def = Json::array();
        for (const auto& p : war.defenders) def.push_back(Json(static_cast<uint32_t>(p.country.v)));
        j.set("attackers", att);
        j.set("defenders", def);
        wars.push_back(j);
    });
    root.set("wars", wars);

    Json factions = Json::array();
    for (const Faction& f : w.factions) {
        Json j = Json::object();
        j.set("id", Json(f.id));
        j.set("name", Json(f.name));
        const Country* leader = w.country(f.leader);
        j.set("leader", Json(leader ? leader->tag : std::string("?")));
        j.set("leader_id", Json(static_cast<uint32_t>(f.leader.valid() ? f.leader.v : 0)));
        Json members = Json::array();
        for (CountryId m : f.members) {
            const Country* mc = w.country(m);
            if (mc) members.push_back(Json(mc->tag));
        }
        j.set("members", members);
        factions.push_back(j);
    }
    root.set("factions", factions);

    Json air_regions = Json::array();
    w.regions.for_each([&](RegionId rid, const Region& r) {
        if (r.is_sea) return;
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(rid.v)));
        j.set("name", Json(r.name));
        Json control = Json::array();
        for (const auto& entry : r.air_control) {
            const Country* c = w.country(entry.first);
            Json e = Json::object();
            e.set("tag", Json(c ? c->tag : std::string("?")));
            e.set("country", Json(static_cast<uint32_t>(entry.first.v)));
            e.set("share", Json(entry.second));
            control.push_back(e);
        }
        j.set("control", control);
        air_regions.push_back(j);
    });
    root.set("air_regions", air_regions);

    Json wings = Json::array();
    w.air_wings.for_each([&](AirWingId wid, const AirWing& wing) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(wid.v)));
        j.set("country", Json(static_cast<uint32_t>(wing.country.v)));
        const EquipmentDef* def = g.content.equipment_def(wing.equipment);
        j.set("equipment", Json(def ? def->key : std::string("?")));
        j.set("name", Json(wing.name));
        j.set("planes", Json(wing.planes));
        j.set("max_planes", Json(wing.max_planes));
        j.set("base", Json(static_cast<uint32_t>(wing.base.valid() ? wing.base.v : 0)));
        const Province* bp = w.province(wing.base);
        j.set("base_name", Json(bp ? bp->name : std::string("")));
        j.set("region", Json(static_cast<uint32_t>(wing.region.valid() ? wing.region.v : 0)));
        const Region* rg = wing.region.valid() ? w.regions.try_get(wing.region) : nullptr;
        j.set("region_name", Json(rg ? rg->name : std::string("")));
        j.set("mission", Json(std::string(air_mission_name(wing.mission))));
        j.set("mission_id", Json(static_cast<int>(wing.mission)));
        j.set("efficiency", Json(wing.efficiency));
        j.set("experience", Json(wing.experience));
        j.set("losses", Json(wing.losses));
        wings.push_back(j);
    });
    root.set("wings", wings);

    Json naval_regions = Json::array();
    w.regions.for_each([&](RegionId rid, const Region& r) {
        if (!r.is_sea) return;
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(rid.v)));
        j.set("name", Json(r.name));
        Json control = Json::array();
        for (const auto& entry : r.naval_control) {
            const Country* c = w.country(entry.first);
            Json e = Json::object();
            e.set("tag", Json(c ? c->tag : std::string("?")));
            e.set("country", Json(static_cast<uint32_t>(entry.first.v)));
            e.set("share", Json(entry.second));
            control.push_back(e);
        }
        j.set("control", control);
        naval_regions.push_back(j);
    });
    root.set("naval_regions", naval_regions);

    Json fleets = Json::array();
    w.fleets.for_each([&](FleetId fid, const Fleet& f) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(fid.v)));
        j.set("country", Json(static_cast<uint32_t>(f.country.v)));
        j.set("name", Json(f.name));
        Json forces = Json::array();
        for (TaskForceId tfid : f.task_forces) {
            const TaskForce* tf = w.task_force(tfid);
            if (!tf) continue;
            Json t = Json::object();
            t.set("id", Json(static_cast<uint32_t>(tfid.v)));
            t.set("name", Json(tf->name));
            t.set("mission", Json(std::string(naval_mission_name(tf->mission))));
            t.set("mission_id", Json(static_cast<int>(tf->mission)));
            t.set("at_sea", Json(tf->at_sea));
            const Region* sr = tf->sea_region.valid() ? w.regions.try_get(tf->sea_region) : nullptr;
            t.set("region", Json(static_cast<uint32_t>(tf->sea_region.valid() ? tf->sea_region.v : 0)));
            t.set("region_name", Json(sr ? sr->name : std::string("")));
            const Province* pp = w.province(tf->port);
            t.set("port", Json(static_cast<uint32_t>(tf->port.valid() ? tf->port.v : 0)));
            t.set("port_name", Json(pp ? pp->name : std::string("")));
            t.set("detection", Json(tf->detection));
            const TaskForceStats stats = task_force_stats(g, tfid);
            Json s = Json::object();
            s.set("ships", Json(stats.ships));
            s.set("naval_attack", Json(stats.naval_attack));
            s.set("torpedo_attack", Json(stats.torpedo_attack));
            s.set("armour", Json(stats.armour));
            s.set("hull", Json(stats.hull));
            s.set("detection", Json(stats.detection));
            s.set("sub_detection", Json(stats.sub_detection));
            t.set("stats", s);
            Json ships = Json::array();
            for (ShipId sid : tf->ships) {
                const Ship* sh = w.ship(sid);
                if (!sh) continue;
                const EquipmentDef* sd = g.content.equipment_def(sh->equipment);
                Json sj = Json::object();
                sj.set("id", Json(static_cast<uint32_t>(sid.v)));
                sj.set("name", Json(sh->name));
                sj.set("equipment", Json(sd ? sd->key : std::string("?")));
                sj.set("strength", Json(sh->strength));
                sj.set("organisation", Json(sh->organisation));
                sj.set("experience", Json(sh->experience));
                sj.set("fuel", Json(sh->fuel));
                sj.set("at_sea", Json(sh->at_sea));
                ships.push_back(sj);
            }
            t.set("ships", ships);
            forces.push_back(t);
        }
        j.set("task_forces", forces);
        fleets.push_back(j);
    });
    root.set("fleets", fleets);

    Json invasions = Json::array();
    for (const NavalInvasion& inv : w.invasions) {
        Json j = Json::object();
        j.set("army", Json(static_cast<uint32_t>(inv.army.v)));
        j.set("country", Json(static_cast<uint32_t>(inv.country.v)));
        j.set("progress", Json(inv.progress));
        j.set("landed", Json(inv.landed));
        const Province* t = w.province(inv.target);
        j.set("target_name", Json(t ? t->name : std::string("")));
        const Province* o = w.province(inv.origin);
        j.set("origin_name", Json(o ? o->name : std::string("")));
        invasions.push_back(j);
    }
    root.set("invasions", invasions);

    Json events = Json::array();
    const size_t event_start = g.events.size() > 200 ? g.events.size() - 200 : 0;
    for (size_t i = event_start; i < g.events.size(); ++i) {
        const SimEvent& e = g.events[i];
        Json j = Json::object();
        j.set("tick", Json(static_cast<double>(e.tick)));
        j.set("kind", Json(e.kind));
        j.set("text", Json(e.text));
        events.push_back(j);
    }
    root.set("events", events);

    // Player-specific detail.
    Json player = Json::object();
    const Country* c = w.country(viewer);
    if (c) {
        player.set("id", Json(static_cast<uint32_t>(viewer.v)));
        player.set("tag", Json(c->tag));
        player.set("name", Json(c->name));
        player.set("manpower", Json(c->manpower));
        player.set("pp", Json(c->political_power));
        player.set("stability", Json(c->stability));
        player.set("war_support", Json(c->war_support));
        player.set("fuel", Json(c->fuel));
        player.set("fuel_capacity", Json(c->fuel_capacity));
        player.set("consumer_goods", Json(c->consumer_goods_ratio));
        player.set("at_war", Json(c->at_war));
        player.set("recruitable", Json(daily_manpower_gain(g, viewer)));
        player.set("pp_gain", Json(daily_political_power(g, viewer)));

        Json stock = Json::object();
        for (size_t i = 0; i < c->equipment_stockpile.size(); ++i) {
            const EquipmentDef* def = g.content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
            if (!def) continue;
            stock.set(def->key, Json(c->equipment_stockpile[i]));
        }
        player.set("stockpile", stock);

        Json lines = Json::array();
        for (const auto& line : c->lines) {
            const EquipmentDef* def = g.content.equipment_def(line.equipment);
            Json j = Json::object();
            j.set("equipment", Json(def ? def->key : std::string("(idle)")));
            j.set("factories", Json(line.factories));
            j.set("efficiency", Json(line.efficiency));
            j.set("efficiency_cap", Json(line.efficiency_cap));
            j.set("output_total", Json(line.output_total));
            j.set("resource_shortage", Json(line.resource_shortage));
            lines.push_back(j);
        }
        player.set("lines", lines);

        Json construction = Json::array();
        for (const auto& project : c->construction.queue) {
            Json j = Json::object();
            j.set("kind", Json(std::string(building_kind_name(project.kind))));
            j.set("state", Json(static_cast<uint32_t>(project.state.v)));
            j.set("province", Json(static_cast<uint32_t>(project.province.v)));
            j.set("progress", Json(project.progress));
            j.set("cost", Json(project.cost));
            j.set("level", Json(project.target_level));
            construction.push_back(j);
        }
        player.set("construction", construction);

        Json slots = Json::array();
        for (const auto& slot : c->research.slots) {
            Json j = Json::object();
            const TechDef* def = g.content.tech_def(slot.tech);
            j.set("tech", Json(def ? def->key : std::string("")));
            j.set("name", Json(def ? def->name : std::string("")));
            j.set("progress", Json(slot.progress));
            j.set("cost", Json(slot.tech.valid() ? tech_cost_days(g, viewer, slot.tech) : 0.0));
            j.set("active", Json(slot.active));
            slots.push_back(j);
        }
        player.set("research", slots);

        Json completed = Json::array();
        for (TechId t : c->research.completed) {
            const TechDef* def = g.content.tech_def(t);
            if (def) completed.push_back(Json(def->key));
        }
        player.set("completed_techs", completed);

        Json available = Json::array();
        for (int i = 0; i < static_cast<int>(g.content.techs.size()); ++i) {
            const TechId t(static_cast<uint32_t>(i));
            if (!tech_available(g, viewer, t)) continue;
            const TechDef* def = g.content.tech_def(t);
            Json j = Json::object();
            j.set("key", Json(def->key));
            j.set("name", Json(def->name));
            j.set("category", Json(def->category));
            j.set("days", Json(tech_cost_days(g, viewer, t)));
            j.set("year", Json(def->year));
            available.push_back(j);
        }
        player.set("available_techs", available);

        Json training = Json::array();
        for (const auto& t : c->training) {
            const Division* dv = w.division(t.division);
            const DivisionTemplate* td = g.content.template_def(t.template_id);
            Json j = Json::object();
            j.set("division", Json(static_cast<uint32_t>(t.division.v)));
            j.set("template", Json(td ? td->name : std::string("")));
            j.set("days_left", Json(t.days_left));
            j.set("ready", Json(t.days_left <= 0.0));
            j.set("strength", Json(dv ? dv->strength : 0.0));
            training.push_back(j);
        }
        player.set("training", training);

        Json armies = Json::array();
        for (ArmyId aid : c->armies) {
            const Army* a = w.army(aid);
            if (!a) continue;
            Json j = Json::object();
            j.set("id", Json(static_cast<uint32_t>(aid.v)));
            j.set("name", Json(a->name));
            j.set("order", Json(std::string(order_kind_name(a->order.kind))));
            j.set("planning", Json(a->order.progress));
            j.set("stance", Json(static_cast<int>(a->stance)));
            j.set("motorization", Json(a->motorization));
            j.set("general", Json(static_cast<int>(a->general.valid() ? static_cast<int>(a->general.v) : -1)));
            Json divs = Json::array();
            for (DivisionId did : a->divisions) divs.push_back(Json(static_cast<uint32_t>(did.v)));
            j.set("divisions", divs);
            Json line = Json::array();
            for (ProvinceId p : a->order.line) line.push_back(Json(static_cast<uint32_t>(p.v)));
            j.set("line", line);
            Json target = Json::array();
            for (ProvinceId p : a->order.target_line) target.push_back(Json(static_cast<uint32_t>(p.v)));
            j.set("target_line", target);
            armies.push_back(j);
        }
        player.set("armies", armies);

        Json generals = Json::array();
        for (CharacterId gid : c->generals) {
            const Character* ch = w.character(gid);
            if (!ch) continue;
            Json j = Json::object();
            j.set("id", Json(static_cast<uint32_t>(gid.v)));
            j.set("name", Json(ch->name));
            j.set("skill", Json(ch->skill));
            j.set("attack", Json(ch->attack));
            j.set("defense", Json(ch->defense));
            j.set("planning", Json(ch->planning));
            j.set("army", Json(static_cast<int>(ch->army.valid() ? static_cast<int>(ch->army.v) : -1)));
            generals.push_back(j);
        }
        player.set("generals", generals);

        Json templates = Json::array();
        for (TemplateId tid : c->templates) {
            const DivisionTemplate* t = g.content.template_def(tid);
            if (!t) continue;
            Json j = Json::object();
            j.set("id", Json(static_cast<uint32_t>(tid.v)));
            j.set("key", Json(t->key));
            j.set("name", Json(t->name));
            j.set("width", Json(t->combat_width));
            j.set("org", Json(t->max_organization));
            j.set("hp", Json(t->max_strength));
            j.set("soft", Json(t->soft_attack));
            j.set("hard", Json(t->hard_attack));
            j.set("defense", Json(t->defense));
            j.set("breakthrough", Json(t->breakthrough));
            j.set("armor", Json(t->armor));
            j.set("piercing", Json(t->piercing));
            j.set("speed", Json(t->speed));
            j.set("manpower", Json(t->manpower));
            j.set("cost", Json(t->build_cost));
            j.set("train_days", Json(t->train_days));
            templates.push_back(j);
        }
        player.set("templates", templates);

        Json player_states = Json::array();
        w.states.for_each([&](StateId sid, const State& s) {
            if (s.controller != viewer) return;
            Json j = Json::object();
            j.set("id", Json(static_cast<uint32_t>(sid.v)));
            j.set("name", Json(s.name));
            j.set("civ", Json(s.civilian_factories));
            j.set("mil", Json(s.military_factories));
            j.set("dock", Json(s.dockyards));
            j.set("slots", Json(s.building_slots));
            j.set("provinces", Json(static_cast<uint32_t>(s.provinces.size())));
            player_states.push_back(j);
        });
        player.set("states", player_states);

        Json laws = Json::array();
        for (const auto& law : g.content.laws) {
            Json j = Json::object();
            j.set("key", Json(law.key));
            j.set("name", Json(law.name));
            j.set("kind", Json(law.kind));
            j.set("level", Json(law.level));
            j.set("cost", Json(law.cost));
            const bool enacted = law.kind >= 0 &&
                                 law.kind < static_cast<int>(c->law_levels.size()) &&
                                 c->law_levels[law.kind] == law.level;
            j.set("enacted", Json(enacted));
            laws.push_back(j);
        }
        player.set("laws", laws);

        // Alerts: strategic situations a player must be able to see (spec section 104).
        Json alerts = Json::array();
        auto add_alert = [&](const std::string& text, const std::string& action) {
            Json j = Json::object();
            j.set("text", Json(text));
            j.set("action", Json(action));
            alerts.push_back(j);
        };
        bool idle_slot = false;
        for (const auto& slot : c->research.slots) {
            if (!slot.active) idle_slot = true;
        }
        if (idle_slot) add_alert("research slot idle", "research");
        int assigned = 0;
        for (const auto& line : c->lines) assigned += line.factories;
        int civ = 0, mil = 0, dock = 0;
        count_factories(w, viewer, &civ, &mil, &dock);
        if (assigned < mil) add_alert("unassigned military factories", "production");
        if (c->training.empty() && c->divisions.size() < 5) add_alert("few divisions in service", "military");
        bool unassigned_division = false;
        for (DivisionId did : c->divisions) {
            const Division* dv = w.division(did);
            if (dv && !dv->in_training() && !dv->army.valid()) unassigned_division = true;
        }
        if (unassigned_division) add_alert("divisions not assigned to an army", "military");
        for (const auto& line : c->lines) {
            if (line.resource_shortage > 0.01 && line.factories > 0) {
                add_alert("production line short of resources", "production");
                break;
            }
        }
        if (!c->training.empty()) {
            for (const auto& t : c->training) {
                if (t.days_left <= 0.0) {
                    add_alert("division ready to deploy", "military");
                    break;
                }
            }
        }
        player.set("alerts", alerts);
    }
    root.set("player", player);

    Json equipment = Json::array();
    for (const auto& def : g.content.equipment) {
        Json j = Json::object();
        j.set("id", Json(static_cast<uint32_t>(def.id.v)));
        j.set("key", Json(def.key));
        j.set("name", Json(def.name));
        j.set("category", Json(std::string(equipment_category_name(def.category))));
        j.set("cost", Json(def.build_cost));
        j.set("archetype", Json(def.archetype));
        equipment.push_back(j);
    }
    root.set("equipment_defs", equipment);

    return root.dump();
}

int run_server(Game& g, const ServerOptions& opts, volatile bool* stop) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        HOI_ERROR("socket() failed: %s", std::strerror(errno));
        return 1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(opts.port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        HOI_ERROR("bind(:%u) failed: %s", static_cast<unsigned>(opts.port), std::strerror(errno));
        ::close(fd);
        return 1;
    }
    ::listen(fd, 16);
    std::printf("serving on http://127.0.0.1:%u  (web root: %s)\n",
                static_cast<unsigned>(opts.port), opts.web_root.c_str());

    bool paused = true;  // the client unpauses; nothing runs before it connects
    int speed = 2;       // 0 = paused, 1..5
    const uint64_t ticks_per_second[] = {0, 1, 2, 4, 8, 24};

    auto now = [] { return Clock::now(); };
    auto next_tick = now();
    uint64_t last_autosave_day = 0;

    std::string map_cache;  // static map JSON, computed once

    while (!(stop && *stop)) {
        // ---- advance the simulation -------------------------------------
        if (!paused && speed > 0) {
            const uint64_t tps = ticks_per_second[static_cast<size_t>(clamp(speed, 1, 5))];
            const auto interval = std::chrono::microseconds(1000000 / std::max<uint64_t>(1, tps));
            int budget = 32;
            while (now() >= next_tick && budget-- > 0) {
                g.tick_once();
                next_tick += interval;
            }
            if (now() > next_tick + std::chrono::seconds(1)) next_tick = now();

            if (opts.autosave_days > 0 && g.world.tick % 24 == 0) {
                const uint64_t day = g.world.tick / 24;
                if (day != last_autosave_day && day % opts.autosave_days == 0) {
                    last_autosave_day = day;
                    std::string err;
                    if (!save_game(g, opts.save_path, &err)) {
                        HOI_WARN("autosave failed: %s", err.c_str());
                    } else {
                        HOI_INFO("autosaved to %s (day %llu)", opts.save_path.c_str(),
                                 static_cast<unsigned long long>(day));
                    }
                }
            }
        }

        // ---- serve at most one connection per loop pass ------------------
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval tv{0, 2000};
        const int ready = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        const int client = ::accept(fd, nullptr, nullptr);
        if (client < 0) continue;
        int flag = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        Request req;
        Response res;
        if (!parse_request(client, &req)) {
            ::close(client);
            continue;
        }

        const size_t query = req.path.find('?');
        const std::string path = query == std::string::npos ? req.path : req.path.substr(0, query);

        if (path == "/api/map") {
            if (map_cache.empty()) map_cache = map_static_json(g);
            res.body = map_cache;
        } else if (path == "/api/state") {
            res.body = world_snapshot_json(g, g.player_country);
        } else if (path == "/api/command" && req.method == "POST") {
            std::string err;
            Json payload = Json::parse(req.body, &err);
            if (payload.is_null()) {
                res.status = 400;
                res.body = Json::object().dump();
                res.body = std::string("{\"ok\":false,\"error\":\"malformed json: ") + err + "\"}";
            } else {
                std::string cmd_err;
                const bool ok = apply_client_command(g, payload, &cmd_err);
                res.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" +
                           (ok ? "" : cmd_err) + "\"}";
                if (!ok) res.status = 400;
            }
        } else if (path == "/api/time" && req.method == "POST") {
            std::string err;
            Json payload = Json::parse(req.body, &err);
            if (payload.has("paused")) paused = payload.at("paused").as_bool(paused);
            if (payload.has("speed")) speed = static_cast<int>(payload.at("speed").as_int(speed));
            speed = clamp(speed, 0, 5);
            res.body = std::string("{\"ok\":true,\"paused\":") + (paused ? "true" : "false") +
                       ",\"speed\":" + std::to_string(speed) + "}";
        } else if (path == "/api/battle") {
            const std::string id_text = query_param(req.path, "id");
            const Battle* b = g.world.battle(BattleId(static_cast<uint32_t>(std::strtoul(id_text.c_str(), nullptr, 10))));
            if (!b) {
                res.status = 404;
                res.body = "{\"error\":\"no such battle\"}";
            } else {
                Json j = Json::object();
                j.set("id", Json(static_cast<uint32_t>(b->id.v)));
                j.set("province", Json(static_cast<uint32_t>(b->province.v)));
                const Province* p = g.world.province(b->province);
                j.set("province_name", Json(p ? p->name : std::string("")));
                j.set("terrain", Json(std::string(terrain_name(b->terrain))));
                j.set("river_crossing", Json(b->river_crossing));
                j.set("encirclement", Json(b->encirclement));
                j.set("progress", Json(b->progress));
                j.set("last_tick", Json(static_cast<double>(b->last_tick)));
                auto side_json = [&](const BattleSideState& side) {
                    Json s = Json::object();
                    s.set("soft_attack", Json(side.total_soft_attack));
                    s.set("hard_attack", Json(side.total_hard_attack));
                    s.set("defense", Json(side.total_defense));
                    s.set("breakthrough", Json(side.total_breakthrough));
                    s.set("armor", Json(side.total_armor));
                    s.set("piercing", Json(side.total_piercing));
                    Json units = Json::array();
                    for (size_t i = 0; i < side.divisions.size(); ++i) {
                        const Division* d = g.world.division(side.divisions[i]);
                        Json u = Json::object();
                        u.set("id", Json(static_cast<uint32_t>(side.divisions[i].v)));
                        u.set("name", Json(d ? d->name : std::string("")));
                        const Country* c = d ? g.world.country(d->country) : nullptr;
                        u.set("tag", Json(c ? c->tag : std::string("")));
                        u.set("org", Json(d ? d->organization : 0.0));
                        u.set("max_org", Json(d ? d->max_organization : 0.0));
                        u.set("strength", Json(d ? d->strength : 0.0));
                        u.set("supply", Json(d ? d->supply : 0.0));
                        u.set("entrenchment", Json(d ? d->entrenchment : 0.0));
                        u.set("planning", Json(d ? d->planning : 0.0));
                        units.push_back(u);
                    }
                    s.set("divisions", units);
                    return s;
                };
                j.set("attacker", side_json(b->attacker));
                j.set("defender", side_json(b->defender));
                Json debug = Json::array();
                for (const BattleDebugLine& line : b->debug) {
                    Json d = Json::object();
                    d.set("division", Json(static_cast<uint32_t>(line.division.v)));
                    d.set("base_attack", Json(line.base_attack));
                    d.set("planning", Json(line.planning_mod));
                    d.set("terrain", Json(line.terrain_mod));
                    d.set("supply", Json(line.supply_mod));
                    d.set("commander", Json(line.commander_mod));
                    d.set("experience", Json(line.experience_mod));
                    d.set("air", Json(line.air_mod));
                    d.set("final_attack", Json(line.final_attack));
                    d.set("enemy_defense", Json(line.enemy_defense));
                    d.set("damage", Json(line.damage));
                    d.set("org_damage", Json(line.org_damage));
                    d.set("strength_damage", Json(line.strength_damage));
                    debug.push_back(d);
                }
                j.set("debug", debug);
                res.body = j.dump();
            }
        } else if (path == "/api/supply") {
            const std::string id_text = query_param(req.path, "province");
            const ProvinceId pid(static_cast<uint32_t>(std::strtoul(id_text.c_str(), nullptr, 10)));
            const Province* p = g.world.province(pid);
            if (!p) {
                res.status = 404;
                res.body = "{\"error\":\"no such province\"}";
            } else {
                const CountryId holder = p->controller.valid() ? p->controller : p->owner;
                std::vector<SupplyRouteStep> route;
                ProvinceId bottleneck;
                const double delivered =
                    holder.valid() ? explain_supply_route(g, holder, pid, &route, &bottleneck) : 0.0;
                Json j = Json::object();
                j.set("province", Json(static_cast<uint32_t>(pid.v)));
                j.set("name", Json(p->name));
                const Country* hc = g.world.country(holder);
                j.set("country", Json(hc ? hc->tag : std::string("")));
                j.set("supply_level", Json(p->supply_level));
                j.set("delivered", Json(delivered));
                const Province* bp = g.world.province(bottleneck);
                j.set("bottleneck", Json(bp ? bp->name : std::string("")));
                Json steps = Json::array();
                for (const SupplyRouteStep& step : route) {
                    Json s = Json::object();
                    const Province* sp = g.world.province(step.province);
                    s.set("province", Json(static_cast<uint32_t>(step.province.v)));
                    s.set("name", Json(sp ? sp->name : std::string("")));
                    s.set("capacity", Json(step.capacity));
                    steps.push_back(s);
                }
                j.set("route", steps);
                Json units = Json::array();
                g.world.divisions.for_each([&](DivisionId did, const Division& d) {
                    if (d.location != pid) return;
                    Json u = Json::object();
                    u.set("id", Json(static_cast<uint32_t>(did.v)));
                    u.set("name", Json(d.name));
                    u.set("supply", Json(d.supply));
                    u.set("fuel", Json(d.fuel));
                    units.push_back(u);
                });
                j.set("divisions", units);
                res.body = j.dump();
            }
        } else if (path == "/api/save" && req.method == "POST") {
            std::string err;
            const bool ok = save_game(g, opts.save_path, &err);
            res.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" +
                       (ok ? "" : err) + "\",\"path\":\"" + opts.save_path + "\"}";
        } else if (path == "/api/load" && req.method == "POST") {
            std::string err;
            Json payload = Json::parse(req.body, &err);
            std::string source = opts.save_path;
            if (payload.is_object() && payload.has("path")) source = payload.at("path").as_string();
            const bool ok = load_game(g, source, &err);
            if (ok) {
                // The loaded world may hand different countries to the AI, so the
                // server re-derives nothing: the save carries ai_controlled and the
                // player country.
                map_cache.clear();
                next_tick = now();
                paused = true;
                last_autosave_day = g.world.tick / 24;
                HOI_INFO("loaded %s at tick %llu", source.c_str(),
                         static_cast<unsigned long long>(g.world.tick));
            } else {
                HOI_WARN("load failed: %s", err.c_str());
            }
            res.body = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"error\":\"" +
                       (ok ? "" : err) + "\",\"tick\":" +
                       std::to_string(static_cast<unsigned long long>(g.world.tick)) + "}";
            if (!ok) res.status = 400;
        } else if (path == "/api/meta") {
            Json j = Json::object();
            j.set("paused", Json(paused));
            j.set("speed", Json(speed));
            j.set("seed", Json(static_cast<double>(g.seed)));
            j.set("scenario", Json(g.scenario_path));
            j.set("player", Json(static_cast<uint32_t>(g.player_country.valid() ? g.player_country.v : 0)));
            j.set("hashes", Json(hash_report(g)));
            res.body = j.dump();
        } else if (path == "/api/hashes") {
            res.content_type = "text/plain; charset=utf-8";
            res.body = hash_report(g);
        } else {
            std::string rel = path == "/" ? "/index.html" : path;
            if (rel.find("..") != std::string::npos) {
                res.status = 400;
                res.body = "bad path";
            } else {
                bool ok = false;
                res.body = read_file(opts.web_root + rel, &ok);
                if (!ok) {
                    res.status = 404;
                    res.content_type = "text/plain; charset=utf-8";
                    res.body = "not found: " + rel;
                } else {
                    res.content_type = content_type_for(rel);
                }
            }
        }

        send_all(client, response_bytes(res));
        ::close(client);
    }
    ::close(fd);
    return 0;
}

}  // namespace hoi
