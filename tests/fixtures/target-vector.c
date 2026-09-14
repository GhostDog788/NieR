#include <stdint.h>

typedef uint32_t lanes4 __attribute__((vector_size(16)));
static volatile lanes4 input = {1, 2, 3, 4};

static uint32_t vector_result(void) {
  lanes4 value = input;
  lanes4 reversed = __builtin_shufflevector(value, value, 3, 2, 1, 0);
  lanes4 sum = value + reversed;
  sum[1] = 17;
  return sum[0] + sum[1] + sum[2] + sum[3];
}

#if defined(__SIZEOF_INT128__)
static uint64_t wide_result(uint64_t x) {
  __uint128_t value = ((__uint128_t)0x123456789abcdef0ULL << 64) | x;
  return (uint64_t)((value >> 64) ^ value);
}
#endif

int main(void) {
  if (vector_result() != 32) return 1;
  unsigned overflow;
  if (!__builtin_add_overflow(UINT32_MAX, (unsigned)input[0], &overflow) || overflow != 0) return 3;
  if (__builtin_popcount((unsigned)input[3]) != 1) return 4;
  if (__builtin_clz((unsigned)input[3]) != 29) return 5;
#if defined(__SIZEOF_INT128__)
  if (wide_result(7) != (0x123456789abcdef0ULL ^ 7)) return 2;
#endif
  return 0;
}
