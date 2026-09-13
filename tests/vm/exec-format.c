#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* A shell may retry ENOEXEC as a script. Observe the actual kernel error. */
int main(int argc, char **argv) {
    if (argc != 2 || sizeof(void *) != 4) return 2;
    char *const arguments[] = {argv[1], NULL};
    char *const environment[] = {NULL};
    execve(argv[1], arguments, environment);
    if (errno != ENOEXEC) {
        perror("ELF64 rejection did not return ENOEXEC");
        return 1;
    }
    puts("SELA_VM_ELF64_ENOEXEC");
    return 0;
}
