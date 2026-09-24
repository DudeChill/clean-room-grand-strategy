// Transport tests: real localhost sockets, in-memory pipes, and the framing rules the
// session loop depends on (a partial record is buffered, a broken one is an error).

#include <memory>
#include <string>
#include <vector>

#include "net/lockstep.h"
#include "net/transport.h"
#include "test.h"

using namespace hoi;
using namespace hoi::net;

namespace {

Command make_command(uint32_t country, Tick tick) {
    Command c;
    c.type = CommandType::SetProductionLine;
    c.country = CountryId(country);
    c.issued_tick = tick;
    c.state = StateId(11);
    c.equipment = EquipmentId(4);
    c.target_country = CountryId(9);
    c.value = 3;
    c.value_f = 0.25;
    c.text = "infantry_equipment";
    c.divisions.push_back(DivisionId(1));
    c.divisions.push_back(DivisionId(2));
    return c;
}

NetMessage make_hello() {
    NetMessage hello;
    hello.kind = NetMessageKind::Hello;
    hello.seat = 1;
    hello.tick = 42;
    hello.text = "KOR";
    return hello;
}

NetMessage make_commands(Tick tick) {
    NetMessage msg;
    msg.kind = NetMessageKind::Commands;
    msg.seat = 1;
    msg.tick = tick;
    msg.commands.push_back(make_command(3, tick));
    msg.commands.push_back(make_command(4, tick));
    return msg;
}

void check_commands_equal(const NetMessage& a, const NetMessage& b) {
    CHECK_EQ(a.kind, b.kind);
    CHECK_EQ(a.seat, b.seat);
    CHECK_EQ(a.tick, b.tick);
    CHECK_EQ(a.commands.size(), b.commands.size());
    for (size_t i = 0; i < a.commands.size(); ++i) {
        const Command& x = a.commands[i];
        const Command& y = b.commands[i];
        CHECK_EQ(static_cast<int>(x.type), static_cast<int>(y.type));
        CHECK_EQ(x.country.raw(), y.country.raw());
        CHECK_EQ(x.issued_tick, y.issued_tick);
        CHECK_EQ(x.state.raw(), y.state.raw());
        CHECK_EQ(x.equipment.raw(), y.equipment.raw());
        CHECK_EQ(x.target_country.raw(), y.target_country.raw());
        CHECK_EQ(x.value, y.value);
        CHECK_EQ(x.value_f, y.value_f);
        CHECK_EQ(x.text, y.text);
        CHECK_EQ(x.divisions.size(), y.divisions.size());
    }
}

}  // namespace

// A real socket round trip on localhost: a peer that never connects is a timeout (not a
// hang), a real one carries two framed messages intact, and its close is visible.
HOI_TEST(transport_localhost_round_trip) {
    std::string err;

    Listener listener;
    CHECK(listener.bind_port(0, &err));
    CHECK(listener.listening());
    CHECK(listener.port() != 0);
    const uint16_t port = listener.port();

    // Nobody is connecting: accept must return on the poll timeout, not block.
    CHECK(listener.accept(20, nullptr, &err) == nullptr);
    CHECK(!err.empty());

    auto client = Connection::connect("127.0.0.1", port, 2000, &err);
    CHECK(client != nullptr);
    CHECK(client->open());

    auto server = listener.accept(2000, nullptr, &err);
    CHECK(server != nullptr);
    CHECK(server->open());

    // A connect to a port nobody listens on must fail, not hang. Reuse a port we just
    // bound and closed so it is one nothing can be serving.
    Listener closed_port;
    CHECK(closed_port.bind_port(0, &err));
    const uint16_t dead_port = closed_port.port();
    closed_port.close();
    CHECK(Connection::connect("127.0.0.1", dead_port, 500, &err) == nullptr);

    Channel outbound(client.get());
    Channel inbound(server.get());

    const NetMessage hello = make_hello();
    const NetMessage cmds = make_commands(7);
    CHECK(outbound.send(hello, &err));
    CHECK(outbound.send(cmds, &err));

    std::vector<NetMessage> got;
    for (int i = 0; i < 20 && got.size() < 2; ++i) {
        const LinkStatus s = inbound.receive(&got, 500, &err);
        CHECK(s != LinkStatus::Malformed);
        if (s == LinkStatus::Closed) break;
    }
    CHECK_EQ(got.size(), 2u);
    CHECK_EQ(got[0].kind, NetMessageKind::Hello);
    CHECK_EQ(got[0].seat, 1u);
    CHECK_EQ(got[0].tick, 42ull);
    CHECK_EQ(got[0].text, std::string("KOR"));
    check_commands_equal(got[1], cmds);
    CHECK_EQ(inbound.buffered(), 0u);
    CHECK_EQ(inbound.messages_received(), 2u);

    // The peer's exit is reported as Closed, so a session loop cannot wait forever.
    client->close();
    LinkStatus last = LinkStatus::Idle;
    for (int i = 0; i < 20 && last != LinkStatus::Closed; ++i) {
        std::vector<NetMessage> tail;
        last = inbound.receive(&tail, 500, &err);
        CHECK(tail.empty());
    }
    CHECK(last == LinkStatus::Closed);
    listener.close();
}

// The same framing over an in-memory pipe, so session logic has a network-free path.
HOI_TEST(transport_memory_link_round_trip) {
    std::unique_ptr<MemoryLink> a;
    std::unique_ptr<MemoryLink> b;
    MemoryLink::make_pair(&a, &b);

    Channel from_a(a.get());
    Channel to_b(b.get());

    std::string err;
    const NetMessage cmds = make_commands(24);
    CHECK(from_a.send(cmds, &err));
    CHECK_EQ(to_b.buffered(), 0u);

    std::vector<NetMessage> got;
    CHECK(to_b.receive(&got, 0, &err) == LinkStatus::Ok);
    CHECK_EQ(got.size(), 1u);
    check_commands_equal(got[0], cmds);

    // Nothing else is pending and the pipe is nondeterministic-by-design: Idle, never a
    // fabricated message.
    std::vector<NetMessage> none;
    CHECK(to_b.receive(&none, 0, &err) == LinkStatus::Idle);
    CHECK(none.empty());

    // Closing one end is visible at the other.
    a->close();
    std::vector<NetMessage> tail;
    CHECK(to_b.receive(&tail, 0, &err) == LinkStatus::Closed);
}

// A record split across two reads stays buffered until the rest of its bytes arrive.
HOI_TEST(transport_truncated_record_is_buffered) {
    std::unique_ptr<MemoryLink> a;
    std::unique_ptr<MemoryLink> b;
    MemoryLink::make_pair(&a, &b);
    Channel reader(b.get());

    const NetMessage hello = make_hello();
    const std::string frame = encode_message(hello);
    CHECK_GT(frame.size(), 2u);
    const size_t split = frame.size() - 1;

    b->feed(frame.substr(0, split));
    std::vector<NetMessage> got;
    std::string err;
    CHECK(reader.receive(&got, 0, &err) == LinkStatus::Idle);
    CHECK(got.empty());
    CHECK_EQ(reader.buffered(), split);

    b->feed(frame.substr(split));
    CHECK(reader.receive(&got, 0, &err) == LinkStatus::Ok);
    CHECK_EQ(got.size(), 1u);
    CHECK_EQ(got[0].kind, NetMessageKind::Hello);
    CHECK_EQ(got[0].text, std::string("KOR"));
    CHECK_EQ(reader.buffered(), 0u);
    CHECK_EQ(reader.messages_received(), 1u);

    // Frame header split from its payload behaves the same way.
    std::vector<NetMessage> more;
    b->feed(frame.substr(0, 4));
    CHECK(reader.receive(&more, 0, &err) == LinkStatus::Idle);
    CHECK(more.empty());
    b->feed(frame.substr(4));
    CHECK(reader.receive(&more, 0, &err) == LinkStatus::Ok);
    CHECK_EQ(more.size(), 1u);
}

// Several records in one read are all decoded; a broken record is a hard error.
HOI_TEST(transport_coalesced_and_malformed_records) {
    std::unique_ptr<MemoryLink> a;
    std::unique_ptr<MemoryLink> b;
    MemoryLink::make_pair(&a, &b);
    Channel reader(b.get());

    const NetMessage hello = make_hello();
    const NetMessage cmds = make_commands(9);
    b->feed(encode_message(hello) + encode_message(cmds));

    std::vector<NetMessage> got;
    std::string err;
    CHECK(reader.receive(&got, 0, &err) == LinkStatus::Ok);
    CHECK_EQ(got.size(), 2u);
    CHECK_EQ(got[0].kind, NetMessageKind::Hello);
    check_commands_equal(got[1], cmds);
    CHECK_EQ(reader.buffered(), 0u);

    // A one-byte payload whose kind byte is out of range is malformed framing, not a
    // message to guess at.
    b->feed(std::string("\x01\x00\x00\x00\x09", 5));
    std::vector<NetMessage> bad;
    CHECK(reader.receive(&bad, 0, &err) == LinkStatus::Malformed);
    CHECK(bad.empty());
    CHECK(!err.empty());
}