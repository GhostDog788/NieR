#include <stdio.h>
int archive_pick(void);
int main(void) {
    int answer = archive_pick();
    printf("Static answer=%d\n", answer);
    return answer != 42;
}
