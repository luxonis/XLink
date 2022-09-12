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
    nc -l -p $PORT &
    CONNECTION_IPS="$CONNECTION_IPS $IP:$PORT"
done
echo "$CONNECTION_IPS"
sleep 1
./tests/connect_timeout_test $CONNECTION_IPS
RET=$?
kill $(jobs -p) 2> /dev/null
exit $RET
