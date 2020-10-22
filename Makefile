CC = gcc
CFLAGS = -Wall -W -O2 $(shell pkg-config --cflags libusb-1.0)
LDLIBS = $(shell pkg-config --libs libusb-1.0)

ledblink: ledblink.o
	$(CC) -Wall ledblink.o -o ledblink $(LDLIBS)
ledblink.o: ledblink.c
clean:
	rm -f ledblink ledblink.o
