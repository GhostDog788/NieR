CC = clang
CFLAGS ?= -O2

.DEFAULT_GOAL := hello
.PHONY: hello
hello: build/native-make/hello

build/native-make/hello: build/native-make/main.o build/native-make/hello.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

build/native-make/%.o: %.c hello.h
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c "$<" -o "$@"
