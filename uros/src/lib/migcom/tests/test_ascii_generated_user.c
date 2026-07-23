#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int main(int argc, char **argv) {
    const char *fname = (argc > 1) ? argv[1] : "generated_user.c";
    FILE *f = fopen(fname, "r");
    if (!f) {
        perror("fopen");
        return 2;
    }
    int c, line = 1, col = 1;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            line++;
            col = 1;
            continue;
        }
        if ((c < 32 && c != 9) || c > 126) {
            fprintf(stderr, "Non-ASCII character 0x%02x at line %d, col %d\n", c, line, col);
            fclose(f);
            return 1;
        }
        col++;
    }
    fclose(f);
    return 0;
}
