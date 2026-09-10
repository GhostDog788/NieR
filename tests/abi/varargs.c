#include "abi.h"
#include <stdarg.h>

double scalar_variadic(int count, ...) {
    va_list arguments;
    va_start(arguments, count);
    double result = 0;
    for (int index = 0; index < count; ++index) {
        result += va_arg(arguments, int);
        result += va_arg(arguments, double);
    }
    va_end(arguments);
    return result;
}

double aggregate_variadic(int prefix, ...) {
    va_list arguments;
    va_start(arguments, prefix);
    double result = 0;
    for (int index = 0; index < prefix; ++index) {
        result += va_arg(arguments, int);
        result += va_arg(arguments, double);
    }
    Pair pair = va_arg(arguments, Pair);
    Mixed mixed = va_arg(arguments, Mixed);
    Large large = va_arg(arguments, Large);
    va_end(arguments);
    return result + pair.first + pair.second + mixed.amount + mixed.tag
        + large.first + large.second + large.tag;
}
