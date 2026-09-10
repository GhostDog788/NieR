CC ?= cc
AR ?= ar
WIDTH := $(shell $(CC) -dM -E -x c /dev/null | awk '/__SIZEOF_POINTER__/ { print $$3 }')
ifeq ($(WIDTH),8)
LIBRARIES = libfirst.a libsecond.a
else
LIBRARIES = libsecond.a libfirst.a
endif

hello: main.o libfirst.a libsecond.a
	$(CC) main.o $(LIBRARIES) -o hello
	./hello

main.o: main.c
	$(CC) -O0 -c $< -o $@

first.o: first.c
	$(CC) -O2 -c $< -o $@

second.o: second.c
	$(CC) -O2 -c $< -o $@

libfirst.a: first.o
	$(AR) rcsD $@ $<

libsecond.a: second.o
	$(AR) rcsD $@ $<
