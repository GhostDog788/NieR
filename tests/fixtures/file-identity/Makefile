CC ?= cc
CFLAGS ?= -O2

hello: main.o
	$(CC) $(CFLAGS) main.o -o hello
	./hello

main.o: main.c
	$(CC) $(CFLAGS) -c "$(CURDIR)/main.c" -o main.o
