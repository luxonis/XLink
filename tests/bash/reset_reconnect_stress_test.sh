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
DIAGNOSTIC_CLIENT_GRACE_MS=${XLINK_STRESS_DIAGNOSTIC_CLIENT_GRACE_MS:-0}

IP="127.0.0.1"
START_PORT="11690"
NUM_CONNECTIONS=4

cleanup() {
    jobs -p | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT

CONNECTION_IPS=()
for ((i=0; i<NUM_CONNECTIONS; i++)); do
    CONNECTION_IPS+=("$IP:$((START_PORT + i))")
done

SERVER_LOOP_PIDS=()
for ((i=0; i<NUM_CONNECTIONS; i++)); do
    (
        for ((round=0; round<NUM_ROUNDS; round++)); do
            echo "Starting server conn $i round $round"
            "$SOURCE_DIR/timeout.sh" -t "$TIMEOUT" -d 3 "$SERVER_EXECUTABLE" "${CONNECTION_IPS[$i]}"
        done
    ) &
    SERVER_LOOP_PIDS+=($!)
done

CLIENT_TIMEOUT=$((TIMEOUT + (NUM_ROUNDS * 2) + 10))
"$SOURCE_DIR/timeout.sh" -t "$CLIENT_TIMEOUT" -d 3 "$CLIENT_EXECUTABLE" "$NUM_ROUNDS" "${CONNECTION_IPS[@]}" &
CLIENT_PID=$!

CLIENT_RET=0
while kill -0 "$CLIENT_PID" 2>/dev/null; do
    for pid in "${SERVER_LOOP_PIDS[@]}"; do
        if ! kill -0 "$pid" 2>/dev/null; then
            if ! wait "$pid"; then
                if [[ "$DIAGNOSTIC_CLIENT_GRACE_MS" -gt 0 ]]; then
                    sleep "$(awk "BEGIN { print $DIAGNOSTIC_CLIENT_GRACE_MS / 1000 }")"
                fi
                kill "$CLIENT_PID" 2>/dev/null || true
                wait "$CLIENT_PID" || true
                exit 1
            fi
        fi
    done
    sleep 1
done

if ! wait "$CLIENT_PID"; then
    CLIENT_RET=$?
fi

SERVER_RET=0
for pid in "${SERVER_LOOP_PIDS[@]}"; do
    if ! wait "$pid"; then
        SERVER_RET=1
    fi
done

if [[ "$CLIENT_RET" != 0 ]]; then
    exit "$CLIENT_RET"
fi
exit "$SERVER_RET"
