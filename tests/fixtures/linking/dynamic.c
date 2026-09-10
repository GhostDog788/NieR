#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *library = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!library) return 1;
    int (*print)(const char *) = (int (*)(const char *))dlsym(library, "puts");
    if (!print) return 2;
    if (print("Ordinary dlopen passed") < 0) return 3;
    return dlclose(library) != 0;
}
