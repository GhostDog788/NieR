#include "extended.h"

FloatBits transform_float_bits(FloatBits value) {
    value.bits ^= 0x11223344u;
    return value;
}
DoubleBits transform_double_bits(DoubleBits value) {
    value.bits ^= 0x1122334455667788ull;
    return value;
}
WideUnion transform_wide_union(WideUnion value) {
    value.bits[0] += value.bits[1];
    return value;
}
FloatUnion transform_float_union(FloatUnion value) {
    value.number += 1.0;
    return value;
}
WidthUnion transform_width_union(WidthUnion value) {
    value.number += 1.0;
    return value;
}
Packed transform_packed(Packed value) {
    value.value += value.lead;
    return value;
}
PackedPair transform_packed_pair(PackedPair value) {
    value.first += value.second;
    return value;
}
Bits transform_bits(Bits value) {
    value.first += 1;
    value.second += 2;
    value.third ^= 0x123456u;
    return value;
}
CrossingBits transform_crossing_bits(CrossingBits value) {
    value.first += value.second;
    value.second += 1;
    return value;
}
Packed packed_stack_pressure(int a, int b, int c, int d, int e, Packed value) {
    value.value += a + b + c + d + e;
    return value;
}
WideUnion union_stack_pressure(int a, int b, int c, int d, int e, WideUnion value, int tail) {
    value.bits[0] += a + b + c + d + e + tail;
    return value;
}
