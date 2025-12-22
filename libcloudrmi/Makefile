CC ?= gcc
AR ?= ar
CFLAGS += -Iinclude
CFLAGS ?= -g -Wall

LIBNAME = libcloudrmi.a
LIB_OBJS = src/payload.o src/protocol_line.o src/protocol_feed.o src/semantics.o

all: $(LIBNAME)

$(LIBNAME): $(LIB_OBJS)
	$(AR) rcs $(LIBNAME) $(LIB_OBJS)

clean:
	rm -f $(LIB_OBJS) $(LIBNAME)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean
