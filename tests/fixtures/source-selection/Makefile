CC ?= cc
CFLAGS ?= -O2
WIDTH := $(shell $(CC) -dM -E -x c /dev/null | awk '/__SIZEOF_POINTER__/ { print $$3 }')

hello: main.o platform.o
	$(CC) main.o platform.o -o hello

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

platform.o: platform$(WIDTH).c
	$(CC) $(CFLAGS) -c $< -o platform.o
