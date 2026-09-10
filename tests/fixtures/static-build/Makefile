CC ?= cc
AR ?= ar
CFLAGS ?= -O2

libduplicate.a: one/member.o two/member.o
	$(AR) qcD libduplicate.a one/member.o two/member.o

empty.a:
	$(AR) rcsD empty.a

ordinary: probe.o libduplicate.a
	$(CC) probe.o libduplicate.a -o ordinary

group: probe.o libduplicate.a
	$(CC) probe.o -Wl,--start-group libduplicate.a libduplicate.a -Wl,--end-group -o group

whole-native-error: probe.o libduplicate.a
	$(CC) probe.o -Wl,--whole-archive libduplicate.a -Wl,--no-whole-archive -o whole-native-error

libdistinct.a: distinct-one/member.o distinct-two/member.o
	$(AR) qcD libdistinct.a distinct-one/member.o distinct-two/member.o

whole: distinct-probe.o libdistinct.a
	$(CC) distinct-probe.o -Wl,--whole-archive libdistinct.a -Wl,--no-whole-archive -o whole

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
