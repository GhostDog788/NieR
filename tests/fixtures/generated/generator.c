#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *output = fopen(argv[1], "w");
    if (!output) return 3;
    fprintf(output, "#define GENERATED_WIDTH %u\n", (unsigned)sizeof(void *));
    return fclose(output) != 0;
}
