#include "abi.h"
#include <stdio.h>

/* Fixed-argument aggregate ABI gate. Aggregate va_arg extraction has its own
   unchanged native-only fixture and is not included in this qualification. */
int main(void) {
    Pair pair = {10, 20};
    Mixed mixed = {2.5, 20};
    Large large = {2.5, 8.5, 10};
    Pair p = pair_transform(pair, 3);
    if (p.first != 13 || p.second != 17) return 1;
    p = pair_indirect(pair_transform, pair, 4);
    if (p.first != 14 || p.second != 16) return 2;
    p = native_pair_bridge(pair_transform, pair, 5);
    if (p.first != 15 || p.second != 15) return 3;
    Mixed m = mixed_transform(mixed, 3);
    if (m.amount != 5.5 || m.tag != 17) return 4;
    m = mixed_indirect(mixed_transform, mixed, 4);
    if (m.amount != 6.5 || m.tag != 16) return 5;
    m = native_mixed_bridge(mixed_transform, mixed, 5);
    if (m.amount != 7.5 || m.tag != 15) return 6;
    Large l = large_transform(large, 3);
    if (l.first != 5.5 || l.second != 5.5 || l.tag != 13) return 7;
    l = large_indirect(large_transform, large, 4);
    if (l.first != 6.5 || l.second != 4.5 || l.tag != 14) return 8;
    l = native_large_bridge(large_transform, large, 5);
    if (l.first != 7.5 || l.second != 3.5 || l.tag != 15) return 9;
    p = pair_register_edge(1, 2, 3, 4, 5, pair, 6);
    if (p.first != 31 || p.second != 20) return 10;
    p = pair_stack_edge(1, 2, 3, 4, 5, 6, pair, 7);
    if (p.first != 38 || p.second != 20) return 11;
    m = mixed_stack_edge(1, 2, 3, 4, 5, 6,
                         0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, mixed, 7);
    if (m.amount != 34.5 || m.tag != 48) return 12;
    puts("NieR fixed aggregate ABI matrix passed");
    return 0;
}
