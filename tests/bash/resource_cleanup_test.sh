#!/bin/bash
set -euo pipefail

SOURCE_DIR=$(dirname "$0")

if [[ "$#" != 4 ]]; then
    echo "Usage $0 [server] [client] [num rounds] [timeout]"
    exit 1
fi

SERVER_EXECUTABLE=$1
CLIENT_EXECUTABLE=$2
NUM_ROUNDS=$3
TIMEOUT=$4

IP=${XLINK_LEAK_TEST_IP:-127.0.0.1:11790}
CLIENT_TIMEOUT=${XLINK_LEAK_TEST_CLIENT_TIMEOUT:-1800}

cleanup() {
    jobs -p | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT

(
    for ((round=0; round<NUM_ROUNDS; round++)); do
        echo "Starting leak-check server round $round"
        "$SOURCE_DIR/timeout.sh" -t "$TIMEOUT" -d 3 "$SERVER_EXECUTABLE" "$IP"
    done
) &
SERVER_LOOP_PID=$!

"$SOURCE_DIR/timeout.sh" -t "$CLIENT_TIMEOUT" -d 3 "$CLIENT_EXECUTABLE" "$NUM_ROUNDS" "$IP" &
CLIENT_PID=$!

CLIENT_RET=0
while kill -0 "$CLIENT_PID" 2>/dev/null; do
    if ! kill -0 "$SERVER_LOOP_PID" 2>/dev/null; then
        if ! wait "$SERVER_LOOP_PID"; then
            kill "$CLIENT_PID" 2>/dev/null || true
            wait "$CLIENT_PID" || true
            exit 1
        fi
    fi
    sleep 1
done

if ! wait "$CLIENT_PID"; then
    CLIENT_RET=$?
fi

SERVER_RET=0
if ! wait "$SERVER_LOOP_PID"; then
    SERVER_RET=1
fi

if [[ "$CLIENT_RET" != 0 ]]; then
    exit "$CLIENT_RET"
fi

exit "$SERVER_RET"
