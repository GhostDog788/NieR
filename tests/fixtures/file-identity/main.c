#include <stdio.h>
#include <string.h>

int main(void) {
    puts(__FILE__);
    return strcmp(__FILE__, "/nier/source/main.c") != 0;
}
