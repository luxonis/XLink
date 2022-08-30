./tests/multiple_connection_test_server 127.0.0.1:5551 &
./tests/multiple_connection_test_server 127.0.0.1:5552 &
sleep 1
./tests/multiple_connection_test_client 127.0.0.1:5551 127.0.0.1:5552
sleep 3
RET=$?
kill $(jobs -p) 2> /dev/null
exit $RET
