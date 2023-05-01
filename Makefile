CXX?=g++
CXX_FLAGS=-std=c++2a -Wall -g -I/usr/include/libusb-1.0
LD_FLAGS=-g -lusb-1.0 -lfmt

usb_http: usb_http.o server.o
	g++ $^ $(LD_FLAGS) -o $@

%.o: %.cc
	g++ -c $^ $(CXX_FLAGS) -o $@


