#include <stdio.h>
extern int public_answer(void);
int executable_export(void) { return 17; }
int main(void) {
    printf("%d\n", public_answer());
    return public_answer() != 42;
}
