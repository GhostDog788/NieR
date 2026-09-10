#include <stdio.h>

volatile unsigned effects;

__attribute__((noinline)) unsigned observe(unsigned value) {
    effects = effects * 3 + value;
    return value + 1;
}

__attribute__((noinline)) unsigned choose(unsigned selector, unsigned *storage) {
    unsigned result;
    switch (selector) {
    case 1:
        result = observe(10);
        *storage += 1;
        break;
#if __SIZEOF_POINTER__ == 8
    case 8:
        result = observe(80);
        *storage += 8;
        break;
    case 16:
        result = observe(160);
        *storage += 16;
        break;
#else
    case 4:
        result = observe(40);
        *storage += 4;
        break;
#endif
    case 2:
        result = observe(20);
        *storage += 2;
        break;
    default:
        result = observe(30);
        *storage += 3;
        break;
    }
    *storage += result;
    return result;
}

int main(void) {
    unsigned storage = 5;
    unsigned sum = choose(1, &storage);
    sum += choose(2, &storage);
    sum += choose(8, &storage);
    sum += choose(4, &storage);
    sum += choose(99, &storage);
    sum += choose(16, &storage);
    printf("switch %u %u %u\n", sum, storage, effects);
    return 0;
}
