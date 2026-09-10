#include "abi.h"

/* Ordinary ordered records, with no source ABI annotations or wrappers. */
typedef struct { float first, second; } TwoFloats;
typedef struct { long first, second; } TwoWords;
typedef struct { char bytes[3]; } ThreeBytes;
typedef struct { Pair value; float amount; } Nested;

TwoFloats two_floats(TwoFloats value) {
    value.first += value.second;
    return value;
}

TwoFloats float_bank_full(double a, double b, double c, double d,
                         double e, double f, double g, double h,
                         TwoFloats value) {
    value.first += a + b + c + d + e + f + g + h;
    return value;
}

TwoFloats both_banks_full(int a, int b, int c, int d, int e, int f,
                         double p, double q, double r, double s,
                         double t, double u, double v, double w,
                         TwoFloats value) {
    value.first += a + b + c + d + e + f + p + q + r + s + t + u + v + w;
    return value;
}

Large hidden_result_pressure(int a, int b, int c, int d, int e, Mixed value) {
    Large result = {value.amount, a + b + c + d + e, value.tag};
    return result;
}

/* One free GP register cannot split a two-GP aggregate. The whole aggregate
   goes to memory, leaving that register available for the following scalar. */
TwoWords rollback_gp(int a, int b, int c, int d, int e,
                     TwoWords value, int tail) {
    value.first += a + b + c + d + e + tail;
    return value;
}

ThreeBytes three_bytes(ThreeBytes value) {
    value.bytes[0] += value.bytes[1];
    return value;
}

Nested nested_record(Nested value) {
    value.value.first += value.value.second;
    value.amount += 1.0f;
    return value;
}
