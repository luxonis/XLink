#!/bin/bash
set -e

SOURCE_DIR=$(dirname "$0")

if [[ "$#" != 4 ]]; then
    echo "Usage $0 [server] [client] [num concurrent connections] [timeout]"
    exit 1
fi

SERVER_EXECUTABLE=$1
CLIENT_EXECUTABLE=$2
NUM_CONNECTIONS=$3
TIMEOUT=$4
SETUP_SLEEP=1

IP="127.0.0.1"
START_PORT="11490"

CONNECTION_IPS=""
for (( i=0; i < $NUM_CONNECTIONS; i++ )); do
    PORT=$(($START_PORT + $i))
    "$SOURCE_DIR/timeout.sh" -t $(($TIMEOUT+$SETUP_SLEEP)) -d 3 "$SERVER_EXECUTABLE" "$IP:$PORT" &
    CONNECTION_IPS="$CONNECTION_IPS $IP:$PORT"
done

echo "$CONNECTION_IPS"
sleep $SETUP_SLEEP
"$SOURCE_DIR/timeout.sh" -t $TIMEOUT -d 3 "$CLIENT_EXECUTABLE" $CONNECTION_IPS
RET=$?
kill $(jobs -p) 2> /dev/null || true
exit $RET
