CC ?= gcc
CFLAGS ?= -Wall -I../include
LDFLAGS ?= ../libcloudrmi.a

protocol_feed_test: protocol_feed_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f protocol_feed_test

.PHONY: clean
