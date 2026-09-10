extern unsigned nier_build_width(void);
int main(void) { return nier_build_width() == sizeof(void *) ? 0 : 1; }
