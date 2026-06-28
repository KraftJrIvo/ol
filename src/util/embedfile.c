#include <stdio.h>
#include <stdlib.h>

static FILE* open_or_exit(const char* fname, const char* mode)
{
    FILE* f = fopen(fname, mode);
    if (f == NULL) {
        perror(fname);
        exit(EXIT_FAILURE);
    }
    return f;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "USAGE: %s {sym} {rsrc}\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* sym = argv[1];
    FILE* in = open_or_exit(argv[2], "rb");

    char symfile[256];
    snprintf(symfile, sizeof(symfile), "%s.c", sym);
    FILE* out = open_or_exit(symfile, "w");

    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "const unsigned char %s[] = {\n", sym);

    unsigned char buf[256];
    size_t line_count = 0;
    size_t nread = 0;
    do {
        nread = fread(buf, 1, sizeof(buf), in);
        for (size_t i = 0; i < nread; ++i) {
            fprintf(out, "0x%02x, ", buf[i]);
            if (++line_count == 10) {
                fprintf(out, "\n");
                line_count = 0;
            }
        }
    } while (nread > 0);

    if (line_count > 0) fprintf(out, "\n");
    fprintf(out, "0x00};\n");
    fprintf(out, "const unsigned long long %s_len = sizeof(%s);\n", sym, sym);

    fclose(in);
    fclose(out);
    return EXIT_SUCCESS;
}
