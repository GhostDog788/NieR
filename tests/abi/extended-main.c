#include "extended.h"
#include <stdio.h>

int main(void) {
    FloatBits f = {.bits = 0xaabbccddu};
    DoubleBits d = {.bits = 0xaabbccddeeff0011ull};
    WideUnion w = {.bits = {11, 17}};
    FloatUnion u = {.number = 2.5};
    WidthUnion width = {.number = 2.5};
    Packed p = {3, 40};
    PackedPair pp = {10, 20};
    Bits b = {2, 5, 0x112233};
    CrossingBits c = {0x123456789abcdefull, 0x1234};
    f = bridge_float_bits(transform_float_bits, f);
    d = bridge_double_bits(transform_double_bits, d);
    w = bridge_wide_union(transform_wide_union, w);
    u = bridge_float_union(transform_float_union, u);
    width = bridge_width_union(transform_width_union, width);
    p = bridge_packed(transform_packed, p);
    pp = bridge_packed_pair(transform_packed_pair, pp);
    b = bridge_bits(transform_bits, b);
    c = bridge_crossing_bits(transform_crossing_bits, c);
    if (f.bits != (0xaabbccddu ^ 0x11223344u) ||
        d.bits != (0xaabbccddeeff0011ull ^ 0x1122334455667788ull) ||
        w.bits[0] != 28 || w.bits[1] != 17 || u.number != 3.5 || width.number != 3.5 ||
        p.lead != 3 || p.value != 43 || pp.first != 30 || pp.second != 20 ||
        b.first != 3 || b.second != 7 || b.third != (0x112233u ^ 0x123456u) ||
        c.first != 0x123456789abcdefull + 0x1234 || c.second != 0x1235)
        return 1;
    p = packed_stack_pressure(1, 2, 3, 4, 5, p);
    w = union_stack_pressure(1, 2, 3, 4, 5, w, 6);
    if (p.value != 58 || w.bits[0] != 49 || w.bits[1] != 17) return 2;
    puts("Extended native ABI passed");
    return 0;
}
