#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int native_format(char *output, size_t capacity, const char *format, ...) {
    va_list arguments, copied;
    va_start(arguments, format);
    va_copy(copied, arguments);
    int count = vsnprintf(output, capacity, format, copied);
    va_end(copied);
    va_end(arguments);
    return count;
}

int main(void) {
    char text[96];
    int count = native_format(text, sizeof(text), "%d %lld %s %.1f",
                              11, 1234567890123LL, "ok", 2.5);
    if (count < 0 || strcmp(text, "11 1234567890123 ok 2.5")) return 1;
    puts("Native va_list forwarding passed");
    return 0;
}
