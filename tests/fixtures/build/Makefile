CC ?= cc
CFLAGS ?= -O2

hello: hello.o helper.o
	$(CC) $(CFLAGS) hello.o helper.o -o hello

hello.o: hello.c
	$(CC) $(CFLAGS) -c hello.c -o hello.o

helper.o: helper.c
	$(CC) $(CFLAGS) -c helper.c -o helper.o
