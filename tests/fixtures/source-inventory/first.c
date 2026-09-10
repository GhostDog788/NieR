#if defined(NIER_CROSS_FRAGMENT)
unsigned second(unsigned value);
unsigned first(void) { return second(sizeof(void *)); }
#elif defined(NIER_PRIVATE_IDENTITY)
static unsigned private_width(void) { return sizeof(void *); }
unsigned first(void) { return private_width(); }
#else
unsigned first(void) { return sizeof(void *); }
#endif
