g++ xlink_server_local.cpp -I../include -L../build -lXLink -lc -lusb-1.0 -o xlink_server_local
g++ xlink_client_local.cpp -I../include -L../build -lXLink -lc -lusb-1.0 -o xlink_client_local

