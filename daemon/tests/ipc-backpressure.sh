#!/bin/sh
# A client that sends requests and never reads the replies must not make the
# daemon grow without bound. Starts a private daemon under temporary XDG
# directories, floods it from a non-reading client, and fails if resident
# memory climbs past a fixed ceiling or the daemon dies.
#
# Usage: daemon/tests/ipc-backpressure.sh [path/to/omacalld]
set -eu

daemon=${1:-"$(dirname "$0")/../omacalld"}
requests=${REQUESTS:-60000}
ceiling_kb=${CEILING_KB:-40000}

tmp=$(mktemp -d)
trap 'kill "$pid" 2>/dev/null; rm -rf "$tmp"' EXIT

export XDG_RUNTIME_DIR="$tmp/run" XDG_CONFIG_HOME="$tmp/config" \
       XDG_DATA_HOME="$tmp/data" XDG_STATE_HOME="$tmp/state"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/omacall"

# No account, and an ephemeral SIP port: the test must not fight a real
# omacalld on this machine for 5060, and it never registers anywhere.
printf '{"accounts":[{"name":"test","port":0}],"active_account":"test"}\n' \
    >"$XDG_CONFIG_HOME/omacall/config.json"

"$daemon" >"$tmp/daemon.log" 2>&1 &
pid=$!
for _ in $(seq 1 50); do
    [ -S "$XDG_RUNTIME_DIR/omacall.sock" ] && break
    sleep 0.1
done
[ -S "$XDG_RUNTIME_DIR/omacall.sock" ] || { echo "daemon did not start"; cat "$tmp/daemon.log"; exit 1; }

rss() { awk '/VmRSS/ { print $2 }' "/proc/$pid/status" 2>/dev/null || echo dead; }
before=$(rss)

# -u: write only, never read. hello is the largest reply the daemon produces.
seq 1 "$requests" | sed 's/.*/{"id":&,"cmd":"hello"}/' \
    | socat -u -T 20 - "UNIX-CONNECT:$XDG_RUNTIME_DIR/omacall.sock" 2>/dev/null &
flood=$!

peak=$before
for _ in $(seq 1 40); do
    sleep 0.25
    now=$(rss)
    [ "$now" = dead ] && { echo "FAIL: daemon died during the flood"; exit 1; }
    [ "$now" -gt "$peak" ] && peak=$now
done
kill "$flood" 2>/dev/null || true; wait "$flood" 2>/dev/null || true

if ! kill -0 "$pid" 2>/dev/null; then
    echo "FAIL: daemon died after the flood"; exit 1
fi
if [ "$peak" -gt "$ceiling_kb" ]; then
    echo "FAIL: RSS peaked at ${peak} kB (ceiling ${ceiling_kb} kB, started at ${before} kB)"
    exit 1
fi
dropped=$(grep -c "not reading its replies" "$tmp/daemon.log" || true)
echo "ok: RSS ${before} -> peak ${peak} kB over ${requests} unread requests; ${dropped} client(s) dropped"
