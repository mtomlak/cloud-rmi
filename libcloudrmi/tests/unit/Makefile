CC ?= gcc
CFLAGS ?= -g -Wall -I../../include -I../../src -I../../../src -DPROXY_SUPPORT
LDFLAGS ?= ../../libcloudrmi.a

protocol_feed_test: protocol_feed_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

proxy_payload_test: proxy_payload_test.c ../../../src/proxy.c ../../../src/cloud-rmi.c ../../../src/client.c ../../../src/config.c ../../../src/capabilities.c
	$(CC) $(CFLAGS) -o $@ $< ../../../src/proxy.c ../../../src/cloud-rmi.c ../../../src/client.c ../../../src/config.c ../../../src/capabilities.c $(LDFLAGS)

semantics_test: semantics_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

all: protocol_feed_test proxy_payload_test semantics_test

clean:
	rm -f protocol_feed_test proxy_payload_test semantics_test

.PHONY: all clean
