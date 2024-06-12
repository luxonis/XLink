g++ xlink_server_local.cpp -g -L../build -lXLink -lusb-1.0 -I../include -o xlink_server_local
g++ xlink_client_local.cpp -g -L../build -lXLink -lusb-1.0 -I../include -o xlink_client_local
