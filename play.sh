#!/usr/bin/env bash
# One-click launcher for Linux and macOS.
#
# Finds the `game` binary next to this file, picks a free TCP port (8080 first,
# then 8081..8089), and runs the browser client in the foreground.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
game="$here/game"

if [ ! -x "$game" ]; then
    echo "game binary not found next to play.sh (expected $game)" >&2
    echo "Build it with:  cmake -S . -B build && cmake --build build -j" >&2
    echo "then copy build/game next to play.sh." >&2
    exit 1
fi

# True when something is already listening on the given TCP port.
port_in_use() {
    local port="$1"
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -qE "[:.]${port}([[:space:]]|$)"
    elif command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1
    elif command -v netstat >/dev/null 2>&1; then
        netstat -an 2>/dev/null | grep -qE "[.:]${port}[[:space:]].*LISTEN"
    else
        # No way to check: assume the port is free and let the game report a bind failure.
        return 1
    fi
}

port=""
for candidate in 8080 8081 8082 8083 8084 8085 8086 8087 8088 8089; do
    if ! port_in_use "$candidate"; then
        port="$candidate"
        break
    fi
done

if [ -z "$port" ]; then
    echo "no free TCP port in the range 8080-8089" >&2
    echo "close whatever is using them and run play.sh again" >&2
    exit 1
fi

echo "starting the game on http://127.0.0.1:$port"
echo "your browser should open by itself; press Ctrl+C to stop the game"

cd "$here" || exit 1
exec "$game" --play --port "$port"
