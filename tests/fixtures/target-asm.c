#include <stdint.h>

static unsigned native_add(unsigned value) {
#if defined(__aarch64__)
  __asm__("add %w0, %w0, #7" : "+r"(value));
#elif defined(__arm__)
  __asm__("add %0, %0, #7" : "+r"(value));
#else
  __asm__("addl $7, %0" : "+r"(value) : : "cc");
#endif
  return value;
}

int main(void) {
  volatile unsigned memory = 0;
  __asm__ volatile("" : "+m"(memory) : : "memory");
  if (native_add(35) != 42) return 1;
#if defined(__arm__) || defined(__aarch64__)
  __asm__ goto("b %l0" : : : : selected);
#else
  __asm__ goto("jmp %l0" : : : : selected);
#endif
  return 2;
selected:
  return 0;
}
