// Multiplayer transport: POSIX TCP sockets plus a framing layer, and nothing else.
//
// The transport moves *messages* (`lockstep.h`), never simulation state: a peer encodes
// a `NetMessage` with `encode_message`, the bytes go over a `Link`, and the receiving
// end buffers bytes until `decode_message` yields a whole message. TCP is a byte stream,
// so the buffering is where "one message" is decided - a `Channel` owns that buffer and
// a truncated read is simply a message that has not arrived yet.
//
// Three pieces, smallest first:
//  * `Link`     - an abstract, byte-oriented, unreliable-boundary channel. The shipped
//                 implementation is `Connection` (a socket); tests substitute
//                 `MemoryLink`, an in-memory pipe, so the framing and the session loop
//                 are testable without a network.
//  * `Channel`  - framing over a `Link`: send one message, receive every complete
//                 message that has arrived, keep the remainder buffered.
//  * `Listener` - a bound listening socket that accepts one `Connection` with a poll
//                 timeout, so a session can also notice "nobody joined" and exit.
//
// Deliberately out of scope (see docs/MULTIPLAYER.md): NAT traversal, matchmaking,
// reconnects, spectators, encryption. This is a LAN/localhost transport for a
// deterministic lockstep session, and a wrong-byte stream is a hard error, never a
// silently ignored one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/lockstep.h"

namespace hoi::net {

// Outcome of a framed receive. `Ok` means at least one message was decoded; `Idle`
// means nothing arrived within the wait (or the link has bytes but no whole message);
// `Closed` means the peer is gone; `Malformed` means the byte stream itself is broken
// (a bad frame - never a partial one, which stays buffered).
enum class LinkStatus : uint8_t {
    Ok = 0,
    Idle,
    Closed,
    Malformed,
};

const char* link_status_name(LinkStatus s);

// A byte channel. Implementations move opaque bytes; framing lives in `Channel`.
// The interface is intentionally tiny so a test can substitute an in-memory pipe.
class Link {
  public:
    virtual ~Link() = default;

    // Writes all `len` bytes. Returns `len` on success, or -1 on error (with `*err` set
    // when non-null). A short write is a failure, never a partial success.
    virtual long send_all(const char* data, size_t len, std::string* err) = 0;

    // Appends whatever is currently readable to `out` without blocking. Returns the
    // number of bytes appended (0 = nothing available right now), or -1 when the link is
    // closed or failed.
    virtual long recv_available(std::string* out, std::string* err) = 0;

    // Waits up to `timeout_ms` for the link to become readable. False on timeout, on a
    // closed link, or when the implementation does not block (in-memory pipes).
    virtual bool wait_readable(int timeout_ms) = 0;

    [[nodiscard]] virtual bool open() const = 0;
    virtual void close() = 0;
};

// Framing over a `Link`: one `NetMessage` per record, with the leftover bytes of a
// partial record retained in `buffer_` until the rest arrives.
class Channel {
  public:
    explicit Channel(Link* link) : link_(link) {}

    // Encodes and sends one message. False (with `*err` set) when the link failed.
    bool send(const NetMessage& msg, std::string* err);

    // Decodes every complete message that has arrived, appending to `out` in arrival
    // order. Waits up to `timeout_ms` for the first byte when the buffer holds no whole
    // message. Returns `Ok` when at least one message was decoded.
    LinkStatus receive(std::vector<NetMessage>* out, int timeout_ms, std::string* err);

    [[nodiscard]] size_t buffered() const { return buffer_.size(); }
    [[nodiscard]] uint64_t messages_received() const { return received_; }
    [[nodiscard]] bool open() const { return link_ != nullptr && link_->open(); }
    Link* link() const { return link_; }

  private:
    Link* link_ = nullptr;
    std::string buffer_;
    uint64_t received_ = 0;
};

class Connection;

// A bound TCP listening socket. `bind_port` takes port 0 for an ephemeral port and
// reports the actual port back through `port()`.
class Listener {
  public:
    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Binds `port` on all local interfaces. False + `*err` on failure.
    bool bind_port(uint16_t port, std::string* err);

    // Waits up to `timeout_ms` (negative = forever) for a connection. Returns nullptr on
    // timeout or when `cancelled` becomes true, so a session loop can shut down.
    std::unique_ptr<Connection> accept(int timeout_ms, const volatile bool* cancelled,
                                       std::string* err);

    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] bool listening() const { return fd_ >= 0; }
    void close();

  private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

// A connected TCP socket. Reads are poll-gated so `recv_available` never blocks.
class Connection final : public Link {
  public:
    Connection() = default;
    ~Connection() override;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Connects to `host:port` (host is an IPv4 literal or a name). False + `*err` on
    // failure; `timeout_ms` bounds the connect so a dead address cannot hang startup.
    static std::unique_ptr<Connection> connect(const std::string& host, uint16_t port,
                                              int timeout_ms, std::string* err);

    // Takes ownership of an accepted descriptor (used by `Listener::accept`).
    static std::unique_ptr<Connection> adopt(int fd, std::string peer);

    long send_all(const char* data, size_t len, std::string* err) override;
    long recv_available(std::string* out, std::string* err) override;
    bool wait_readable(int timeout_ms) override;
    [[nodiscard]] bool open() const override { return fd_ >= 0 && !closed_; }
    void close() override;

    [[nodiscard]] const std::string& peer() const { return peer_; }

  private:
    int fd_ = -1;
    bool closed_ = false;
    std::string peer_;
};

// In-memory `Link` pair for tests: bytes sent by one end are readable at the other.
// `feed` injects raw bytes as if they had arrived from the peer, so a test can split a
// frame across two reads (the truncation case) without touching a socket.
class MemoryLink final : public Link {
  public:
    MemoryLink() = default;

    // Creates two connected ends; `*a` and `*b` each read what the other sends.
    static void make_pair(std::unique_ptr<MemoryLink>* a, std::unique_ptr<MemoryLink>* b);

    // Delivers `bytes` to this end's inbox as if the peer had sent them.
    void feed(const std::string& bytes) { inbox_.append(bytes); }

    long send_all(const char* data, size_t len, std::string* err) override;
    long recv_available(std::string* out, std::string* err) override;
    bool wait_readable(int timeout_ms) override;
    [[nodiscard]] bool open() const override { return !closed_; }
    void close() override { closed_ = true; }

    [[nodiscard]] bool peer_closed() const { return peer_ != nullptr && peer_->closed_; }

  private:
    std::string inbox_;
    MemoryLink* peer_ = nullptr;
    bool closed_ = false;
};

}  // namespace hoi::net