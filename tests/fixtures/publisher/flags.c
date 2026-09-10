#include <stdio.h>
#include <nier_options.h>
#if __SIZEOF_POINTER__ == 8
#include <nier_width64.h>
#else
#include <nier_width32.h>
#endif
#ifndef PUBLISHED_VALUE
#error PUBLISHED_VALUE must be provided through ordinary Clang -D
#endif
int main(void) {
    printf("%d %d\n", HEADER_VALUE, PUBLISHED_VALUE);
    return 0;
}
