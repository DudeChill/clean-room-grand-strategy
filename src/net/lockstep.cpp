// Multiplayer lockstep session: the transport moves messages, this file owns the
// ordering rule, the tick barrier and the desync report. See `lockstep.h` for the
// contract; the notes below record the two decisions the header leaves open.
//
// 1) "A seat has submitted for a tick" has to be distinguishable from "a seat
//    submitted nothing". The header only gives us `by_tick_`, a per-tick vector of
//    per-seat command lists, so an empty list means "no submission" and a seat that
//    submits *zero* commands stores a single `CommandType::None` marker with the
//    seat's country. `None` is not a playable order (the command layer rejects it),
//    so it never collides with a real command and is filtered out of every ordered
//    list. `submit_empty(tick)` is the public way to store that marker - the barrier
//    must open for a seat that has nothing to say - while `submit` still refuses a
//    `None` command so the marker can never enter a list through the back door.
//
// 2) One seat sends at most one `Commands` message per tick: `submit` coalesces all
//    of a seat's commands for a tick into the queued message, and receiving a
//    `Commands` message *replaces* that seat's list for the tick. Duplicate delivery
//    is therefore idempotent and the submission order inside a seat is the order in
//    which it called `submit`. This assumes the transport preserves the order of one
//    seat's messages (a length-prefixed stream per peer does).

#include "net/lockstep.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/binio.h"
#include "game/game.h"
#include "save/save.h"

namespace hoi {
namespace {

constexpr size_t kNoSeat = static_cast<size_t>(-1);
// Sanity caps only: the length prefix is a u32, but a garbage length must not make
// the transport wait forever for bytes that will never come.
constexpr uint32_t kMaxRecordBytes = 1u << 25;  // 32 MiB
constexpr uint32_t kMaxCommandsPerRecord = 1u << 16;

// Seat order the whole session agrees on: ascending seat, ties broken by tag then
// country id. Every ordering rule below is derived from this one comparator.
bool seat_less(const PeerSeat& a, const PeerSeat& b) {
    if (a.seat != b.seat) return a.seat < b.seat;
    if (a.tag != b.tag) return a.tag < b.tag;
    return a.country.v < b.country.v;
}

size_t find_seat(const std::vector<PeerSeat>& seats, uint32_t seat) {
    for (size_t i = 0; i < seats.size(); ++i) {
        if (seats[i].seat == seat) return i;
    }
    return kNoSeat;
}

std::string hex64(uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string first_token(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    return line.substr(0, i);
}

// `ticks_` stays sorted and parallel to `by_tick_` so every derivation walks ticks in
// ascending order without sorting at read time.
size_t tick_slot(const std::vector<Tick>& ticks, Tick tick) {
    const auto it = std::lower_bound(ticks.begin(), ticks.end(), tick);
    if (it == ticks.end() || *it != tick) return kNoSeat;
    return static_cast<size_t>(it - ticks.begin());
}

size_t ensure_tick_slot(std::vector<Tick>& ticks,
                        std::vector<std::vector<std::vector<Command>>>& by_tick, Tick tick) {
    const auto it = std::lower_bound(ticks.begin(), ticks.end(), tick);
    const size_t idx = static_cast<size_t>(it - ticks.begin());
    if (it != ticks.end() && *it == tick) return idx;
    ticks.insert(it, tick);
    by_tick.insert(by_tick.begin() + static_cast<std::ptrdiff_t>(idx),
                   std::vector<std::vector<Command>>());
    return idx;
}

Command empty_submission_marker(const PeerSeat& seat, Tick tick) {
    Command c;
    c.type = CommandType::None;
    c.country = seat.country;
    c.issued_tick = tick;
    return c;
}

void set_seat_list(std::vector<Tick>& ticks,
                   std::vector<std::vector<std::vector<Command>>>& by_tick,
                   const std::vector<PeerSeat>& seats, Tick tick, size_t seat_index,
                   const std::vector<Command>& commands) {
    const size_t slot = ensure_tick_slot(ticks, by_tick, tick);
    auto& per = by_tick[slot];
    per.resize(seats.size());
    std::vector<Command> filtered;
    for (const Command& c : commands) {
        if (c.type != CommandType::None) filtered.push_back(c);
    }
    if (filtered.empty()) filtered.push_back(empty_submission_marker(seats[seat_index], tick));
    per[seat_index] = std::move(filtered);
}

void append_seat_command(std::vector<Tick>& ticks,
                         std::vector<std::vector<std::vector<Command>>>& by_tick,
                         const std::vector<PeerSeat>& seats, Tick tick, size_t seat_index,
                         const Command& cmd) {
    const size_t slot = ensure_tick_slot(ticks, by_tick, tick);
    auto& per = by_tick[slot];
    per.resize(seats.size());
    auto& list = per[seat_index];
    // A lone marker (empty submission) is replaced by the first real command.
    if (list.size() == 1 && list.front().type == CommandType::None) list.clear();
    list.push_back(cmd);
}

void erase_tick(std::vector<Tick>& ticks,
                std::vector<std::vector<std::vector<Command>>>& by_tick, Tick tick) {
    const size_t slot = tick_slot(ticks, tick);
    if (slot == kNoSeat) return;
    ticks.erase(ticks.begin() + static_cast<std::ptrdiff_t>(slot));
    by_tick.erase(by_tick.begin() + static_cast<std::ptrdiff_t>(slot));
}

// When the seat set changes, every stored tick gains or loses one slot. The per-seat
// lists move with their seat so pending submissions stay attached to the right peer.
void insert_seat_slot(std::vector<std::vector<std::vector<Command>>>& by_tick, size_t index) {
    for (auto& per : by_tick) {
        if (index <= per.size()) {
            per.insert(per.begin() + static_cast<std::ptrdiff_t>(index),
                       std::vector<Command>());
        } else {
            per.resize(index + 1);
        }
    }
}

void erase_seat_slot(std::vector<std::vector<std::vector<Command>>>& by_tick, size_t index) {
    for (auto& per : by_tick) {
        if (index < per.size()) per.erase(per.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

// Application order: seat order (seats_ is already sorted), submission order inside a
// seat. Markers are not commands.
std::vector<Command> concat_ordered(const std::vector<std::vector<Command>>& per) {
    std::vector<Command> out;
    for (const auto& seat_list : per) {
        for (const Command& c : seat_list) {
            if (c.type != CommandType::None) out.push_back(c);
        }
    }
    return out;
}

// One seat's list as it goes on the wire: the empty-submission marker never leaves this
// process, so "submitted nothing" is an empty `Commands` message.
std::vector<Command> wire_commands(const std::vector<Command>& stored) {
    std::vector<Command> out;
    for (const Command& c : stored) {
        if (c.type != CommandType::None) out.push_back(c);
    }
    return out;
}

// One queued `Commands` message per (seat, tick), always the seat's full accumulated
// list, so re-delivery is idempotent on the receiving side.
void queue_commands_message(std::vector<NetMessage>& outgoing, std::vector<Command> commands,
                            uint32_t seat, Tick tick) {
    for (NetMessage& m : outgoing) {
        if (m.kind == NetMessageKind::Commands && m.seat == seat && m.tick == tick) {
            m.commands = std::move(commands);
            return;
        }
    }
    NetMessage m;
    m.kind = NetMessageKind::Commands;
    m.seat = seat;
    m.tick = tick;
    m.commands = std::move(commands);
    outgoing.push_back(std::move(m));
}

std::string render_desync_report(const Game& g, const NetMessage& peer, uint64_t local_hash,
                                 const std::vector<PeerSeat>& seats, uint32_t local_seat,
                                 Tick command_tick, const std::vector<Command>& ordered) {
    const std::string local_report = hash_report(g);
    const std::vector<std::string> local_lines = split_lines(local_report);
    const std::vector<std::string> peer_lines = split_lines(peer.text);

    std::string first_diff = "(none)";
    const size_t lines = std::max(local_lines.size(), peer_lines.size());
    for (size_t i = 0; i < lines; ++i) {
        const std::string la = i < local_lines.size() ? local_lines[i] : std::string();
        const std::string lb = i < peer_lines.size() ? peer_lines[i] : std::string();
        if (la != lb) {
            first_diff = first_token(la.empty() ? lb : la);
            if (first_diff.empty()) first_diff = "(line " + std::to_string(i) + ")";
            break;
        }
    }

    const size_t local_index = find_seat(seats, local_seat);
    const std::string local_tag = local_index == kNoSeat ? "(unknown)" : seats[local_index].tag;
    const size_t peer_index = find_seat(seats, peer.seat);
    const std::string peer_tag = peer_index == kNoSeat ? "(unknown)" : seats[peer_index].tag;

    std::string out;
    out += "lockstep desync at tick " + std::to_string(peer.tick) + "\n";
    out += "  local  seat " + std::to_string(local_seat) + " (" + local_tag + ") hash 0x" +
           hex64(local_hash) + "\n";
    out += "  remote seat " + std::to_string(peer.seat) + " (" + peer_tag + ") hash 0x" +
           hex64(peer.hash) + "\n";
    out += "  first differing subsystem: " + first_diff + "\n";
    out += "  commands at tick " + std::to_string(command_tick) +
           " (seat order, then submission):\n";
    if (ordered.empty()) {
        out += "    (no commands collected for this tick)\n";
    } else {
        for (size_t i = 0; i < ordered.size(); ++i) {
            const Command& c = ordered[i];
            const Country* country = g.world.country(c.country);
            out += "    #" + std::to_string(i) + " type=" + command_type_name(c.type) +
                   " country=" + std::to_string(country ? country->id.v : c.country.v) +
                   (country ? " (" + country->tag + ")" : "") + " key='" + c.text +
                   "' issued_tick=" + std::to_string(c.issued_tick) + "\n";
        }
    }
    out += "  local hash report:\n" + local_report;
    return out;
}

}  // namespace

const char* net_message_kind_name(NetMessageKind kind) {
    switch (kind) {
        case NetMessageKind::Hello: return "hello";
        case NetMessageKind::Commands: return "commands";
        case NetMessageKind::Applied: return "applied";
        case NetMessageKind::Hash: return "hash";
        case NetMessageKind::Desync: return "desync";
        case NetMessageKind::Bye: return "bye";
        case NetMessageKind::Count: break;
    }
    return "unknown";
}

// Framing: [u32 little-endian payload length][payload]. The payload is a flat,
// unpadded, little-endian record:
//   u8  kind
//   u32 seat
//   u64 tick
//   u64 hash
//   u32 command count, then one `serialize_command` payload per command
//   u32 text length, then the UTF-8 bytes
// Every field goes through `core/binio.h`, so the bytes are the canonical save bytes.
std::string encode_message(const NetMessage& msg) {
    ByteWriter body;
    body.u8(static_cast<uint8_t>(msg.kind));
    body.u32(msg.seat);
    body.u64(msg.tick);
    body.u64(msg.hash);
    body.u32(static_cast<uint32_t>(msg.commands.size()));
    for (const Command& c : msg.commands) serialize_command(body, c);
    body.str(msg.text);

    ByteWriter frame;
    frame.u32(static_cast<uint32_t>(body.size()));
    frame.raw(body.data().data(), body.size());
    const auto& bytes = frame.data();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

size_t decode_message(const std::string& buffer, NetMessage* out, bool* ok, std::string* err) {
    auto malformed = [&](const std::string& reason) -> size_t {
        if (ok) *ok = false;
        if (err) *err = "lockstep.cpp: " + reason;
        if (out) *out = NetMessage{};
        return 0;
    };
    if (ok) *ok = true;
    if (err) err->clear();
    if (out) *out = NetMessage{};

    if (buffer.size() < 4) return 0;  // incomplete: length prefix not here yet
    const auto* p = reinterpret_cast<const uint8_t*>(buffer.data());
    const uint32_t length = static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                                 (static_cast<uint32_t>(p[3]) << 24));
    if (length > kMaxRecordBytes) {
        return malformed("record says " + std::to_string(length) +
                         " bytes, above the 32 MiB sanity cap");
    }
    if (buffer.size() < 4 + static_cast<size_t>(length)) return 0;  // incomplete payload

    ByteReader r(p + 4, length);
    NetMessage msg;
    uint8_t kind = 0;
    if (!r.u8(&kind)) return malformed("record is too short for a kind byte");
    if (kind >= static_cast<uint8_t>(NetMessageKind::Count)) {
        return malformed("message kind " + std::to_string(kind) + " is out of range");
    }
    msg.kind = static_cast<NetMessageKind>(kind);
    if (!r.u32(&msg.seat) || !r.u64(&msg.tick) || !r.u64(&msg.hash)) {
        return malformed("record header is truncated");
    }
    uint32_t count = 0;
    if (!r.u32(&count)) return malformed("record has no command count");
    if (count > kMaxCommandsPerRecord) {
        return malformed("record claims " + std::to_string(count) + " commands");
    }
    msg.commands.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        // `deserialize_command` does not bounds-check a zero-copy view itself; a
        // corrupt length field can make it throw, which is a malformed frame here.
        try {
            msg.commands.push_back(deserialize_command(r));
        } catch (const std::exception& e) {
            return malformed("command " + std::to_string(i) + " is malformed: " + e.what());
        }
        if (!r.ok()) return malformed("command " + std::to_string(i) + " is truncated");
    }
    if (!r.str(&msg.text)) return malformed("text field is truncated");
    if (!r.eof()) {
        return malformed("record has " + std::to_string(r.remaining()) + " trailing bytes");
    }
    if (out) *out = std::move(msg);
    return 4 + static_cast<size_t>(length);
}

Lockstep::Lockstep(std::vector<PeerSeat> seats, uint32_t local_seat)
    : seats_(std::move(seats)), local_seat_(local_seat) {
    std::sort(seats_.begin(), seats_.end(), seat_less);
    seats_.erase(std::unique(seats_.begin(), seats_.end(),
                             [](const PeerSeat& a, const PeerSeat& b) { return a.seat == b.seat; }),
                 seats_.end());
}

CountryId Lockstep::local_country() const {
    const size_t i = find_seat(seats_, local_seat_);
    return i == kNoSeat ? CountryId{} : seats_[i].country;
}

LockstepResult Lockstep::submit(Tick tick, const Command& cmd) {
    if (desynced_) return LockstepResult::Desynced;
    const size_t local = find_seat(seats_, local_seat_);
    if (local == kNoSeat) return LockstepResult::Unknown;
    // `None` is not a playable order and is reserved as the in-memory empty-submission
    // marker: a seat with nothing to say calls `submit_empty`, never `submit(None)`.
    if (cmd.type == CommandType::None) return LockstepResult::Unknown;

    // A peer may only command the country it was given: anything else is refused and
    // stores nothing.
    const CountryId mine = seats_[local].country;
    if (!mine.valid() || cmd.country != mine) return LockstepResult::NotMySeat;

    append_seat_command(ticks_, by_tick_, seats_, tick, local, cmd);
    const size_t slot = tick_slot(ticks_, tick);
    if (slot == kNoSeat) return LockstepResult::Unknown;
    queue_commands_message(outgoing_, wire_commands(by_tick_[slot][local]), local_seat_, tick);
    return LockstepResult::Ok;
}

LockstepResult Lockstep::submit_empty(Tick tick) {
    if (desynced_) return LockstepResult::Desynced;
    const size_t local = find_seat(seats_, local_seat_);
    if (local == kNoSeat) return LockstepResult::NotMySeat;

    const size_t slot = ensure_tick_slot(ticks_, by_tick_, tick);
    by_tick_[slot].resize(seats_.size());
    // A no-op when the seat already submitted real commands for this tick (or already
    // submitted nothing): "submitted nothing" is only recorded when nothing else was.
    if (by_tick_[slot][local].empty()) {
        by_tick_[slot][local].push_back(empty_submission_marker(seats_[local], tick));
    }
    queue_commands_message(outgoing_, wire_commands(by_tick_[slot][local]), local_seat_, tick);
    return LockstepResult::Ok;
}

LockstepResult Lockstep::receive(const NetMessage& msg) {
    switch (msg.kind) {
        case NetMessageKind::Hello: {
            const size_t existing = find_seat(seats_, msg.seat);
            if (existing != kNoSeat) {
                if (!msg.text.empty()) seats_[existing].tag = msg.text;
                return LockstepResult::Ok;
            }
            PeerSeat seat;
            seat.seat = msg.seat;
            seat.tag = msg.text;
            // The wire struct has no country field; an announcer that wants its seat
            // to be commandable carries its country on a placeholder command.
            if (!msg.commands.empty()) seat.country = msg.commands.front().country;
            seats_.push_back(seat);
            std::sort(seats_.begin(), seats_.end(), seat_less);
            const size_t index = find_seat(seats_, msg.seat);
            if (index != kNoSeat) insert_seat_slot(by_tick_, index);
            return LockstepResult::Ok;
        }
        case NetMessageKind::Bye: {
            const size_t index = find_seat(seats_, msg.seat);
            if (index == kNoSeat) return LockstepResult::Ok;
            erase_seat_slot(by_tick_, index);
            seats_.erase(seats_.begin() + static_cast<std::ptrdiff_t>(index));
            return LockstepResult::Ok;
        }
        case NetMessageKind::Commands: {
            if (desynced_) return LockstepResult::Desynced;
            // Our own broadcast echoed back: the commands are already stored locally.
            if (msg.seat == local_seat_) return LockstepResult::Ok;
            const size_t index = find_seat(seats_, msg.seat);
            if (index == kNoSeat) return LockstepResult::Unknown;
            const CountryId seat_country = seats_[index].country;
            if (seat_country.valid()) {
                for (const Command& c : msg.commands) {
                    if (c.country != seat_country) return LockstepResult::Unknown;
                }
            }
            set_seat_list(ticks_, by_tick_, seats_, msg.tick, index, msg.commands);
            return LockstepResult::Ok;
        }
        case NetMessageKind::Applied: {
            if (desynced_) return LockstepResult::Desynced;
            if (seats_.empty()) return LockstepResult::Ok;
            // When this tick is already fully satisfied locally, our own derivation is
            // authoritative: a re-delivered or forged Applied must not reorder a tick
            // this peer has already completed.
            if (ready(msg.tick, nullptr)) return LockstepResult::Ok;
            // The host's order becomes this tick's authoritative list: the first seat
            // carries it and every other seat is marked satisfied.
            set_seat_list(ticks_, by_tick_, seats_, msg.tick, 0, msg.commands);
            for (size_t i = 1; i < seats_.size(); ++i) {
                set_seat_list(ticks_, by_tick_, seats_, msg.tick, i, {});
            }
            return LockstepResult::Ok;
        }
        case NetMessageKind::Hash:
            if (desynced_) return LockstepResult::Desynced;
            // The session holds no world, so a hash cannot be compared here: the
            // caller that owns the `Game` calls `check_hash(g, msg)`.
            return LockstepResult::Ok;
        case NetMessageKind::Desync: {
            if (desynced_) return LockstepResult::Desynced;
            desynced_ = true;
            if (!msg.text.empty()) {
                desync_report_ = msg.text;
            } else {
                desync_report_ = "lockstep desync reported by seat " +
                                 std::to_string(msg.seat) + " at tick " +
                                 std::to_string(msg.tick) + " (no report text)";
            }
            return LockstepResult::Desynced;
        }
        case NetMessageKind::Count:
            break;
    }
    return LockstepResult::Unknown;
}

std::vector<NetMessage> Lockstep::outgoing() {
    std::vector<NetMessage> out;
    out.swap(outgoing_);
    return out;
}

bool Lockstep::ready(Tick tick, std::vector<Command>* ordered) const {
    if (desynced_) return false;
    const size_t slot = tick_slot(ticks_, tick);
    if (slot == kNoSeat) return false;
    const auto& per = by_tick_[slot];
    if (per.size() != seats_.size()) return false;
    for (const auto& list : per) {
        if (list.empty()) return false;
    }
    if (ordered) *ordered = concat_ordered(per);
    return true;
}

NetMessage Lockstep::hash_message(const Game& g) const {
    NetMessage msg;
    msg.kind = NetMessageKind::Hash;
    msg.seat = local_seat_;
    msg.tick = g.world.tick;
    msg.hash = world_hash(g);
    msg.text = hash_report(g);
    return msg;
}

LockstepResult Lockstep::check_hash(const Game& g, const NetMessage& msg) {
    if (desynced_) return LockstepResult::Desynced;
    const uint64_t local = world_hash(g);
    if (local == msg.hash) return LockstepResult::Ok;

    // The hash names the world's tick - the state that was compared, i.e. what the
    // sender's `hash_message` tagged. The commands that produced that state belong to
    // the tick just before it, so fall back one tick when the tagged tick has no
    // collected list of its own; the report labels the tick it actually lists.
    Tick command_tick = msg.tick;
    std::vector<Command> ordered;
    const size_t slot = tick_slot(ticks_, msg.tick);
    if (slot != kNoSeat) {
        ordered = concat_ordered(by_tick_[slot]);
    } else if (msg.tick > 0) {
        const size_t previous = tick_slot(ticks_, msg.tick - 1);
        if (previous != kNoSeat) {
            ordered = concat_ordered(by_tick_[previous]);
            command_tick = msg.tick - 1;
        }
    }

    desynced_ = true;
    desync_report_ =
        render_desync_report(g, msg, local, seats_, local_seat_, command_tick, ordered);
    return LockstepResult::Desynced;
}

std::vector<Tick> Lockstep::applicable() const {
    std::vector<Tick> out;
    if (desynced_ || ticks_.empty()) return out;
    Tick expect = ticks_.front();
    for (Tick t : ticks_) {
        // Only a contiguous run from the oldest unapplied tick is applicable; a gap
        // or a not-yet-ready tick stalls the session rather than skipping ahead.
        if (t != expect) break;
        if (!ready(t, nullptr)) break;
        out.push_back(t);
        ++expect;
    }
    return out;
}

void Lockstep::mark_applied(Tick tick) { erase_tick(ticks_, by_tick_, tick); }

}  // namespace hoi