#ifndef NIER_TEST_EXTENDED_ABI_H
#define NIER_TEST_EXTENDED_ABI_H

typedef union { float number; unsigned bits; } FloatBits;
typedef union { double number; unsigned long long bits; } DoubleBits;
typedef union { double values[2]; unsigned long long bits[2]; } WideUnion;
typedef struct { float low, high; } FloatPair;
typedef union { double number; FloatPair parts; } FloatUnion;
typedef union {
#if __SIZEOF_POINTER__ == 8
    unsigned long long bits;
#endif
    double number;
} WidthUnion;
typedef struct __attribute__((packed)) { unsigned char lead; unsigned value; } Packed;
typedef struct __attribute__((packed)) { unsigned first, second; } PackedPair;
typedef struct { unsigned first : 3, second : 5, third : 24; } Bits;
typedef struct __attribute__((packed)) { unsigned long long first : 60; unsigned second : 16; } CrossingBits;

FloatBits transform_float_bits(FloatBits);
DoubleBits transform_double_bits(DoubleBits);
WideUnion transform_wide_union(WideUnion);
FloatUnion transform_float_union(FloatUnion);
WidthUnion transform_width_union(WidthUnion);
Packed transform_packed(Packed);
PackedPair transform_packed_pair(PackedPair);
Bits transform_bits(Bits);
CrossingBits transform_crossing_bits(CrossingBits);

#define NIER_EXTENDED_BRIDGE(type, name) \
    type bridge_##name(type (*callback)(type), type value)
NIER_EXTENDED_BRIDGE(FloatBits, float_bits);
NIER_EXTENDED_BRIDGE(DoubleBits, double_bits);
NIER_EXTENDED_BRIDGE(WideUnion, wide_union);
NIER_EXTENDED_BRIDGE(FloatUnion, float_union);
NIER_EXTENDED_BRIDGE(WidthUnion, width_union);
NIER_EXTENDED_BRIDGE(Packed, packed);
NIER_EXTENDED_BRIDGE(PackedPair, packed_pair);
NIER_EXTENDED_BRIDGE(Bits, bits);
NIER_EXTENDED_BRIDGE(CrossingBits, crossing_bits);
#undef NIER_EXTENDED_BRIDGE

Packed packed_stack_pressure(int, int, int, int, int, Packed);
WideUnion union_stack_pressure(int, int, int, int, int, WideUnion, int);

#endif
