#!/usr/bin/env bash
# perf-test.sh — end-to-end perf test using our iperf3 client on the OS4 guest
# vs a real `iperf3 -s` server on the host.
#
# Requires:
#   * build/iperf3 (run ./scripts/build.sh first)
#   * `amiga_mcp` devbench running on localhost:3000
#   * `iperf3` on host (macOS: brew install iperf3; Linux: apt install iperf3)
#
# The very first run after a fresh QEMU boot is the meaningful number.
# See docs/PERF-TESTING.md for why (Roadshow's TCP control-block pool
# degrades after ~3 successive tests).
#
# Usage:
#   ./scripts/perf-test.sh                  # 10 second test, 64K blocks
#   ./scripts/perf-test.sh 30               # 30 second test
#   ./scripts/perf-test.sh 10 131072        # 10s test, 128K blocks
#   MODE=--raw ./scripts/perf-test.sh       # skip iperf3 protocol, use pyperf-style server
#   MODE='--raw --direct' ./scripts/perf-test.sh   # bypass newlib libc socket wrapper

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
DURATION="${1:-10}"
BLKSIZE="${2:-65536}"
PORT="${PORT:-17999}"
TARGET="${TARGET:-192.168.100.2}"
API="${API:-http://localhost:3000}"
MODE="${MODE:-}"      # empty = full iperf3 protocol; set to --raw for dumb TCP blast
CLIENT_BIN="${CLIENT_BIN:-$HERE/build/iperf3}"
REMOTE_BIN="${REMOTE_BIN:-DH1:iperf3}"

if [ ! -x "$CLIENT_BIN" ]; then
    echo "perf-test: $CLIENT_BIN not found — run ./scripts/build.sh first" >&2
    exit 2
fi
if ! curl -fs "$API/api/status" >/dev/null; then
    echo "perf-test: devbench REST not reachable at $API" >&2
    exit 3
fi

# Pick the server binary based on MODE. --raw doesn't do iperf3
# protocol so any dumb-recv server works — use pyperf.py if available.
if [ -n "$MODE" ]; then
    SERVER_CMD=(python3 -u "$HERE/../amiga_mcp/scripts/pyperf.py"
                --server --port "$PORT" --bind 0.0.0.0)
    SERVER_KIND="pyperf.py"
    if ! [ -f "$HERE/../amiga_mcp/scripts/pyperf.py" ]; then
        echo "perf-test: pyperf.py not found at expected sibling path" >&2
        echo "           set PYPERF=/path/to/pyperf.py" >&2
        exit 4
    fi
else
    if ! command -v iperf3 >/dev/null; then
        echo "perf-test: iperf3 not on PATH. brew install iperf3." >&2
        exit 4
    fi
    SERVER_CMD=(iperf3 -s -p "$PORT" --forceflush)
    SERVER_KIND="iperf3 -s"
fi

# Deploy the client binary if the guest doesn't already have a fresh copy.
# Always transfer — cheap; guarantees byte-for-byte match.
echo "→ deploy $CLIENT_BIN → $REMOTE_BIN ..."
curl -fs -X POST "$API/api/transfer" \
    -H 'Content-Type: application/json' \
    -d "$(printf '{"source":"%s","dest":"%s","direction":"push"}' \
        "$CLIENT_BIN" "$REMOTE_BIN")" \
    | python3 -c "import sys,json;d=json.load(sys.stdin);print('  ',d.get('message',d))"
curl -fs -X POST "$API/api/launch" \
    -H 'Content-Type: application/json' \
    -d '{"command":"protect '"$REMOTE_BIN"' +rwed"}' >/dev/null

# Start server.
echo "→ start $SERVER_KIND on 0.0.0.0:$PORT"
pkill -f "${SERVER_CMD[0]}.*port $PORT" 2>/dev/null || true
sleep 1
: >/tmp/perf-server.log
"${SERVER_CMD[@]}" >/tmp/perf-server.log 2>&1 &
SERVER_PID=$!
sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "perf-test: server failed to start:"
    cat /tmp/perf-server.log
    exit 5
fi
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Run client on guest.
STAMP=$(date +%s)
OUT="RAM:pt_${STAMP}.txt"
GUEST_CMD="$REMOTE_BIN -c $TARGET -p $PORT -t $DURATION -l $BLKSIZE $MODE >$OUT"
echo "→ guest: $GUEST_CMD"
curl -fs --max-time "$((DURATION + 60))" -X POST "$API/api/launch" \
    -H 'Content-Type: application/json' \
    -d "$(printf '{"command":"%s"}' "$GUEST_CMD")" >/dev/null

sleep 3

echo ""
echo "=== guest client output ==="
curl -fs "$API/api/file?path=$OUT&offset=0&size=4096" \
  | python3 -c "
import sys, json, binascii
d = json.load(sys.stdin)
h = d.get('hexData')
if h:
    print(binascii.unhexlify(h.split('...')[0]).decode('latin-1', errors='replace'))
else:
    print(d.get('content', d))
"

echo ""
echo "=== host server (last 15 lines) ==="
tail -15 /tmp/perf-server.log
