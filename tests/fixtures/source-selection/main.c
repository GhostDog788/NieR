extern unsigned selected_width(void);
int main(void) { return selected_width() == sizeof(void *) ? 0 : 1; }
