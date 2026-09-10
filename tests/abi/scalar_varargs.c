#include <stdarg.h>
#include <stdio.h>

static int pointed_value = 7;

long long scalar_values(int groups, ...) {
    va_list arguments, copied;
    va_start(arguments, groups);
    va_copy(copied, arguments);
    long long first = 0;
    for (int index = 0; index < groups; ++index) {
        first += va_arg(arguments, int);
        first += va_arg(arguments, long long);
        first += *va_arg(arguments, int *);
        first += (long long)va_arg(arguments, double);
    }
    va_end(arguments);
    long long second = 0;
    for (int index = 0; index < groups; ++index) {
        second += va_arg(copied, int);
        second += va_arg(copied, long long);
        second += *va_arg(copied, int *);
        second += (long long)va_arg(copied, double);
    }
    va_end(copied);
    return first == second ? first : -1;
}

int main(void) {
    long long result = scalar_values(10,
        (signed char)1, 1001LL, &pointed_value, 1.0f,
        (short)2, 1002LL, &pointed_value, 2.0f,
        3, 1003LL, &pointed_value, 3.0,
        4, 1004LL, &pointed_value, 4.0,
        5, 1005LL, &pointed_value, 5.0,
        6, 1006LL, &pointed_value, 6.0,
        7, 1007LL, &pointed_value, 7.0,
        8, 1008LL, &pointed_value, 8.0,
        9, 1009LL, &pointed_value, 9.0,
        10, 1010LL, &pointed_value, 10.0);
    if (result != 10235LL) return 1;
    puts("Scalar varargs passed");
    return 0;
}
