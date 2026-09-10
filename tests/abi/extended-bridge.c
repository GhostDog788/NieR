#include "extended.h"

#define NIER_EXTENDED_BRIDGE(type, name) \
    type bridge_##name(type (*callback)(type), type value) { return callback(value); }
NIER_EXTENDED_BRIDGE(FloatBits, float_bits)
NIER_EXTENDED_BRIDGE(DoubleBits, double_bits)
NIER_EXTENDED_BRIDGE(WideUnion, wide_union)
NIER_EXTENDED_BRIDGE(FloatUnion, float_union)
NIER_EXTENDED_BRIDGE(WidthUnion, width_union)
NIER_EXTENDED_BRIDGE(Packed, packed)
NIER_EXTENDED_BRIDGE(PackedPair, packed_pair)
NIER_EXTENDED_BRIDGE(Bits, bits)
NIER_EXTENDED_BRIDGE(CrossingBits, crossing_bits)
