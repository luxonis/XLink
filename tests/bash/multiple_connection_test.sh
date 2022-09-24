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

IP="127.0.0.1"
START_PORT="11490"

CONNECTION_IPS=""
for (( i=0; i < $NUM_CONNECTIONS; i++ )); do
    PORT=$(($START_PORT + $i))
    URL=$IP:$PORT
    "$SOURCE_DIR/timeout.sh" -t $TIMEOUT -d 3 "$SERVER_EXECUTABLE" "$URL" &
    CONNECTION_IPS="$CONNECTION_IPS $URL"
done

echo "$CONNECTION_IPS"
sleep 1
"$SOURCE_DIR/timeout.sh" -t $TIMEOUT -d 3 "$CLIENT_EXECUTABLE" $CONNECTION_IPS
RET=$?
wait $(jobs -p)
exit $RET
