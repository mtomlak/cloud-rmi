CC ?= gcc
AR ?= ar
CFLAGS ?= -g -Wall -Iinclude

LIBNAME = libcloudrmi.a
LIB_OBJS = src/payload_legacy.o src/protocol_legacy.o src/protocol_feed.o

all: $(LIBNAME)

$(LIBNAME): $(LIB_OBJS)
	$(AR) rcs $(LIBNAME) $(LIB_OBJS)

clean:
	rm -f $(LIB_OBJS) $(LIBNAME)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean
