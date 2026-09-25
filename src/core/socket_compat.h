// Portable TCP sockets: POSIX on Unix, Winsock2 on Windows.
//
// The engine uses sockets in exactly two places (the browser client's HTTP server and the
// multiplayer transport), and both need the same handful of calls. This header is that
// handful, so neither file carries platform `#ifdef`s of its own.
//
// Rules:
//  * `socket_layer_init` must be called once per process before the first socket is
//    created. It is a no-op on POSIX and starts Winsock on Windows; calling it twice is
//    safe.
//  * Errors go through `last_socket_error` / `socket_error_text`, because Windows reports
//    them through `WSAGetLastError` and not `errno`.
//  * `kSendFlags` carries `MSG_NOSIGNAL` on POSIX (a peer that has gone away must not kill
//    the process with SIGPIPE) and 0 on Windows, where the situation cannot arise.
//  * Sizes are `io_size_t`, not `ssize_t`: MSVC has no `ssize_t`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>  // getaddrinfo/freeaddrinfo/gai_strerror (ws2tcpip.h on Windows)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hoi {
namespace net {

// Result of a socket read/write: a byte count, or a negative value for "failed" or
// "would block" (the caller distinguishes them with `last_socket_error`).
using io_size_t = std::ptrdiff_t;

#ifdef _WIN32

using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline constexpr int kSendFlags = 0;  // no SIGPIPE on Windows
inline constexpr int kShutdownBoth = SD_BOTH;  // Winsock's spelling of SHUT_RDWR
inline constexpr bool kWindowsSockets = true;

inline bool socket_layer_init(std::string* err) {
    static bool started = false;
    static int last_code = 0;
    if (started) return true;
    WSADATA data;
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        last_code = rc;
        if (err) {
            *err = "WSAStartup failed with code " + std::to_string(rc);
        }
        return false;
    }
    started = true;
    return true;
}

inline int last_socket_error() { return ::WSAGetLastError(); }

inline bool would_block(int code) { return code == WSAEWOULDBLOCK; }

inline std::string socket_error_text(int code) {
    switch (code) {
        case WSAEWOULDBLOCK: return "would block";
        case WSAECONNRESET: return "connection reset by peer";
        case WSAECONNREFUSED: return "connection refused";
        case WSAEADDRINUSE: return "address already in use";
        case WSAENOTCONN: return "not connected";
        case WSAETIMEDOUT: return "timed out";
        default: break;
    }
    return "socket error " + std::to_string(code);
}

inline void close_socket(socket_t s) {
    if (s != INVALID_SOCKET) ::closesocket(s);
}

inline bool set_nonblocking(socket_t s, bool on) {
    u_long mode = on ? 1UL : 0UL;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}

inline bool set_nodelay(socket_t s) {
    BOOL flag = TRUE;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&flag), sizeof(flag)) == 0;
}

// Send timeout in milliseconds. Winsock wants a DWORD of milliseconds, POSIX a timeval,
// so the difference stays here instead of leaking into every caller.
inline bool set_send_timeout(socket_t s, int ms) {
    const DWORD value = static_cast<DWORD>(ms);
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

inline bool io_interrupted() { return ::WSAGetLastError() == WSAEINTR; }

// MSVC has no socklen_t; Winsock's own calls take an `int*`.
using socklen_compat = int;

#else  // POSIX

using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline constexpr int kSendFlags = MSG_NOSIGNAL;
inline constexpr int kShutdownBoth = SHUT_RDWR;
inline constexpr bool kWindowsSockets = false;

inline bool socket_layer_init(std::string*) { return true; }

inline int last_socket_error() { return errno; }

inline bool would_block(int code) { return code == EAGAIN || code == EWOULDBLOCK; }

inline std::string socket_error_text(int code) { return std::strerror(code); }

inline void close_socket(socket_t s) {
    if (s >= 0) ::close(s);
}

inline bool set_nonblocking(socket_t s, bool on) {
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, next) == 0;
}

inline bool set_nodelay(socket_t s) {
    int flag = 1;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

inline bool set_send_timeout(socket_t s, int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

inline bool io_interrupted() { return errno == EINTR; }

using socklen_compat = socklen_t;

#endif

// Close a socket and mark the handle invalid, so a double close cannot happen.
inline void close_socket(socket_t* s) {
    if (s == nullptr) return;
    close_socket(*s);
    *s = kInvalidSocket;
}

// Wait until a socket is readable or the timeout expires. Returns 1 readable, 0 timeout,
// -1 error. Implemented with `select` on both platforms: Windows' `WSAPoll` has different
// semantics from POSIX `poll`, and this one call is not worth the difference.
inline int poll_readable(socket_t s, int timeout_ms) {
    // A signal arriving mid-wait is not a socket failure: retry until the caller's
    // timeout budget runs out, so a Ctrl-C handler elsewhere cannot turn a wait into an
    // error return.
    int remaining = timeout_ms;
    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        timeval tv;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;
        const int ready = ::select(static_cast<int>(s) + 1, &readfds, nullptr, nullptr,
                                   timeout_ms < 0 ? nullptr : &tv);
        if (ready >= 0) return ready == 0 ? 0 : 1;
        if (!io_interrupted()) return -1;
        if (timeout_ms < 0) continue;
        if (remaining <= 0) return 0;
        remaining = remaining > 10 ? remaining - 10 : 0;
    }
}

// The same wait on the write set, for a non-blocking connect: a socket becomes writable
// once the connection attempt has resolved (success or failure - check SO_ERROR next).
inline int poll_writable(socket_t s, int timeout_ms) {
    int remaining = timeout_ms;
    for (;;) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(s, &writefds);
        timeval tv;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;
        const int ready = ::select(static_cast<int>(s) + 1, nullptr, &writefds, nullptr,
                                   timeout_ms < 0 ? nullptr : &tv);
        if (ready >= 0) return ready == 0 ? 0 : 1;
        if (!io_interrupted()) return -1;
        if (timeout_ms < 0) continue;
        if (remaining <= 0) return 0;
        remaining = remaining > 10 ? remaining - 10 : 0;
    }
}

// The pending error of a connected (or failed) non-blocking socket, 0 when there is none.
inline int socket_pending_error(socket_t s) {
    int value = 0;
    socklen_compat len = static_cast<socklen_compat>(sizeof(value));
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &len) != 0) {
        return last_socket_error();
    }
    return value;
}

// True when a read/write result means "nothing right now", as opposed to an error.
inline bool io_would_block(io_size_t n) {
    return n < 0 && would_block(last_socket_error());
}

}  // namespace net
}  // namespace hoi