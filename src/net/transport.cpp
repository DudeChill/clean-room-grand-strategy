#include "net/transport.h"

#include "core/socket_compat.h"

#include <cstdio>

namespace hoi::net {
namespace {

constexpr size_t kReadChunk = 64 * 1024;
// A peer that stops reading must not hang the session forever: bound both directions.
constexpr int kSendTimeoutSec = 10;
constexpr int kConnectTimeoutMs = 5000;

// "<what>: <reason>", with the reason from whichever error source this platform reports
// socket failures through (errno on POSIX, WSAGetLastError on Windows).
std::string socket_message(const char* what) {
    return std::string(what) + ": " + socket_error_text(last_socket_error());
}

// Byte count for a single send/recv call: Winsock takes an `int` where POSIX takes a
// size_t, so one call never asks for more than a chunk both accept.
int io_chunk(size_t n) { return static_cast<int>(n < kReadChunk ? n : kReadChunk); }

}  // namespace

const char* link_status_name(LinkStatus s) {
    switch (s) {
        case LinkStatus::Ok: return "ok";
        case LinkStatus::Idle: return "idle";
        case LinkStatus::Closed: return "closed";
        case LinkStatus::Malformed: return "malformed";
    }
    return "?";
}

// ---------------------------------------------------------------- Channel ----

bool Channel::send(const NetMessage& msg, std::string* err) {
    if (link_ == nullptr) {
        if (err) *err = "no link";
        return false;
    }
    const std::string bytes = encode_message(msg);
    const long written = link_->send_all(bytes.data(), bytes.size(), err);
    if (written < 0 || static_cast<size_t>(written) != bytes.size()) {
        if (err != nullptr && err->empty()) *err = "short send";
        return false;
    }
    return true;
}

LinkStatus Channel::receive(std::vector<NetMessage>* out, int timeout_ms, std::string* err) {
    if (link_ == nullptr) {
        if (err) *err = "no link";
        return LinkStatus::Closed;
    }

    // Decodes every whole record the buffer holds. False = malformed framing.
    auto decode_all = [&]() {
        for (;;) {
            NetMessage msg;
            bool ok = false;
            std::string derr;
            const size_t used = decode_message(buffer_, &msg, &ok, &derr);
            if (used == 0) {
                if (!ok) {
                    if (err) *err = derr.empty() ? "malformed net message framing" : derr;
                    return false;
                }
                return true;  // incomplete record: keep buffering it
            }
            buffer_.erase(0, used);
            ++received_;
            out->push_back(std::move(msg));
        }
    };

    // Never block while the buffer already holds a whole message.
    long got = link_->recv_available(&buffer_, err);
    if (got < 0) return LinkStatus::Closed;
    if (!decode_all()) return LinkStatus::Malformed;
    if (!out->empty()) return LinkStatus::Ok;

    if (got == 0 && timeout_ms > 0) {
        if (!link_->wait_readable(timeout_ms)) return LinkStatus::Idle;
        got = link_->recv_available(&buffer_, err);
        if (got < 0) return LinkStatus::Closed;
        if (!decode_all()) return LinkStatus::Malformed;
        if (!out->empty()) return LinkStatus::Ok;
    }
    return LinkStatus::Idle;
}

// --------------------------------------------------------------- Listener ----

Listener::~Listener() { close(); }

void Listener::close() {
    close_socket(&fd_);
    port_ = 0;
}

bool Listener::bind_port(uint16_t port, std::string* err) {
    close();
    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        if (err) *err = socket_message("socket");
        return false;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 static_cast<socklen_compat>(sizeof(one)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr),
               static_cast<socklen_compat>(sizeof(addr))) != 0) {
        if (err) *err = socket_message("bind");
        close_socket(fd);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        if (err) *err = socket_message("listen");
        close_socket(fd);
        return false;
    }
    if (!set_nonblocking(fd, true)) {
        if (err) *err = socket_message("set_nonblocking");
        close_socket(fd);
        return false;
    }

    // Report the real port so `--port 0` is usable for tests.
    sockaddr_in bound{};
    socklen_compat blen = static_cast<socklen_compat>(sizeof(bound));
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        if (err) *err = socket_message("getsockname");
        close_socket(fd);
        return false;
    }
    fd_ = fd;
    port_ = ntohs(bound.sin_port);
    return true;
}

std::unique_ptr<Connection> Listener::accept(int timeout_ms, const volatile bool* cancelled,
                                             std::string* err) {
    if (fd_ == kInvalidSocket) {
        if (err) *err = "listener is not bound";
        return nullptr;
    }
    // Walk the wait in short slices so a SIGINT can end a long join wait.
    const int slice = timeout_ms < 0 ? 200 : (timeout_ms > 200 ? 200 : timeout_ms);
    int remaining = timeout_ms;
    for (;;) {
        if (cancelled != nullptr && *cancelled) {
            if (err) *err = "cancelled";
            return nullptr;
        }
        const int ready = poll_readable(fd_, slice);
        if (ready < 0) {
            if (err) *err = socket_message("poll");
            return nullptr;
        }
        if (ready > 0) break;
        if (remaining >= 0) {
            remaining -= slice;
            if (remaining <= 0) {
                if (err) *err = "timed out waiting for a peer";
                return nullptr;
            }
        }
    }

    sockaddr_in peer{};
    socklen_compat plen = static_cast<socklen_compat>(sizeof(peer));
    const socket_t cfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (cfd == kInvalidSocket) {
        const int code = last_socket_error();
        if (would_block(code) || io_interrupted()) {
            if (err) *err = "accept interrupted";
            return nullptr;
        }
        if (err) *err = "accept: " + socket_error_text(code);
        return nullptr;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s:%u", ::inet_ntoa(peer.sin_addr),
                  static_cast<unsigned>(ntohs(peer.sin_port)));
    return Connection::adopt(cfd, name);
}

// ------------------------------------------------------------- Connection ----

Connection::~Connection() { close(); }

void Connection::close() {
    if (fd_ != kInvalidSocket) {
        ::shutdown(fd_, kShutdownBoth);
        close_socket(&fd_);
    }
    closed_ = true;
}

std::unique_ptr<Connection> Connection::adopt(socket_t fd, std::string peer) {
    auto conn = std::unique_ptr<Connection>(new Connection());
    conn->fd_ = fd;
    conn->peer_ = std::move(peer);
    // An accepted socket inherits the listener's non-blocking mode on Windows but not on
    // POSIX, so the transport's blocking semantics are stated here rather than assumed.
    set_nonblocking(fd, false);
    set_nodelay(fd);
    set_send_timeout(fd, kSendTimeoutSec * 1000);
    return conn;
}

std::unique_ptr<Connection> Connection::connect(const std::string& host, uint16_t port,
                                               int timeout_ms, std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(static_cast<unsigned>(port));
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        if (err) *err = std::string("resolve ") + host + ": " + ::gai_strerror(rc);
        if (res != nullptr) ::freeaddrinfo(res);
        return nullptr;
    }

    socket_t fd = kInvalidSocket;
    std::string why;  // why the last attempt failed, for the caller's message
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            why = socket_error_text(last_socket_error());
            continue;
        }
        if (!set_nonblocking(fd, true)) {
            why = socket_error_text(last_socket_error());
            close_socket(&fd);
            continue;
        }
        const int c =
            ::connect(fd, it->ai_addr, static_cast<socklen_compat>(it->ai_addrlen));
        if (c == 0) break;  // connected at once (localhost usually does)
        // Otherwise the attempt is still running or has already failed. Either way the
        // socket becomes writable and SO_ERROR carries the outcome, so the wait needs no
        // knowledge of EINPROGRESS/WSAEWOULDBLOCK.
        const int wait_ms = timeout_ms > 0 ? timeout_ms : kConnectTimeoutMs;
        if (poll_writable(fd, wait_ms) > 0) {
            const int pending = socket_pending_error(fd);
            if (pending == 0) break;
            why = socket_error_text(pending);
        } else {
            why = "timed out";
        }
        close_socket(&fd);
    }
    ::freeaddrinfo(res);
    if (fd == kInvalidSocket) {
        if (err) {
            *err = "connect " + host + ":" + service + ": " + (why.empty() ? "no address" : why);
        }
        return nullptr;
    }
    if (!set_nonblocking(fd, false)) {
        if (err) *err = socket_message("set_nonblocking");
        close_socket(&fd);
        return nullptr;
    }
    return adopt(fd, host + ":" + service);
}

long Connection::send_all(const char* data, size_t len, std::string* err) {
    if (!open()) {
        if (err) *err = "connection closed";
        return -1;
    }
    size_t sent = 0;
    while (sent < len) {
        const io_size_t n = ::send(fd_, data + sent, io_chunk(len - sent), kSendFlags);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && io_interrupted()) continue;
        if (err) *err = socket_message("send");
        return -1;
    }
    return static_cast<long>(sent);
}

long Connection::recv_available(std::string* out, std::string* err) {
    if (fd_ == kInvalidSocket) {
        if (err) *err = "connection closed";
        return -1;
    }
    long total = 0;
    char buf[kReadChunk];
    for (;;) {
        const int ready = poll_readable(fd_, 0);
        if (ready == 0) break;
        if (ready < 0) {
            if (err) *err = socket_message("poll");
            return total > 0 ? total : -1;
        }
        const io_size_t n = ::recv(fd_, buf, io_chunk(sizeof(buf)), 0);
        if (n > 0) {
            out->append(buf, static_cast<size_t>(n));
            total += static_cast<long>(n);
            continue;
        }
        if (n == 0) {
            closed_ = true;
            if (total > 0) return total;  // deliver what we got; the next call reports EOF
            if (err) *err = "peer closed the connection";
            return -1;
        }
        if (io_interrupted()) continue;
        if (io_would_block(n)) break;
        if (err) *err = socket_message("recv");
        return total > 0 ? total : -1;
    }
    return total;
}

bool Connection::wait_readable(int timeout_ms) {
    if (fd_ == kInvalidSocket) return false;
    return poll_readable(fd_, timeout_ms) > 0;
}

// ------------------------------------------------------------- MemoryLink ----

void MemoryLink::make_pair(std::unique_ptr<MemoryLink>* a, std::unique_ptr<MemoryLink>* b) {
    std::unique_ptr<MemoryLink> first(new MemoryLink());
    std::unique_ptr<MemoryLink> second(new MemoryLink());
    first->peer_ = second.get();
    second->peer_ = first.get();
    *a = std::move(first);
    *b = std::move(second);
}

long MemoryLink::send_all(const char* data, size_t len, std::string* err) {
    if (closed_) {
        if (err) *err = "link closed";
        return -1;
    }
    if (peer_ == nullptr || peer_->closed_) {
        if (err) *err = "peer closed";
        return -1;
    }
    peer_->inbox_.append(data, len);
    return static_cast<long>(len);
}

long MemoryLink::recv_available(std::string* out, std::string* err) {
    if (!inbox_.empty()) {
        const long n = static_cast<long>(inbox_.size());
        out->append(inbox_);
        inbox_.clear();
        return n;
    }
    if (peer_closed()) {
        if (err) *err = "peer closed";
        return -1;
    }
    return 0;
}

bool MemoryLink::wait_readable(int timeout_ms) {
    (void)timeout_ms;  // in-memory: state never changes while we are not reading
    return !inbox_.empty();
}

}  // namespace hoi::net
