./tests/multiple_connection_test_server 127.0.0.1:11481 &
./tests/multiple_connection_test_server 127.0.0.1:11482 &
./tests/multiple_connection_test_server 127.0.0.1:11483 &
./tests/multiple_connection_test_server 127.0.0.1:11484 &
./tests/multiple_connection_test_server 127.0.0.1:11485 &
./tests/multiple_connection_test_server 127.0.0.1:11486 &
./tests/multiple_connection_test_server 127.0.0.1:11487 &
./tests/multiple_connection_test_server 127.0.0.1:11488 &
sleep 1
./tests/multiple_connection_test_client 127.0.0.1:11481 127.0.0.1:11482 127.0.0.1:11483 127.0.0.1:11484 127.0.0.1:11485 127.0.0.1:11486 127.0.0.1:11487 127.0.0.1:11488
sleep 3
RET=$?
kill $(jobs -p) 2> /dev/null
exit $RET
