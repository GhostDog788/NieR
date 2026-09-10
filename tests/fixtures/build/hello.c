#include <stddef.h>
#include <stdio.h>

size_t native_width(void);

int main(void) {
    printf("Hello from a normal build: %zu\n", native_width());
    return 0;
}
