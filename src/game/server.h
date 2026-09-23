#pragma once
// Local HTTP server: the playable surface.
//
// The client is a pure observer: it reads JSON snapshots and writes Commands. It
// never owns gameplay state, so the same simulation runs headless, in a browser
// session, or in a future dedicated server without changes.

#include <cstdint>
#include <string>

#include "core/json.h"
#include "game/game.h"

namespace hoi {

struct ServerOptions {
    uint16_t port = 8080;
    std::string web_root = "web";       // static files (index.html, app.js, style.css)
    std::string save_path = "session.save";
    uint64_t autosave_days = 0;         // 0 disables periodic saves
    bool verbose = false;
};

// Runs the server loop until `stop` becomes true (SIGINT/SIGTERM handler sets it)
// or the simulation end condition is met. Returns a process exit code.
int run_server(Game& g, const ServerOptions& opts, volatile bool* stop);

// Static map description (geometry, names, terrain). Sent once per session.
std::string map_static_json(const Game& g);

// Dynamic world snapshot for the player. `viewer` selects the player's country.
std::string world_snapshot_json(const Game& g, CountryId viewer);

// Applies a client-issued command payload ({"type": "...", ...}). Returns false and
// fills `err` when the payload is malformed or the command is invalid.
bool apply_client_command(Game& g, const Json& payload, std::string* err);

// Parses a command type name ("move_division", ...). Returns false when unknown.
bool parse_command_type(const std::string& name, CommandType* out);

}  // namespace hoi
