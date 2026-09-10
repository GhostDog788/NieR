#include <stddef.h>
#include <stdio.h>

__attribute__((noinline)) size_t next_word(size_t input) {
    return input + 1;
}

unsigned pointer_bytes(void) { return sizeof(void *); }
unsigned fixed_four(void) { return 4; }
unsigned fixed_eight(void) { return 8; }

int main(void) {
    unsigned long long wide = 4294967304ULL;
    printf("pointer=%u literals=%u,%u next=%zu fixed64=%llu\n",
           pointer_bytes(), fixed_four(), fixed_eight(), next_word(sizeof(void *)), wide);
    return 0;
}
