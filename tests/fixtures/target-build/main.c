#include <stdint.h>
#include <stdio.h>
#if UINTPTR_MAX == UINT32_MAX
extern int extra(void);
#endif
int main(void) {
#if UINTPTR_MAX == UINT32_MAX
  if (LANE_VALUE != 13 || extra() != 42) return 1;
#else
  if (LANE_VALUE != 17) return 2;
#endif
  puts("target build passed");
  return 0;
}
