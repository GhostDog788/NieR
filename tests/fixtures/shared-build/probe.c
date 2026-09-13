extern unsigned sela_build_width(void);
int main(void) { return sela_build_width() == sizeof(void *) ? 0 : 1; }
