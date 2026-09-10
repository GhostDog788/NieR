CC ?= cc
CFLAGS ?= -O2
WIDTH := $(shell $(CC) -dM -E -x c /dev/null | awk '/__SIZEOF_POINTER__/ { print $$3 }')
ifeq ($(WIDTH),8)
PARTS = first.o second.o
else
PARTS = joined.o
endif

hello: main.o $(PARTS)
	$(CC) main.o $(PARTS) -o hello
	./hello

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
