CC ?= cc
CFLAGS ?= -O2

libnierfixture.so: library.o exports.map
	$(CC) -shared -Wl,-soname,libnierfixture.so,--version-script,exports.map library.o -o libnierfixture.so

library.o: library.c
	$(CC) $(CFLAGS) -fPIC -c library.c -o library.o

probe: probe.o libnierfixture.so
	$(CC) probe.o -L. -lnierfixture -Wl,-rpath,'$$ORIGIN' -o probe

probe.o: probe.c
	$(CC) $(CFLAGS) -c probe.c -o probe.o
