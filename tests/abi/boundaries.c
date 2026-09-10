#include "abi.h"

Pair pair_transform(Pair value, int delta) {
    value.first += delta;
    value.second -= delta;
    return value;
}

Mixed mixed_transform(Mixed value, int delta) {
    value.amount += delta;
    value.tag -= delta;
    return value;
}

Large large_transform(Large value, int delta) {
    value.first += delta;
    value.second -= delta;
    value.tag += delta;
    return value;
}

Pair pair_indirect(PairCallback callback, Pair value, int delta) {
    return callback(value, delta);
}

Mixed mixed_indirect(MixedCallback callback, Mixed value, int delta) {
    return callback(value, delta);
}

Large large_indirect(LargeCallback callback, Large value, int delta) {
    return callback(value, delta);
}

Pair pair_register_edge(int a, int b, int c, int d, int e, Pair value, int tail) {
    value.first += a + b + c + d + e + tail;
    return value;
}

Pair pair_stack_edge(int a, int b, int c, int d, int e, int f, Pair value, int tail) {
    value.first += a + b + c + d + e + f + tail;
    return value;
}

Mixed mixed_stack_edge(int a, int b, int c, int d, int e, int f,
                       double x0, double x1, double x2, double x3,
                       double x4, double x5, double x6, double x7,
                       Mixed value, int tail) {
    value.amount += x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
    value.tag += a + b + c + d + e + f + tail;
    return value;
}
