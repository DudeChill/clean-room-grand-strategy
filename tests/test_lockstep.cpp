// Multiplayer lockstep tests (MP-001). Everything runs in-process: no sockets, no
// threads, no wall clock. Two `Lockstep` instances stand in for the two machines, and
// the messages one queues are handed to the other exactly as a transport would.
//
// The properties pinned here are the ones a split-brain or a silent divergence would
// violate: the seat order is identical no matter what order the lobby arrives in, a
// tick is not applicable until every seat submitted, the application order is
// (seat, submission), a command for another seat is refused, a real divergence is
// caught and named, and two peers driven by the same messages stay hash-identical
// over a long run.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/binio.h"
#include "game/game.h"
#include "net/lockstep.h"
#include "save/save.h"
#include "sim/commands.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

std::string repo_root() {
    std::string file(__FILE__);
    const std::string suffix = "/tests/test_lockstep.cpp";
    if (file.size() > suffix.size() &&
        file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return file.substr(0, file.size() - suffix.size());
    }
    return ".";
}

bool same_command(const Command& a, const Command& b) {
    ByteWriter wa, wb;
    serialize_command(wa, a);
    serialize_command(wb, b);
    return wa.data() == wb.data();
}

bool same_commands(const std::vector<Command>& a, const std::vector<Command>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!same_command(a[i], b[i])) return false;
    }
    return true;
}

Command make_cmd(CommandType type, CountryId country, Tick tick) {
    Command c;
    c.type = type;
    c.country = country;
    c.issued_tick = tick;
    return c;
}

std::string hex64(uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

int subsystem_from_name(const std::string& name) {
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        if (name == subsystem_name(static_cast<Subsystem>(i))) return i;
    }
    return -1;
}

std::string extract_after(const std::string& haystack, const std::string& marker) {
    const size_t at = haystack.find(marker);
    if (at == std::string::npos) return std::string();
    const size_t start = at + marker.size();
    const size_t end = haystack.find('\n', start);
    return haystack.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

CountryId find_country(const Game& g, const std::string& tag) {
    for (uint32_t i = 0; i < g.world.countries.capacity(); ++i) {
        const CountryId c(i);
        const Country* p = g.world.countries.try_get(c);
        if (p && p->alive && p->tag == tag) return c;
    }
    return CountryId{};
}

void first_two_alive(const Game& g, CountryId* a, CountryId* b) {
    *a = CountryId{};
    *b = CountryId{};
    for (uint32_t i = 0; i < g.world.countries.capacity(); ++i) {
        const CountryId c(i);
        if (!g.world.countries.alive(c)) continue;
        if (!a->valid()) {
            *a = c;
        } else if (!b->valid()) {
            *b = c;
            return;
        }
    }
}

std::vector<PeerSeat> two_seats(const MiniWorld& m) {
    return {PeerSeat{0, m.a, "AAA"}, PeerSeat{1, m.b, "BBB"}};
}

}  // namespace

HOI_TEST(LOCKSTEP_001_encode_decode_round_trips_every_kind) {
    for (int k = 0; k < static_cast<int>(NetMessageKind::Count); ++k) {
        NetMessage m;
        m.kind = static_cast<NetMessageKind>(k);
        m.seat = 3 + static_cast<uint32_t>(k);
        m.tick = 1000 + static_cast<Tick>(k) * 7;
        m.hash = 0x00DEADBEEF00ull + static_cast<uint64_t>(k);
        m.text = std::string("kind-") + net_message_kind_name(m.kind);

        const std::string bytes = encode_message(m);
        NetMessage back;
        bool ok = false;
        std::string err;
        const size_t used = decode_message(bytes, &back, &ok, &err);
        CHECK(ok);
        CHECK(err.empty());
        CHECK_EQ(used, bytes.size());
        CHECK(back.kind == m.kind);
        CHECK_EQ(back.seat, m.seat);
        CHECK_EQ(back.tick, m.tick);
        CHECK_EQ(back.hash, m.hash);
        CHECK_EQ(back.text, m.text);
        CHECK(back.commands.empty());
        // Re-encoding the decoded message reproduces the exact bytes.
        CHECK_EQ(encode_message(back), bytes);
    }

    // `Count` is not a wire value: it must be rejected as malformed, not accepted.
    NetMessage sentinel;
    sentinel.kind = NetMessageKind::Count;
    const std::string bytes = encode_message(sentinel);
    NetMessage back;
    bool ok = true;
    std::string err;
    CHECK_EQ(decode_message(bytes, &back, &ok, &err), size_t{0});
    CHECK(!ok);
    CHECK(!err.empty());
}

HOI_TEST(LOCKSTEP_002_encode_decode_round_trips_commands_byte_identically) {
    // Every payload field is set on at least one command so the framing cannot drop
    // one silently: scalars, ids, text, battalions, divisions and designer components.
    Command a;
    a.type = CommandType::CreateTemplate;
    a.country = CountryId{7};
    a.issued_tick = 4242;
    a.province = ProvinceId{3};
    a.province_b = ProvinceId{4};
    a.state = StateId{5};
    a.region = RegionId{6};
    a.division = DivisionId{8};
    a.army = ArmyId{9};
    a.character = CharacterId{10};
    a.wing = AirWingId{11};
    a.fleet_id = FleetId{12};
    a.ship_id = ShipId{13};
    a.task_force = TaskForceId{14};
    a.equipment = EquipmentId{15};
    a.template_id = TemplateId{16};
    a.tech = TechId{17};
    a.target_country = CountryId{18};
    a.war = WarId{19};
    a.value = -3;
    a.value_f = 1.25;
    a.text = "3. Panzer \xE2\x80\x94 armoured";  // UTF-8 survives the framing
    BattalionSlot b;
    b.equipment = EquipmentId{2};
    b.count = 9;
    b.support = true;
    a.battalions.push_back(b);
    a.divisions = {DivisionId{1}, DivisionId{5}};
    a.components = {{3, 11}, {0, 4}};

    Command c = make_cmd(CommandType::StartConstruction, CountryId{2}, 7);
    c.state = StateId{1};
    c.value = 1;
    Command d = make_cmd(CommandType::ToggleFuelPriority, CountryId{9}, 7);

    NetMessage m;
    m.kind = NetMessageKind::Commands;
    m.seat = 7;
    m.tick = 4242;
    m.hash = 0x123456789ABCDEFull;
    m.commands = {a, c, d};
    m.text = "several commands";

    const std::string bytes = encode_message(m);
    NetMessage back;
    bool ok = false;
    std::string err;
    const size_t used = decode_message(bytes, &back, &ok, &err);
    CHECK(ok);
    CHECK(err.empty());
    CHECK_EQ(used, bytes.size());
    CHECK_EQ(back.commands.size(), size_t{3});
    CHECK(same_command(back.commands[0], a));
    CHECK(same_command(back.commands[1], c));
    CHECK(same_command(back.commands[2], d));
    CHECK_EQ(encode_message(back), bytes);

    // Two records concatenated on a stream are consumed one at a time, in order.
    const std::string stream = bytes + encode_message(m);
    size_t offset = 0;
    for (int i = 0; i < 2; ++i) {
        NetMessage out;
        bool record_ok = false;
        std::string record_err;
        const size_t consumed =
            decode_message(stream.substr(offset), &out, &record_ok, &record_err);
        CHECK(record_ok);
        CHECK_EQ(consumed, bytes.size());
        CHECK(same_commands(out.commands, m.commands));
        offset += consumed;
    }
    CHECK_EQ(offset, stream.size());
}

HOI_TEST(LOCKSTEP_003_reversed_lobby_order_agrees_on_seats_and_readiness) {
    MiniWorld m = make_mini_world();
    std::vector<PeerSeat> forward = two_seats(m);
    std::vector<PeerSeat> reversed = {forward[1], forward[0]};

    Lockstep a(forward, 0);
    Lockstep b(reversed, 1);

    CHECK_EQ(a.seats().size(), size_t{2});
    CHECK_EQ(b.seats().size(), size_t{2});
    CHECK_EQ(a.seats()[0].seat, 0u);
    CHECK_EQ(a.seats()[1].seat, 1u);
    CHECK_EQ(b.seats()[0].seat, 0u);
    CHECK_EQ(b.seats()[1].seat, 1u);
    CHECK_EQ(a.seats()[0].tag, b.seats()[0].tag);
    CHECK_EQ(a.seats()[1].tag, b.seats()[1].tag);
    CHECK_EQ(a.seats()[0].country.v, b.seats()[0].country.v);
    CHECK_EQ(a.seats()[1].country.v, b.seats()[1].country.v);
    CHECK_EQ(a.local_country().v, m.a.v);
    CHECK_EQ(b.local_country().v, m.b.v);

    const Tick t = 11;
    CHECK(a.submit(t, make_cmd(CommandType::ToggleFuelPriority, m.a, t)) == LockstepResult::Ok);
    CHECK(b.submit(t, make_cmd(CommandType::ToggleFuelPriority, m.b, t)) == LockstepResult::Ok);

    std::vector<NetMessage> to_b = a.outgoing();
    std::vector<NetMessage> to_a = b.outgoing();
    for (const NetMessage& msg : to_a) CHECK(a.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : to_b) CHECK(b.receive(msg) == LockstepResult::Ok);

    std::vector<Command> ordered_a;
    std::vector<Command> ordered_b;
    CHECK(a.ready(t, &ordered_a));
    CHECK(b.ready(t, &ordered_b));
    CHECK(same_commands(ordered_a, ordered_b));
    CHECK_EQ(ordered_a.size(), size_t{2});
    CHECK_EQ(ordered_a[0].country.v, m.a.v);
    CHECK_EQ(ordered_a[1].country.v, m.b.v);

    const std::vector<Tick> applicable_a = a.applicable();
    const std::vector<Tick> applicable_b = b.applicable();
    CHECK_EQ(applicable_a.size(), size_t{1});
    CHECK_EQ(applicable_b.size(), size_t{1});
    CHECK_EQ(applicable_a[0], t);
    CHECK_EQ(applicable_b[0], t);
}

HOI_TEST(LOCKSTEP_004_a_tick_waiting_for_a_missing_seat_is_not_applicable) {
    MiniWorld m = make_mini_world();
    const std::vector<PeerSeat> seats = two_seats(m);
    Lockstep a(seats, 0);
    Lockstep b(seats, 1);

    const Tick t = 5;
    CHECK(a.submit(t, make_cmd(CommandType::ToggleFuelPriority, m.a, t)) == LockstepResult::Ok);
    CHECK(!a.ready(t, nullptr));
    CHECK(a.applicable().empty());

    for (const NetMessage& msg : a.outgoing()) CHECK(b.receive(msg) == LockstepResult::Ok);
    // B has seen A's submission but has not submitted: the session stalls, it does not
    // advance with a guessed order.
    CHECK(!b.ready(t, nullptr));
    CHECK(b.applicable().empty());

    CHECK(b.submit(t, make_cmd(CommandType::ToggleFuelPriority, m.b, t)) == LockstepResult::Ok);
    CHECK(b.ready(t, nullptr));
    const std::vector<Tick> applicable = b.applicable();
    CHECK_EQ(applicable.size(), size_t{1});
    CHECK_EQ(applicable[0], t);
}

HOI_TEST(LOCKSTEP_005_two_seats_apply_in_seat_then_submission_order) {
    MiniWorld m1 = make_mini_world();
    MiniWorld m2 = make_mini_world();
    const std::vector<PeerSeat> seats = two_seats(m1);
    Lockstep p0(seats, 0);
    Lockstep p1(seats, 1);

    const Tick t = 0;
    const Command first = make_cmd(CommandType::ToggleFuelPriority, m1.a, t);
    Command second = make_cmd(CommandType::StartConstruction, m1.a, t);
    second.state = m1.state_a;
    second.value = 0;
    const Command third = make_cmd(CommandType::ToggleFuelPriority, m1.b, t);

    // Seat 0 submits twice, seat 1 once; submissions are interleaved on the wire.
    CHECK(p0.submit(t, first) == LockstepResult::Ok);
    CHECK(p0.submit(t, second) == LockstepResult::Ok);
    CHECK(p1.submit(t, third) == LockstepResult::Ok);

    std::vector<NetMessage> from_0 = p0.outgoing();
    std::vector<NetMessage> from_1 = p1.outgoing();
    for (const NetMessage& msg : from_1) CHECK(p0.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : from_0) CHECK(p1.receive(msg) == LockstepResult::Ok);

    std::vector<Command> ordered_0;
    std::vector<Command> ordered_1;
    CHECK(p0.ready(t, &ordered_0));
    CHECK(p1.ready(t, &ordered_1));
    CHECK(same_commands(ordered_0, ordered_1));

    const std::vector<Command> expected = {first, second, third};
    CHECK(same_commands(ordered_0, expected));

    // Both peers execute the same program on identical worlds and land on the same hash.
    for (const Command& c : ordered_0) m1.game.queue.push(c);
    for (const Command& c : ordered_1) m2.game.queue.push(c);
    m1.game.tick_once();
    m2.game.tick_once();
    CHECK_EQ(world_hash(m1.game), world_hash(m2.game));
}

HOI_TEST(LOCKSTEP_006_a_command_for_another_seat_is_refused) {
    MiniWorld m = make_mini_world();
    const std::vector<PeerSeat> seats = two_seats(m);
    Lockstep a(seats, 0);
    Lockstep b(seats, 1);

    const Tick t = 0;
    const Command foreign = make_cmd(CommandType::ToggleFuelPriority, m.b, t);
    CHECK(a.submit(t, foreign) == LockstepResult::NotMySeat);
    CHECK(a.outgoing().empty());
    CHECK(!a.ready(t, nullptr));
    CHECK(a.applicable().empty());

    for (const NetMessage& msg : a.outgoing()) CHECK(b.receive(msg) == LockstepResult::Ok);
    CHECK(!b.ready(t, nullptr));
    CHECK(b.applicable().empty());

    // A `None` command is not a playable order and must not become an empty-submission
    // marker either: it is refused and stores nothing.
    Command none;
    none.type = CommandType::None;
    none.country = m.a;
    CHECK(a.submit(t, none) == LockstepResult::Unknown);
    CHECK(a.outgoing().empty());
    CHECK(!a.ready(t, nullptr));

    // The seat's own, valid command still works and B still sees exactly one list.
    CHECK(a.submit(t, make_cmd(CommandType::ToggleFuelPriority, m.a, t)) == LockstepResult::Ok);
    for (const NetMessage& msg : a.outgoing()) CHECK(b.receive(msg) == LockstepResult::Ok);
    std::vector<Command> ordered;
    CHECK(!b.ready(t, &ordered));
}

HOI_TEST(LOCKSTEP_007_hash_mismatch_names_the_tick_and_first_differing_subsystem) {
    MiniWorld m1 = make_mini_world(true);
    MiniWorld m2 = make_mini_world(true);
    CHECK_EQ(world_hash(m1.game), world_hash(m2.game));

    // Diverge for real: only one of the two worlds flips its fuel priority.
    CHECK(apply_command(m1.game, make_cmd(CommandType::ToggleFuelPriority, m1.a, 0)) ==
          CommandResult::Applied);
    CHECK(world_hash(m1.game) != world_hash(m2.game));

    const std::vector<PeerSeat> seats = {{0, m1.a, "AAA"}, {1, m2.b, "BBB"}};
    Lockstep local(seats, 0);

    const Tick t = m1.game.world.tick;
    CHECK(local.submit(t, make_cmd(CommandType::ToggleFuelPriority, m1.a, t)) ==
          LockstepResult::Ok);

    // Same hash: no desync.
    const NetMessage own = local.hash_message(m1.game);
    CHECK(local.check_hash(m1.game, own) == LockstepResult::Ok);
    CHECK(!local.desynced());

    // The peer's hash comes from the diverged world.
    Lockstep peer_session(seats, 1);
    const NetMessage peer = peer_session.hash_message(m2.game);
    CHECK_EQ(peer.hash, world_hash(m2.game));

    CHECK(local.check_hash(m1.game, peer) == LockstepResult::Desynced);
    CHECK(local.desynced());

    const std::string report = local.desync_report();
    CHECK(report.find("tick " + std::to_string(t)) != std::string::npos);
    CHECK(report.find("seat 0 (AAA)") != std::string::npos);
    CHECK(report.find("seat 1 (BBB)") != std::string::npos);
    CHECK(report.find(hex64(world_hash(m1.game))) != std::string::npos);
    CHECK(report.find(hex64(world_hash(m2.game))) != std::string::npos);
    // The tick's ordered command list is in the report: key, country, type.
    CHECK(report.find("type=toggle_fuel_priority") != std::string::npos);
    CHECK(report.find("key=''") != std::string::npos);
    CHECK(report.find("country=" + std::to_string(m1.a.v) + " (VLA)") != std::string::npos);

    // The named subsystem is real: every earlier subsystem hashes the same, and the
    // named one genuinely differs between the two worlds.
    const std::string named = extract_after(report, "first differing subsystem: ");
    const int index = subsystem_from_name(named);
    CHECK(index >= 0);
    for (int s = 0; s < index; ++s) {
        CHECK_EQ(subsystem_hash(m1.game, static_cast<Subsystem>(s)),
                 subsystem_hash(m2.game, static_cast<Subsystem>(s)));
    }
    CHECK(subsystem_hash(m1.game, static_cast<Subsystem>(index)) !=
          subsystem_hash(m2.game, static_cast<Subsystem>(index)));

    // A desync stops the session: no more submissions, no applicable ticks.
    CHECK(!local.ready(t, nullptr));
    CHECK(local.applicable().empty());
    CHECK(local.submit(t, make_cmd(CommandType::ToggleFuelPriority, m1.a, t)) ==
          LockstepResult::Desynced);

    // A peer's Desync message is adopted as this session's report.
    Lockstep other(seats, 1);
    NetMessage desync;
    desync.kind = NetMessageKind::Desync;
    desync.seat = 0;
    desync.tick = t;
    desync.text = "peer says the worlds differ at tick " + std::to_string(t);
    CHECK(other.receive(desync) == LockstepResult::Desynced);
    CHECK(other.desynced());
    CHECK_EQ(other.desync_report(), desync.text);
    CHECK(other.receive(desync) == LockstepResult::Desynced);
}

HOI_TEST(LOCKSTEP_008_two_peers_stay_hash_identical_for_60_days) {
    // The shipped 1936 campaign drives this test (real content, real map, real
    // economy). AI is disabled so the only inputs are the two seats' commands; the
    // lockstep session, not the AI, is what must keep the two worlds identical.
    const std::string root = repo_root();
    const std::string scenario = root + "/data/scenarios/1936.json";
    Game ga;
    Game gb;
    std::string err;
    if (!Game::create(root + "/data", scenario, 20260101, &ga, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "scenario load failed: " + err);
    }
    if (!Game::create(root + "/data", scenario, 20260101, &gb, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "scenario load failed: " + err);
    }

    CountryId ca = find_country(ga, "GER");
    CountryId cb = find_country(ga, "SOV");
    if (!ca.valid() || !cb.valid()) {
        first_two_alive(ga, &ca, &cb);
    }
    if (!ca.valid() || !cb.valid()) {
        ::hoi_test::fail(__FILE__, __LINE__, "scenario has fewer than two playable countries");
    }
    const std::string tag_a = ga.world.country(ca)->tag;
    const std::string tag_b = ga.world.country(cb)->tag;

    for (uint32_t i = 0; i < ga.world.countries.capacity(); ++i) {
        const CountryId c(i);
        if (!ga.world.countries.alive(c)) continue;
        ga.set_ai(c, false);
        gb.set_ai(c, false);
    }

    const std::vector<PeerSeat> seats = {{0, ca, tag_a}, {1, cb, tag_b}};
    Lockstep host(seats, 0);
    Lockstep joiner(seats, 1);
    CHECK_EQ(host.local_country().v, ca.v);
    CHECK_EQ(joiner.local_country().v, cb.v);
    CHECK_EQ(world_hash(ga), world_hash(gb));

    const StateId host_capital = ga.world.country(ca)->capital;
    int applied_records = 0;

    for (int day = 0; day < 60; ++day) {
        const Tick tick = ga.world.tick;
        CHECK_EQ(gb.world.tick, tick);

        Command host_cmd = make_cmd(CommandType::ToggleFuelPriority, ca, tick);
        CHECK(host.submit(tick, host_cmd) == LockstepResult::Ok);
        Command joiner_cmd = make_cmd(CommandType::ToggleFuelPriority, cb, tick);
        CHECK(joiner.submit(tick, joiner_cmd) == LockstepResult::Ok);
        // The host also queues real construction so a command that mutates the world
        // (not just a flag) flows through the pipeline.
        Command build = make_cmd(CommandType::StartConstruction, ca, tick);
        build.state = host_capital;
        build.value = 0;
        CHECK(host.submit(tick, build) == LockstepResult::Ok);

        for (const NetMessage& msg : host.outgoing()) {
            CHECK(joiner.receive(msg) == LockstepResult::Ok);
        }
        for (const NetMessage& msg : joiner.outgoing()) {
            CHECK(host.receive(msg) == LockstepResult::Ok);
        }

        std::vector<Command> host_order;
        std::vector<Command> joiner_order;
        CHECK(host.ready(tick, &host_order));
        CHECK(joiner.ready(tick, &joiner_order));
        CHECK(same_commands(host_order, joiner_order));
        CHECK_EQ(host_order.size(), size_t{3});

        for (const Command& c : host_order) ga.queue.push(c);
        for (const Command& c : joiner_order) gb.queue.push(c);
        ga.run_ticks(TICKS_PER_DAY);
        gb.run_ticks(TICKS_PER_DAY);
        host.mark_applied(tick);
        joiner.mark_applied(tick);

        // Hash exchange on the fixed cadence, both directions.
        if (tick % kLockstepHashCadence == 0) {
            const NetMessage from_host = host.hash_message(ga);
            const NetMessage from_joiner = joiner.hash_message(gb);
            CHECK(joiner.check_hash(gb, from_host) == LockstepResult::Ok);
            CHECK(host.check_hash(ga, from_joiner) == LockstepResult::Ok);
        }

        CHECK_EQ(world_hash(ga), world_hash(gb));
    }

    CHECK(!host.desynced());
    CHECK(!joiner.desynced());
    CHECK_EQ(world_hash(ga), world_hash(gb));
    CHECK_EQ(ga.world.tick, 60 * static_cast<Tick>(TICKS_PER_DAY));

    for (const CommandRecord& r : ga.log.records) {
        if (r.result == CommandResult::Applied) ++applied_records;
    }
    CHECK_GT(applied_records, 0);
}

HOI_TEST(LOCKSTEP_009_decode_handles_incomplete_and_malformed_buffers) {
    NetMessage m;
    m.kind = NetMessageKind::Commands;
    m.seat = 1;
    m.tick = 9;
    m.hash = 77;
    m.text = "hello";
    m.commands = {make_cmd(CommandType::ToggleFuelPriority, CountryId{0}, 9)};
    const std::string bytes = encode_message(m);

    // Every strict prefix is "incomplete": 0 bytes consumed, ok stays true so the
    // transport knows to wait for more. Nothing crashes.
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        NetMessage out;
        out.seat = 12345;
        bool ok = false;
        std::string err;
        const size_t used = decode_message(bytes.substr(0, cut), &out, &ok, &err);
        CHECK_EQ(used, size_t{0});
        CHECK(ok);
        CHECK(err.empty());
    }
    {
        NetMessage out;
        bool ok = false;
        std::string err;
        CHECK_EQ(decode_message(bytes, &out, &ok, &err), bytes.size());
        CHECK(ok);
        CHECK_EQ(out.seat, 1u);
    }

    // Malformed: kind out of range.
    {
        const std::string bad = std::string("\x01\x00\x00\x00\x09", 5);
        NetMessage out;
        bool ok = true;
        std::string err;
        CHECK_EQ(decode_message(bad, &out, &ok, &err), size_t{0});
        CHECK(!ok);
        CHECK(err.find("lockstep.cpp:") == 0);
    }
    // Malformed: length prefix above the sanity cap.
    {
        const std::string bad = std::string("\xff\xff\xff\xff", 4);
        NetMessage out;
        bool ok = true;
        std::string err;
        CHECK_EQ(decode_message(bad, &out, &ok, &err), size_t{0});
        CHECK(!ok);
        CHECK(!err.empty());
    }
    // Malformed: internally inconsistent payload (command count far beyond the data).
    {
        ByteWriter body;
        body.u8(static_cast<uint8_t>(NetMessageKind::Commands));
        body.u32(0);
        body.u64(0);
        body.u64(0);
        body.u32(0xFFFFFFFFu);
        ByteWriter frame;
        frame.u32(static_cast<uint32_t>(body.size()));
        frame.raw(body.data().data(), body.size());
        const std::string bad(reinterpret_cast<const char*>(frame.data().data()),
                              frame.data().size());
        NetMessage out;
        bool ok = true;
        std::string err;
        CHECK_EQ(decode_message(bad, &out, &ok, &err), size_t{0});
        CHECK(!ok);
        CHECK(!err.empty());
    }
    // Malformed: a well-shaped record with trailing bytes.
    {
        ByteWriter body;
        body.u8(static_cast<uint8_t>(NetMessageKind::Hello));
        body.u32(1);
        body.u64(0);
        body.u64(0);
        body.u32(0);
        body.str("tag");
        body.u8(0xFF);  // stray byte inside the declared payload
        ByteWriter frame;
        frame.u32(static_cast<uint32_t>(body.size()));
        frame.raw(body.data().data(), body.size());
        const std::string bad(reinterpret_cast<const char*>(frame.data().data()),
                              frame.data().size());
        NetMessage out;
        bool ok = true;
        std::string err;
        CHECK_EQ(decode_message(bad, &out, &ok, &err), size_t{0});
        CHECK(!ok);
        CHECK(!err.empty());
    }
}

HOI_TEST(LOCKSTEP_013_report_finds_the_applied_tick_commands) {
    // A peer hashing right after applying tick T tags the message with the world's
    // tick (T+1). The report must still name T's commands, because those are the ones
    // that produced the compared state.
    MiniWorld d1 = make_mini_world(true);
    MiniWorld d2 = make_mini_world(true);
    CHECK_EQ(world_hash(d1.game), world_hash(d2.game));

    const std::vector<PeerSeat> seats = {{0, d1.a, "AAA"}, {1, d2.b, "BBB"}};
    Lockstep s0(seats, 0);
    Lockstep s1(seats, 1);

    Command build = make_cmd(CommandType::StartConstruction, d1.a, 0);
    build.state = d1.state_a;
    build.value = 0;
    CHECK(s0.submit(0, build) == LockstepResult::Ok);
    CHECK(s1.submit_empty(0) == LockstepResult::Ok);
    for (const NetMessage& msg : s0.outgoing()) CHECK(s1.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : s1.outgoing()) CHECK(s0.receive(msg) == LockstepResult::Ok);
    std::vector<Command> ordered;
    CHECK(s0.ready(0, &ordered));
    CHECK_EQ(ordered.size(), size_t{1});

    // Both peers apply tick 0, then one of them diverges.
    for (const Command& c : ordered) d1.game.queue.push(c);
    for (const Command& c : ordered) d2.game.queue.push(c);
    d1.game.tick_once();
    d2.game.tick_once();
    CHECK_EQ(d1.game.world.tick, 1u);
    CHECK(apply_command(d1.game, make_cmd(CommandType::ToggleFuelPriority, d1.a, 0)) ==
          CommandResult::Applied);
    CHECK(world_hash(d1.game) != world_hash(d2.game));

    // Tick 0's list is still collected (the caller has not marked it applied yet).
    const NetMessage peer_hash = s1.hash_message(d2.game);
    CHECK_EQ(peer_hash.tick, 1u);
    CHECK(s0.check_hash(d1.game, peer_hash) == LockstepResult::Desynced);
    const std::string report = s0.desync_report();
    CHECK(report.find("lockstep desync at tick 1") != std::string::npos);
    CHECK(report.find("commands at tick 0") != std::string::npos);
    CHECK(report.find("type=start_construction") != std::string::npos);
    CHECK(report.find("country=" + std::to_string(d1.a.v) + " (VLA)") != std::string::npos);

    // A hash tagged with the applied tick itself resolves the same list without the
    // fallback.
    Lockstep t0(seats, 0);
    CHECK(t0.submit(0, build) == LockstepResult::Ok);
    NetMessage tagged = t0.hash_message(d2.game);  // the diverged-from world
    tagged.tick = 0;
    CHECK(t0.check_hash(d1.game, tagged) == LockstepResult::Desynced);
    CHECK(t0.desync_report().find("commands at tick 0") != std::string::npos);
    CHECK(t0.desync_report().find("type=start_construction") != std::string::npos);
}

HOI_TEST(LOCKSTEP_011_empty_submission_opens_the_barrier) {
    MiniWorld m = make_mini_world();
    const std::vector<PeerSeat> seats = two_seats(m);
    Lockstep a(seats, 0);
    Lockstep b(seats, 1);

    const Tick t = 0;
    const Command only = make_cmd(CommandType::ToggleFuelPriority, m.a, t);
    CHECK(a.submit(t, only) == LockstepResult::Ok);
    // Seat 1 has nothing to say, and says so twice: a seat may submit any number of
    // times and the second empty submission is a no-op.
    CHECK(b.submit_empty(t) == LockstepResult::Ok);
    CHECK(b.submit_empty(t) == LockstepResult::Ok);

    for (const NetMessage& msg : a.outgoing()) CHECK(b.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : b.outgoing()) CHECK(a.receive(msg) == LockstepResult::Ok);

    std::vector<Command> ordered_a;
    std::vector<Command> ordered_b;
    CHECK(a.ready(t, &ordered_a));
    CHECK(b.ready(t, &ordered_b));
    CHECK(same_commands(ordered_a, ordered_b));
    CHECK_EQ(ordered_a.size(), size_t{1});
    CHECK(same_command(ordered_a[0], only));
    for (const Command& c : ordered_a) CHECK(c.type != CommandType::None);
    CHECK_EQ(a.applicable().size(), size_t{1});
    CHECK_EQ(b.applicable().size(), size_t{1});

    // Both peers execute the agreed program and land on the same hash.
    MiniWorld w1 = make_mini_world();
    MiniWorld w2 = make_mini_world();
    for (const Command& c : ordered_a) w1.game.queue.push(c);
    for (const Command& c : ordered_b) w2.game.queue.push(c);
    w1.game.tick_once();
    w2.game.tick_once();
    CHECK_EQ(world_hash(w1.game), world_hash(w2.game));

    // An empty submission never erases real commands already submitted, and never
    // leaks a placeholder into the ordered list.
    const Tick t2 = 1;
    CHECK(a.submit(t2, only) == LockstepResult::Ok);
    CHECK(a.submit_empty(t2) == LockstepResult::Ok);
    CHECK(b.submit_empty(t2) == LockstepResult::Ok);
    for (const NetMessage& msg : a.outgoing()) CHECK(b.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : b.outgoing()) CHECK(a.receive(msg) == LockstepResult::Ok);
    std::vector<Command> ordered2;
    CHECK(a.ready(t2, &ordered2));
    CHECK_EQ(ordered2.size(), size_t{1});
    CHECK(same_command(ordered2[0], only));

    // A session that does not know the seat knows nothing to mark.
    Lockstep stranger({}, 9);
    CHECK(stranger.submit_empty(0) == LockstepResult::NotMySeat);
    CHECK(stranger.submit(0, only) == LockstepResult::Unknown);
    CHECK(!stranger.ready(0, nullptr));
}

HOI_TEST(LOCKSTEP_012_applied_order_and_seat_lifecycle) {
    MiniWorld m = make_mini_world();
    const std::vector<PeerSeat> seats = two_seats(m);

    // A joiner that only receives the host's Applied message can become ready: the
    // host-decided order is adopted as the tick's authoritative list.
    Lockstep joiner(seats, 1);
    const Tick t = 3;
    NetMessage applied;
    applied.kind = NetMessageKind::Applied;
    applied.tick = t;
    applied.commands = {make_cmd(CommandType::ToggleFuelPriority, m.a, t),
                        make_cmd(CommandType::ToggleFuelPriority, m.b, t)};
    CHECK(joiner.receive(applied) == LockstepResult::Ok);
    std::vector<Command> ordered;
    CHECK(joiner.ready(t, &ordered));
    CHECK(same_commands(ordered, applied.commands));
    // A conflicting or re-delivered Applied for a tick that is already satisfied is
    // ignored: this peer's own derivation is authoritative once the barrier is open.
    NetMessage conflicting;
    conflicting.kind = NetMessageKind::Applied;
    conflicting.tick = t;
    conflicting.commands = {make_cmd(CommandType::ToggleFuelPriority, m.b, t)};
    CHECK(joiner.receive(conflicting) == LockstepResult::Ok);
    std::vector<Command> again;
    CHECK(joiner.ready(t, &again));
    CHECK(same_commands(again, applied.commands));
    CHECK_EQ(joiner.applicable().size(), size_t{1});
    joiner.mark_applied(t);
    CHECK(!joiner.ready(t, nullptr));
    CHECK(joiner.applicable().empty());

    // The same guard protects a peer that completed a tick from its own submissions.
    Lockstep h0(seats, 0);
    Lockstep h1(seats, 1);
    const Tick bt = 0;
    const Command c0 = make_cmd(CommandType::ToggleFuelPriority, m.a, bt);
    const Command c1 = make_cmd(CommandType::ToggleFuelPriority, m.b, bt);
    CHECK(h0.submit(bt, c0) == LockstepResult::Ok);
    CHECK(h1.submit(bt, c1) == LockstepResult::Ok);
    for (const NetMessage& msg : h0.outgoing()) CHECK(h1.receive(msg) == LockstepResult::Ok);
    for (const NetMessage& msg : h1.outgoing()) CHECK(h0.receive(msg) == LockstepResult::Ok);
    NetMessage forged;
    forged.kind = NetMessageKind::Applied;
    forged.tick = bt;
    forged.commands = {c1, c0};
    CHECK(h0.receive(forged) == LockstepResult::Ok);
    std::vector<Command> derived;
    CHECK(h0.ready(bt, &derived));
    CHECK(same_commands(derived, std::vector<Command>{c0, c1}));

    // Hello adds a seat deterministically; Bye removes it and its pending commands.
    Lockstep fresh({}, 0);
    CHECK(fresh.seats().empty());
    NetMessage hello;
    hello.kind = NetMessageKind::Hello;
    hello.seat = 2;
    hello.text = "CCC";
    Command placeholder;
    placeholder.type = CommandType::None;
    placeholder.country = CountryId{12};
    hello.commands = {placeholder};
    CHECK(fresh.receive(hello) == LockstepResult::Ok);
    CHECK_EQ(fresh.seats().size(), size_t{1});
    CHECK_EQ(fresh.seats()[0].seat, 2u);
    CHECK_EQ(fresh.seats()[0].tag, "CCC");
    CHECK_EQ(fresh.seats()[0].country.v, 12u);

    // Hello for a known seat updates its tag without reordering anything.
    NetMessage rename;
    rename.kind = NetMessageKind::Hello;
    rename.seat = 2;
    rename.text = "CCC2";
    CHECK(fresh.receive(rename) == LockstepResult::Ok);
    CHECK_EQ(fresh.seats().size(), size_t{1});
    CHECK_EQ(fresh.seats()[0].tag, "CCC2");

    NetMessage bye;
    bye.kind = NetMessageKind::Bye;
    bye.seat = 2;
    CHECK(fresh.receive(bye) == LockstepResult::Ok);
    CHECK(fresh.seats().empty());

    // Duplicate seat ids are collapsed, and the roster is sorted by seat id.
    std::vector<PeerSeat> messy = {{5, m.b, "EEE"}, {1, m.a, "AAA"}, {1, m.a, "AAA"}};
    Lockstep sorted(messy, 5);
    CHECK_EQ(sorted.seats().size(), size_t{2});
    CHECK_EQ(sorted.seats()[0].seat, 1u);
    CHECK_EQ(sorted.seats()[1].seat, 5u);
    CHECK_EQ(sorted.local_country().v, m.b.v);
}