// Multiplayer lockstep: the network only moves commands, the simulation stays local.
//
// The engine is already deterministic (fixed tick order, seeded streams per subsystem,
// `world_hash` as the oracle), so multiplayer is a *transport* problem, not a simulation
// one: every peer runs the same simulation and applies the same commands on the same
// tick. What has to be right is the ordering rule, the tick barrier, and what happens
// when a peer disagrees.
//
// Rules that keep this honest:
//  * A peer may only issue commands for the country it was given (its seat). A command
//    for another seat is dropped with a diagnostic, never applied.
//  * Commands for a tick are applied in a deterministic order: by seat, then by the order
//    the seat submitted them. Every peer derives the same order from the same messages.
//  * The session does not advance a tick until every seat has submitted for it (or the
//    session is told the seat is behind, in which case it stalls rather than guessing).
//  * Peers exchange `world_hash` on a fixed cadence. On a mismatch the session stops and
//    reports the tick, the first differing subsystem (from the per-subsystem hashes) and
//    the command list of that tick, because a silent divergence is worse than a halt.
//
// `Lockstep` holds no sockets: `transport.h` moves its messages, and tests drive the
// same class in-process so the logic is verifiable without a network.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/commands.h"

namespace hoi {

struct Game;

// One seat at the table. Seat 0 is the host in the shipped CLI, but nothing here depends
// on that: the session only needs a stable, agreed seat order.
struct PeerSeat {
    uint32_t seat = 0;
    CountryId country;      // the country this seat may command
    std::string tag;        // for logs and the desync report
};

enum class NetMessageKind : uint8_t {
    Hello = 0,    // announce a seat (seat, country, tag)
    Commands,     // one seat's commands for a tick (seat, tick, commands)
    Applied,      // the ordered command list the host applied for a tick (tick, commands)
    Hash,         // a peer's world hash (seat, tick, hash, subsystem hashes in text)
    Desync,       // a peer found a mismatch (tick, hash, text = report)
    Bye,          // seat is leaving
    Count
};

struct NetMessage {
    NetMessageKind kind = NetMessageKind::Hello;
    uint32_t seat = 0;
    Tick tick = 0;
    uint64_t hash = 0;
    std::vector<Command> commands;
    std::string text;  // tag, or a rendered desync report
};

const char* net_message_kind_name(NetMessageKind kind);

// Deterministic framing: `encode` writes a length-prefixed record; `decode` consumes one
// record from the front of `buffer` and returns how many bytes it used (0 = incomplete,
// `std::string::npos`-style 0 with `ok=false` = malformed).
std::string encode_message(const NetMessage& msg);
size_t decode_message(const std::string& buffer, NetMessage* out, bool* ok, std::string* err);

// How often peers compare hashes, in ticks.
constexpr uint32_t kLockstepHashCadence = 24;

// The result of a session step, so a caller can log or halt without inspecting internals.
enum class LockstepResult : uint8_t {
    Ok = 0,
    Waiting,        // some seat has not submitted for the target tick
    Desynced,       // a peer disagreed; the report is in `desync_report()`
    NotMySeat,      // a command was offered for a seat this peer does not own
    Unknown,
};

class Lockstep {
  public:
    Lockstep() = default;
    // Seats must be handed over in a deterministic order (ascending seat): the session
    // sorts them, so two peers that receive the lobby in different orders agree anyway.
    Lockstep(std::vector<PeerSeat> seats, uint32_t local_seat);

    const std::vector<PeerSeat>& seats() const { return seats_; }
    uint32_t local_seat() const { return local_seat_; }
    CountryId local_country() const;

    // Queue one command from this peer for `tick`. Returns NotMySeat when the command's
    // country is not this seat's country.
    LockstepResult submit(Tick tick, const Command& cmd);

    // Mark this seat as having submitted for `tick` with an empty command list. Every seat
    // must submit for every tick - including "I have nothing to say" - or the barrier can
    // never open. Returns NotMySeat only when the session does not know this seat.
    LockstepResult submit_empty(Tick tick);

    // Hand a message received from the network to the session. Returns Desynced when the
    // message proves a divergence (the report is then available).
    LockstepResult receive(const NetMessage& msg);

    // Messages the caller must send, in a deterministic order. Drains the queue.
    std::vector<NetMessage> outgoing();

    // True when every seat has submitted for `tick`; `ordered` then receives the commands
    // in application order (by seat, then submission order).
    bool ready(Tick tick, std::vector<Command>* ordered) const;

    // Build the message a peer sends after applying a tick: its hash plus the per-subsystem
    // hashes rendered into `text` (so a mismatch can name the subsystem that drifted).
    NetMessage hash_message(const Game& g) const;

    // Compare a peer's hash report against this peer's own. On a mismatch the session is
    // marked desynced and the report names the tick and the first differing subsystem.
    LockstepResult check_hash(const Game& g, const NetMessage& msg);

    bool desynced() const { return desynced_; }
    const std::string& desync_report() const { return desync_report_; }

    // Ticks whose commands have been fully collected and can be applied now, ascending.
    std::vector<Tick> applicable() const;

    // Forget the collected commands for `tick` (called once applied).
    void mark_applied(Tick tick);

  private:
    std::vector<PeerSeat> seats_;
    uint32_t local_seat_ = 0;
    bool desynced_ = false;
    std::string desync_report_;
    // per tick: per seat, the submitted commands (seat order fixed by seats_)
    std::vector<Tick> ticks_;
    std::vector<std::vector<std::vector<Command>>> by_tick_;
    std::vector<NetMessage> outgoing_;
};

}  // namespace hoi