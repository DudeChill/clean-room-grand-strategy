#pragma once
// Command system.
//
// Every gameplay mutation enters the simulation as a Command. Commands are
// validated before they mutate state, are serializable, are recorded in the
// command log (which makes replays possible), and are the same objects the AI
// issues - so AI play exercises exactly the code path players use.

#include <cstdint>
#include <string>
#include <vector>

#include "core/binio.h"
#include "core/types.h"
#include "sim/world.h"

namespace hoi {

struct Game;

enum class CommandType : uint8_t {
    None = 0,
    SetProductionLine,     // equipment + factory count for a production line
    RemoveProductionLine,
    StartConstruction,     // queue a building project
    CancelConstruction,
    StartResearch,         // occupy a research slot with a technology
    CancelResearch,
    CreateTemplate,        // define a division template
    EditTemplate,          // change battalions of a template
    RecruitDivision,       // queue training of a division from a template
    DeployDivision,        // deploy a trained division to a province
    MoveDivision,          // move one division to an adjacent province
    SetDivisionOrder,      // army-level order (front line / offensive / fallback)
    CreateArmy,
    AssignDivisionToArmy,
    AssignGeneral,
    DeclareWar,
    OfferPeace,
    JoinFaction,        // join an existing faction led by `target_country`
    LeaveFaction,
    SetLaw,
    SetTradePolicy,
    SetStance,             // aggressive / defensive / garrison posture for an army
    MotorizeSupply,        // motorisation level of an army's supply
    ToggleFuelPriority,
    Count
};

const char* command_type_name(CommandType t);

struct Command {
    CommandType type = CommandType::None;
    CountryId country;  // issuing country
    Tick issued_tick = 0;

    // Generic payload: commands are fixed-shape, so a flat set of fields keeps
    // serialization trivial and network framing trivial with it.
    ProvinceId province;
    ProvinceId province_b;
    StateId state;
    DivisionId division;
    ArmyId army;
    CharacterId character;
    EquipmentId equipment;
    TemplateId template_id;
    TechId tech;
    CountryId target_country;
    WarId war;
    int32_t value = 0;       // factories, level, law level, motorisation level
    double value_f = 0.0;    // reserved for fractional parameters
    std::string text;        // template name / army name
    std::vector<BattalionSlot> battalions;  // CreateTemplate / EditTemplate
    std::vector<DivisionId> divisions;      // bulk assignment
};

// Why a command was rejected. Rejections never mutate state.
enum class CommandResult : uint8_t {
    Applied = 0,
    InvalidType,
    UnknownEntity,
    NotOwner,
    InsufficientResources,
    PrerequisitesMissing,
    InvalidTarget,
    QueueFull,
    SlotUnavailable,
    AlreadyAtWar,
    AtWar,
    InvalidValue,
    Count
};

const char* command_result_name(CommandResult r);

struct CommandRecord {
    Tick tick;
    uint32_t sequence;  // order within the tick
    Command command;
    CommandResult result;
};

// FIFO queue of commands awaiting application at the start of the next tick.
struct CommandQueue {
    std::vector<Command> pending;

    void push(Command c) { pending.push_back(std::move(c)); }
    [[nodiscard]] bool empty() const { return pending.empty(); }
    void clear() { pending.clear(); }
};

struct CommandLog {
    std::vector<CommandRecord> records;
    uint32_t next_sequence = 0;

    void record(Tick tick, const Command& c, CommandResult r) {
        records.push_back(CommandRecord{tick, next_sequence++, c, r});
    }
    void clear() {
        records.clear();
        next_sequence = 0;
    }
};

// Validation + application. `validate_command` is pure: it inspects state and
// returns whether the command can apply. `apply_command` performs the mutation and
// must only be called after validation succeeded.
CommandResult validate_command(const Game& game, const Command& cmd);
CommandResult apply_command(Game& game, const Command& cmd);

// Applies every queued command in order, recording results, then clears the queue.
// This is phase 1 of the tick.
void phase_commands(Game& game);

void serialize_command(ByteWriter& w, const Command& c);
Command deserialize_command(ByteReader& r);

}  // namespace hoi
