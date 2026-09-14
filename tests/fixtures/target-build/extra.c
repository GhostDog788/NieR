#include <math.h>
static volatile double value = 0.0;
int extra(void) { return cos(value) == 1.0 ? 42 : 0; }
