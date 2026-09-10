#include <stdio.h>
__attribute__((constructor)) static void announce(void) {
    puts("constructor");
}
