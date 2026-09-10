unsigned first(void);
unsigned second(unsigned value);
int main(void) {
#if defined(NIER_CROSS_FRAGMENT)
    return first() == sizeof(void *) + 7 && second(5) == 12 ? 0 : 1;
#else
    return first() == sizeof(void *) && second(5) == 12 ? 0 : 1;
#endif
}
