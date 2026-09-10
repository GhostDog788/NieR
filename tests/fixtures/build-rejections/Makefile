CC ?= cc
AR ?= ar
CFLAGS ?= -O2

archive: main.o libhelper.a
	$(CC) main.o libhelper.a -o archive

thin: main.o libhelper-thin.a
	$(CC) main.o libhelper-thin.a -o thin

libhelper-thin.a: helper.o unused.o
	$(AR) rcsT libhelper-thin.a helper.o unused.o

libhelper.a: helper.o unused.o
	$(AR) rcs libhelper.a helper.o unused.o

repeated-journal: main.o librepeated.a
	$(CC) main.o librepeated.a -o repeated-journal

librepeated.a: helper.o
	$(AR) qcD librepeated.a helper.o helper.o

whole: main.o libhelper.a
	$(CC) main.o -Wl,--whole-archive libhelper.a -Wl,--no-whole-archive -o whole

group: main.o libhelper.a
	$(CC) main.o -Wl,--start-group libhelper.a libhelper.a -Wl,--end-group -o group

renamed: main.o helper.lo
	$(CC) main.o helper.lo -o renamed

helper.lo: helper.o
	mv helper.o helper.lo

missing-capture: main.o helper.o
	objcopy --remove-section=.nier.capture helper.o uncaptured.o
	$(CC) main.o uncaptured.o -o missing-capture

mutated-object: changed-main.o helper.o
	objcopy --redefine-sym helper=changed_helper helper.o changed-helper.o
	$(CC) changed-main.o changed-helper.o -o mutated-object

transplanted-marker: main.o helper.o alternate.o
	objcopy --dump-section .nier.capture=marker.bin helper.o
	objcopy --update-section .nier.capture=marker.bin alternate.o transplanted.o
	$(CC) main.o transplanted.o -o transplanted-marker

dependent: main.o libhelper.so
	$(CC) main.o -L. -lhelper -Wl,-rpath,. -o dependent

libhelper.so: helper.o
	$(CC) -shared helper.o -Wl,-soname,libhelper.so -o libhelper.so

unsupported-link: main.o helper.o
	$(CC) main.o helper.o -Wl,-z,execstack -o unsupported-link

unsupported-interpreter: main.o helper.o
	$(CC) main.o helper.o -Wl,--dynamic-linker,/unqualified/loader.so -o unsupported-interpreter

hash-override: main.o helper.o
	$(CC) main.o helper.o -Wl,--hash-style=both,--hash-style=gnu -o hash-override

%.o: %.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
