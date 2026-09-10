CC ?= cc
CFLAGS ?= -O2

hello: hello.o
	$(CC) $(CFLAGS) hello.o -o hello

hello.o: hello.c generated.h
	$(CC) $(CFLAGS) -c hello.c -o hello.o

generated.h: generator
	./generator generated.h

generator: generator.c
	$(CC) $(CFLAGS) generator.c -o generator
