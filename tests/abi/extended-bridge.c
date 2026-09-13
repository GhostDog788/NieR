#include "extended.h"

#define SELA_EXTENDED_BRIDGE(type, name) \
    type bridge_##name(type (*callback)(type), type value) { return callback(value); }
SELA_EXTENDED_BRIDGE(FloatBits, float_bits)
SELA_EXTENDED_BRIDGE(DoubleBits, double_bits)
SELA_EXTENDED_BRIDGE(WideUnion, wide_union)
SELA_EXTENDED_BRIDGE(FloatUnion, float_union)
SELA_EXTENDED_BRIDGE(WidthUnion, width_union)
SELA_EXTENDED_BRIDGE(Packed, packed)
SELA_EXTENDED_BRIDGE(PackedPair, packed_pair)
SELA_EXTENDED_BRIDGE(Bits, bits)
SELA_EXTENDED_BRIDGE(CrossingBits, crossing_bits)
