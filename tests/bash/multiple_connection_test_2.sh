./tests/multiple_connection_test_server 127.0.0.1:5551 &
./tests/multiple_connection_test_server 127.0.0.1:5552 &
# ./tests/multiple_connection_test_server 127.0.0.1:5553 &
# ./tests/multiple_connection_test_server 127.0.0.1:5554 &
# ./tests/multiple_connection_test_server 127.0.0.1:5555 &
# ./tests/multiple_connection_test_server 127.0.0.1:5556 &
# ./tests/multiple_connection_test_server 127.0.0.1:5557 &
# ./tests/multiple_connection_test_server 127.0.0.1:5558 &
# ./tests/multiple_connection_test_client 127.0.0.1:5551 127.0.0.1:5552 127.0.0.1:5553 127.0.0.1:5554 127.0.0.1:5555 127.0.0.1:5556 127.0.0.1:5557 127.0.0.1:5558
sleep 1
./tests/multiple_connection_test_client 127.0.0.1:5551 127.0.0.1:5552 # 127.0.0.1:5553 127.0.0.1:5554 127.0.0.1:5555 127.0.0.1:5556 127.0.0.1:5557 127.0.0.1:5558
RET=$?
wait $(jobs -p)
exit $RET
