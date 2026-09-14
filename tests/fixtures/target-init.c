#include <stdio.h>
static int state;
__attribute__((constructor(200))) static void initialize(void) { state = 42; }
__attribute__((destructor(200))) static void finalize(void) { puts("finalized"); }
int main(void) {
  printf("initialized %d\n", state);
  return state != 42;
}
