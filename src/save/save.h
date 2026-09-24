#pragma once
// Save/load, world hashing and deterministic replay (spec sections 22-23, 83-88).
//
// Save files are versioned binary containers of self-describing sections. A world
// hash is computed over the same canonical bytes, so a load that reproduces the
// hash provably reproduced the state.

#include <cstdint>
#include <string>
#include <vector>

#include "core/binio.h"
#include "core/types.h"

namespace hoi {

struct Game;

inline constexpr uint32_t SAVE_MAGIC = 0x34494F48;  // "HOI4" little-endian
// 4: Politics gained the per-country focus/event/decision state and the script
//    variables + delayed events; Economy gained the focus/event/decision content
//    tables. Version 3 files have none of those fields and are rejected.
// 5: Politics gained the per-country national spirits, spirit/advisor rosters and
//    slot capacities; Economy gained the national spirit and political advisor
//    content tables. Layout changed, so version 4 files are rejected.
// 6: Economy gained the world-level trade routes (World::trade_routes) after the
//    per-country economy blocks; layout changed, so version 5 files are rejected.
inline constexpr uint32_t SAVE_VERSION = 6;

enum class Subsystem : uint8_t {
    Map = 0,
    Countries,
    Economy,
    Military,
    Battles,
    Diplomacy,
    Politics,
    Ai,
    Rng,
    Count
};

const char* subsystem_name(Subsystem s);

struct SectionHeader {
    Subsystem subsystem;
    uint32_t bytes;
    uint64_t hash;
};

// Serializes one subsystem into `out` (payload only).
void serialize_subsystem(const Game& g, Subsystem s, ByteWriter* out);
// Deserializes one subsystem payload into `g`. Returns false on malformed input.
bool deserialize_subsystem(Game& g, Subsystem s, ByteReader* in);

// Hash of one subsystem, computed over its canonical serialization.
uint64_t subsystem_hash(const Game& g, Subsystem s);

// Combined hash of the whole world, in fixed subsystem order.
uint64_t world_hash(const Game& g);

// Human-readable hash report (used by the determinism test and the CLI).
std::string hash_report(const Game& g);

// Writes a complete save file: header, sections, world hash, command log.
bool save_game(const Game& g, const std::string& path, std::string* err);
bool load_game(Game& g, const std::string& path, std::string* err);

// Deterministic replay: the initial state hash plus the ordered command stream is
// sufficient to reproduce a run exactly.
struct ReplayHeader {
    uint32_t version = SAVE_VERSION;
    uint64_t seed = 0;
    std::string scenario;
    uint64_t initial_hash = 0;
    Tick ticks = 0;
};

bool save_replay(const Game& g, const std::string& path, std::string* err);
bool load_replay(Game& g, const std::string& path, std::string* err);

}  // namespace hoi
