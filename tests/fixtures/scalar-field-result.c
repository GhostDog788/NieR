#include <stddef.h>
#include <stdlib.h>

struct PointerSlot { void *value; };
struct IntegerSlot { long value; };
static int sentinel = 41;

__attribute__((noinline)) static void *scalar_pointer(void) { return &sentinel; }
__attribute__((noinline)) static long scalar_integer(void) { return 43; }

int main(void) {
    struct PointerSlot pointer;
    struct PointerSlot external;
    struct PointerSlot indirect;
    struct IntegerSlot integer;
    struct IntegerSlot indirect_integer;
    void *(*volatile pointer_call)(void) = scalar_pointer;
    long (*volatile integer_call)(void) = scalar_integer;
    pointer.value = scalar_pointer();
    integer.value = scalar_integer();
    external.value = malloc(sizeof(int));
    if (external.value == NULL)
        return 1;
    *(int *)external.value = 47;
    indirect.value = pointer_call();
    indirect_integer.value = integer_call();
    int failed = pointer.value != &sentinel || integer.value != 43 ||
        *(int *)external.value != 47 || indirect.value != &sentinel ||
        indirect_integer.value != 43;
    free(external.value);
    return failed;
}
