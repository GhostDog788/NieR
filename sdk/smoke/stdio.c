#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

size_t sdk_pointer_width(void) { return sizeof(void *); }

int main(void) {
  char *buffer = malloc(32);
  if (buffer == NULL) return 2;
  int result = snprintf(buffer, 32, "SDK smoke: pointer=%zu", sdk_pointer_width());
  if (result < 0 || result >= 32) { free(buffer); return 3; }
  puts(buffer);
  free(buffer);
  return 0;
}
