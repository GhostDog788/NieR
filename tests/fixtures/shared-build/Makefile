CC ?= cc
CFLAGS ?= -O2

libselafixture.so: library.o exports.map
	$(CC) -shared -Wl,-soname,libselafixture.so,--version-script,exports.map library.o -o libselafixture.so

library.o: library.c
	$(CC) $(CFLAGS) -fPIC -c library.c -o library.o

probe: probe.o libselafixture.so
	$(CC) probe.o -L. -lselafixture -Wl,-rpath,'$$ORIGIN' -o probe

probe.o: probe.c
	$(CC) $(CFLAGS) -c probe.c -o probe.o
