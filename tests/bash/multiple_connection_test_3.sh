./tests/multiple_connection_test_server 127.0.0.1:11471 &
./tests/multiple_connection_test_server 127.0.0.1:11472 &
./tests/multiple_connection_test_server 127.0.0.1:11473 &
./tests/multiple_connection_test_server 127.0.0.1:11474 &
./tests/multiple_connection_test_server 127.0.0.1:11475 &
./tests/multiple_connection_test_server 127.0.0.1:11476 &
./tests/multiple_connection_test_server 127.0.0.1:11477 &
./tests/multiple_connection_test_server 127.0.0.1:11478 &

./tests/multiple_connection_test_server 127.0.0.1:11481 &
./tests/multiple_connection_test_server 127.0.0.1:11482 &
./tests/multiple_connection_test_server 127.0.0.1:11483 &
./tests/multiple_connection_test_server 127.0.0.1:11484 &
./tests/multiple_connection_test_server 127.0.0.1:11485 &
./tests/multiple_connection_test_server 127.0.0.1:11486 &
./tests/multiple_connection_test_server 127.0.0.1:11487 &
./tests/multiple_connection_test_server 127.0.0.1:11488 &
sleep 1
./tests/multiple_connection_test_client 127.0.0.1:11471 127.0.0.1:11472 127.0.0.1:11473 127.0.0.1:11474 127.0.0.1:11475 127.0.0.1:11476 127.0.0.1:11477 127.0.0.1:11478 127.0.0.1:11481 127.0.0.1:11482 127.0.0.1:11483 127.0.0.1:11484 127.0.0.1:11485 127.0.0.1:11486 127.0.0.1:11487 127.0.0.1:11488
RET=$?
wait $(jobs -p)
exit $RET
