#include <stdio.h>

union ScalarStorage {
    signed char byte;
    short half;
    int integer;
#if __SIZEOF_POINTER__ == 8
    long long wide;
#endif
    float single;
    double real;
};

static union ScalarStorage persistent;

__attribute__((noinline)) int check_storage(int argument) {
    union ScalarStorage local;
    persistent.real = 42.5;
    local.real = persistent.real;
    if (local.real != 42.5) return 1;
    local.integer = argument;
    persistent.integer = local.integer;
    if (persistent.integer != 73) return 2;
    persistent.single = 1.5f;
    return persistent.single != 1.5f || sizeof(local) != 8;
}

int main(void) {
    int result = check_storage(73);
    if (result) return result;
    puts("Overlapping native storage passed");
    return 0;
}
