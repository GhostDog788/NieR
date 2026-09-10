#include <stddef.h>
#include <stdio.h>

static size_t pointer_size(void) {
    return sizeof(void *);
}

int main(void) {
    size_t width = pointer_size();
    printf("pointer=%zu word=%zu fixed=%u,%u\n", width, sizeof(size_t), 4u, 8u);
    return 0;
}
