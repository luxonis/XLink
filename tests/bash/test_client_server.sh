#!/bin/bash
set -e

if [[ "$#" != 4 ]]; then
    echo "Usage $0 [server] [client] [num iterations] [timeout]"
    exit 1
fi

SERVER_EXECUTABLE=$1
CLIENT_EXECUTABLE=$2
NUM_ITERATIONS=$3
TIMEOUT=$4

echo "Server: $SERVER_EXECUTABLE, Client: $CLIENT_EXECUTABLE, Iterations: $NUM_ITERATIONS, Timeout: $TIMEOUT"

for ((i = 0 ; i < $NUM_ITERATIONS ; i++ )); do
    timeout $TIMEOUT "$SERVER_EXECUTABLE" &
    sleep 1
    timeout $TIMEOUT "$CLIENT_EXECUTABLE" &
    wait $(jobs -p)
done
