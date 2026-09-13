#include <stdio.h>
#include <sela_options.h>
#if __SIZEOF_POINTER__ == 8
#include <sela_width64.h>
#else
#include <sela_width32.h>
#endif
#ifndef PUBLISHED_VALUE
#error PUBLISHED_VALUE must be provided through ordinary Clang -D
#endif
int main(void) {
    printf("%d %d\n", HEADER_VALUE, PUBLISHED_VALUE);
    return 0;
}
