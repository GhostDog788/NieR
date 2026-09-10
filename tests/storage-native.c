#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>

static int increment(int value) { return value + 1; }

struct State {
    char marker;
    size_t count;
    int (*callback)(int);
    int values[3];
};

static struct State state = {7, 17, increment, {3, 1, 2}};
static int (*selected)(int) = increment;

static int compare(const void *left, const void *right) {
    return *(const int *)left - *(const int *)right;
}

static int checkpoint(void) {
    jmp_buf environment;
    switch (setjmp(environment)) {
    case 0:
        longjmp(environment, 42);
    case 42:
        return 42;
    default:
        return -1;
    }
}

int main(void) {
    qsort(state.values, 3, sizeof(state.values[0]), compare);
    return state.marker != 7 || state.count != 17 ||
           state.values[0] != 1 || state.values[1] != 2 || state.values[2] != 3 ||
           state.callback(10) != 11 || selected(20) != 21 || checkpoint() != 42;
}
