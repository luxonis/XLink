NUM=$1
if [[ "$#" != 1 ]]; then
    echo "Usage $0 [num concurrent connections]"
    exit 1
fi

IP="127.0.0.1"
START_PORT="11490"

CONNECTION_IPS=""
for (( i=0; i < $NUM; i++ )); do
    PORT=$(($START_PORT + $i))
    URL=$IP:$PORT
    ./tests/multiple_connection_test_server $URL &
    CONNECTION_IPS="$CONNECTION_IPS $URL"
done

echo "$CONNECTION_IPS"
sleep 1
./tests/multiple_connection_test_client $CONNECTION_IPS
RET=$?
kill $(jobs -p) 2> /dev/null
exit $RET
