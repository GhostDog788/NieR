unsigned first(void);
unsigned second(void);
int main(void) { return first() == sizeof(void *) && second() == 7 ? 0 : 1; }
