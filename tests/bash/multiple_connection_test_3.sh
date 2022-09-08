./tests/multiple_connection_test_server 127.0.0.1:11471 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11472 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11473 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11474 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11475 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11476 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11477 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11478 > /dev/null 2> /dev/null &

./tests/multiple_connection_test_server 127.0.0.1:11481 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11482 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11483 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11484 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11485 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11486 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11487 > /dev/null 2> /dev/null &
./tests/multiple_connection_test_server 127.0.0.1:11488 > /dev/null 2> /dev/null &
sleep 1
./tests/multiple_connection_test_client 127.0.0.1:11471 127.0.0.1:11472 127.0.0.1:11473 127.0.0.1:11474 127.0.0.1:11475 127.0.0.1:11476 127.0.0.1:11477 127.0.0.1:11478 127.0.0.1:11481 127.0.0.1:11482 127.0.0.1:11483 127.0.0.1:11484 127.0.0.1:11485 127.0.0.1:11486 127.0.0.1:11487 127.0.0.1:11488
RET=$?
kill $(jobs -p) 2> /dev/null
exit $RET