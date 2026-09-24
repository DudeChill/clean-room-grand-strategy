#include "net/transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

#include "core/log.h"

namespace hoi::net {
namespace {

constexpr size_t kReadChunk = 64 * 1024;
// A peer that stops reading must not hang the session forever: bound both directions.
constexpr int kSendTimeoutSec = 10;
constexpr int kConnectTimeoutMs = 5000;

std::string errno_message(const char* what) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %s", what, std::strerror(errno));
    return buf;
}

bool set_nonblocking(int fd, bool on) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, next) == 0;
}

// Polls one descriptor. Returns 1 ready, 0 timeout, -1 error/closed.
int poll_one(int fd, short events, int timeout_ms) {
    struct pollfd p{};
    p.fd = fd;
    p.events = events;
    for (;;) {
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return rc;
    }
}

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
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    port_ = 0;
}

bool Listener::bind_port(uint16_t port, std::string* err) {
    close();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = errno_message("socket");
        return false;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err) *err = errno_message("bind");
        ::close(fd);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        if (err) *err = errno_message("listen");
        ::close(fd);
        return false;
    }
    if (!set_nonblocking(fd, true)) {
        if (err) *err = errno_message("fcntl(O_NONBLOCK)");
        ::close(fd);
        return false;
    }

    // Report the real port so `--port 0` is usable for tests.
    struct sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &blen) != 0) {
        if (err) *err = errno_message("getsockname");
        ::close(fd);
        return false;
    }
    fd_ = fd;
    port_ = ntohs(bound.sin_port);
    return true;
}

std::unique_ptr<Connection> Listener::accept(int timeout_ms, const volatile bool* cancelled,
                                             std::string* err) {
    if (fd_ < 0) {
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
        const int ready = poll_one(fd_, POLLIN, slice);
        if (ready < 0) {
            if (err) *err = errno_message("poll");
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

    struct sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    const int cfd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&peer), &plen);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (err) *err = "accept interrupted";
            return nullptr;
        }
        if (err) *err = errno_message("accept");
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
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    closed_ = true;
}

std::unique_ptr<Connection> Connection::adopt(int fd, std::string peer) {
    auto conn = std::unique_ptr<Connection>(new Connection());
    conn->fd_ = fd;
    conn->peer_ = std::move(peer);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv{};
    tv.tv_sec = kSendTimeoutSec;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return conn;
}

std::unique_ptr<Connection> Connection::connect(const std::string& host, uint16_t port,
                                               int timeout_ms, std::string* err) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string service = std::to_string(static_cast<unsigned>(port));
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        if (err) *err = std::string("resolve ") + host + ": " + ::gai_strerror(rc);
        if (res != nullptr) ::freeaddrinfo(res);
        return nullptr;
    }

    int fd = -1;
    for (struct addrinfo* it = res; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (!set_nonblocking(fd, true)) {
            ::close(fd);
            fd = -1;
            continue;
        }
        const int c = ::connect(fd, it->ai_addr, it->ai_addrlen);
        if (c == 0) break;
        if (errno == EINPROGRESS) {
            const int ready = poll_one(fd, POLLOUT, timeout_ms > 0 ? timeout_ms : kConnectTimeoutMs);
            if (ready > 0) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) break;
            }
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        if (err) *err = "connect " + host + ":" + service + ": " + std::strerror(errno);
        return nullptr;
    }
    if (!set_nonblocking(fd, false)) {
        if (err) *err = errno_message("fcntl(blocking)");
        ::close(fd);
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
        const ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (err) *err = errno_message("send");
        return -1;
    }
    return static_cast<long>(sent);
}

long Connection::recv_available(std::string* out, std::string* err) {
    if (fd_ < 0) {
        if (err) *err = "connection closed";
        return -1;
    }
    long total = 0;
    char buf[kReadChunk];
    for (;;) {
        const int ready = poll_one(fd_, POLLIN, 0);
        if (ready == 0) break;
        if (ready < 0) {
            if (err) *err = errno_message("poll");
            return total > 0 ? total : -1;
        }
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
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
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (err) *err = errno_message("recv");
        return total > 0 ? total : -1;
    }
    return total;
}

bool Connection::wait_readable(int timeout_ms) {
    if (fd_ < 0) return false;
    const int ready = poll_one(fd_, POLLIN, timeout_ms);
    return ready > 0;
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