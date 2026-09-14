/* ABI/streaming checks against the unmodified upstream library, not a benchmark. */
#include <stdio.h>
#include <xxhash.h>

int main(void) {
  unsigned char data[4096];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = (unsigned char)(i * 13u + (i >> 3));
  XXH32_state_t *s32 = XXH32_createState();
  XXH64_state_t *s64 = XXH64_createState();
  XXH3_state_t *s3 = XXH3_createState();
  if (!s32 || !s64 || !s3) return 1;
  if (XXH32_reset(s32, 17) || XXH64_reset(s64, 17) ||
      XXH3_64bits_reset_withSeed(s3, 17)) return 2;
  for (unsigned i = 0; i < sizeof(data); i += 64)
    if (XXH32_update(s32, data + i, 64) || XXH64_update(s64, data + i, 64) ||
        XXH3_64bits_update(s3, data + i, 64)) return 3;
  XXH32_hash_t h32 = XXH32(data, sizeof(data), 17);
  XXH64_hash_t h64 = XXH64(data, sizeof(data), 17);
  XXH64_hash_t h3 = XXH3_64bits_withSeed(data, sizeof(data), 17);
  if (XXH32_digest(s32) != h32 || XXH64_digest(s64) != h64 ||
      XXH3_64bits_digest(s3) != h3) return 4;
  if (XXH3_128bits_reset_withSeed(s3, 17)) return 5;
  for (unsigned i = 0; i < sizeof(data); i += 64)
    if (XXH3_128bits_update(s3, data + i, 64)) return 6;
  XXH128_hash_t h128 = XXH3_128bits_withSeed(data, sizeof(data), 17);
  XXH128_hash_t streamed = XXH3_128bits_digest(s3);
  if (h128.low64 != streamed.low64 || h128.high64 != streamed.high64) return 7;
  printf("%08x %016llx %016llx %016llx%016llx\n", (unsigned)h32,
         (unsigned long long)h64, (unsigned long long)h3,
         (unsigned long long)h128.high64, (unsigned long long)h128.low64);
  XXH32_freeState(s32);
  XXH64_freeState(s64);
  XXH3_freeState(s3);
  return 0;
}
