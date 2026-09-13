#include <stdio.h>
int device_add(int value);
int main(void) {
    int answer = device_add(37);
    printf("Shared answer=%d\n", answer);
    return answer != 42;
}
