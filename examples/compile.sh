g++ xlink_usb_server.cpp -g -I../include -L../build -lXLink -lc -lusb-1.0 -o xlink_usb_server
g++ xlink_usb_client.cpp -g -I../include -L../build -lXLink -lc -lusb-1.0 -o xlink_usb_client

