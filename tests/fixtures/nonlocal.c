#include <setjmp.h>
#include <stdio.h>

static jmp_buf primary;
static jmp_buf outer;
static jmp_buf inner;
static volatile int visits;

static void cross_frame(int depth, int value) {
    ++visits;
    if (depth != 0)
        cross_frame(depth - 1, value);
    else
        longjmp(primary, value);
}

static int zero_becomes_one(void) {
    volatile int preserved = 7;
    switch (setjmp(primary)) {
    case 0:
        preserved = 19;
        cross_frame(3, 0);
        return 1;
    case 1:
        return preserved != 19 || visits != 4;
    default:
        return 1;
    }
}

static int nested_environments(void) {
    volatile int preserved = 31;
    switch (setjmp(outer)) {
    case 0: {
        volatile int inner_preserved = 11;
        switch (setjmp(inner)) {
        case 0:
            inner_preserved = 23;
            longjmp(inner, 5);
        case 5:
            if (inner_preserved != 23)
                return 1;
            break;
        default:
            return 1;
        }
        preserved = 47;
        longjmp(outer, 9);
    }
    case 9:
        return preserved != 47;
    default:
        return 1;
    }
}

int main(void) {
    if (zero_becomes_one() || nested_environments())
        return 1;
    puts("Native nonlocal jumps passed");
    return 0;
}
