#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIQ_VERSION "0.1.0-dev"

static void die(const char *message) {
    fprintf(stderr, "tiq: %s\n", message);
    exit(1);
}

static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *data;

    if (file == NULL) {
        fprintf(stderr, "tiq: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        die("cannot measure source file");
    }

    data = malloc((size_t)size + 1U);
    if (data == NULL) {
        fclose(file);
        die("out of memory");
    }
    if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        die("cannot read source file");
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

static void emit_c_string(FILE *out, const char *start, size_t length) {
    size_t i;
    fputc('"', out);
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)start[i];
        switch (ch) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (ch < 32U || ch == 127U) {
                    fprintf(out, "\\x%02x", ch);
                } else {
                    fputc((int)ch, out);
                }
        }
    }
    fputc('"', out);
}

static const char *skip_space(const char *p, int *line) {
    for (;;) {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            if (*p == '\n') (*line)++;
            p++;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p != '\0' && *p != '\n') p++;
            continue;
        }
        return p;
    }
}

static void compile_to_c(const char *source_path, const char *source, FILE *out) {
    const char *p = source;
    int line = 1;

    fputs("#include <stdio.h>\n\nint main(void) {\n", out);
    p = skip_space(p, &line);

    while (*p != '\0') {
        const char *start;
        const char *end;

        if (*p != '!') {
            fprintf(stderr, "%s:%d: error: expected print statement starting with '!'\n", source_path, line);
            exit(1);
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') {
            fprintf(stderr, "%s:%d: error: bootstrap compiler expects a string literal after '!'\n", source_path, line);
            exit(1);
        }
        p++;
        start = p;
        while (*p != '\0' && *p != '"') {
            if (*p == '\n') {
                fprintf(stderr, "%s:%d: error: newline in string literal\n", source_path, line);
                exit(1);
            }
            if (*p == '\\' && p[1] != '\0') p++;
            p++;
        }
        if (*p != '"') {
            fprintf(stderr, "%s:%d: error: unterminated string literal\n", source_path, line);
            exit(1);
        }
        end = p;
        p++;

        fputs("    fputs(", out);
        emit_c_string(out, start, (size_t)(end - start));
        fputs(", stdout);\n    fputc('\\n', stdout);\n", out);

        p = skip_space(p, &line);
    }

    fputs("    return 0;\n}\n", out);
}

static int emit_file(const char *input, const char *output) {
    char *source = read_all(input);
    FILE *out = output == NULL ? stdout : fopen(output, "wb");

    if (out == NULL) {
        free(source);
        fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno));
        return 1;
    }
    compile_to_c(input, source, out);
    if (output != NULL) fclose(out);
    free(source);
    return 0;
}

static int build(const char *input, const char *output) {
    const char *cc = getenv("CC");
    char temp_name[L_tmpnam];
    char command[4096];
    int status;

    if (cc == NULL || *cc == '\0') cc = "cc";
    if (tmpnam(temp_name) == NULL) die("cannot create temporary path");
    if (emit_file(input, temp_name) != 0) return 1;

    if (snprintf(command, sizeof(command), "%s -std=c11 -Os \"%s\" -o \"%s\"", cc, temp_name, output) >= (int)sizeof(command)) {
        remove(temp_name);
        die("compiler command is too long");
    }
    status = system(command);
    remove(temp_name);
    if (status != 0) {
        fprintf(stderr, "tiq: host C compiler failed\n");
        return 1;
    }
    return 0;
}

static void usage(FILE *out) {
    fputs("usage:\n"
          "  tiq --version\n"
          "  tiq emit-c <file.tiq>\n"
          "  tiq build <file.tiq> [-o output]\n", out);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("tiq %s\n", TIQ_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "emit-c") == 0) {
        return emit_file(argv[2], NULL);
    }
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        const char *output = "a.out";
        if (argc == 5 && strcmp(argv[3], "-o") == 0) output = argv[4];
        else if (argc != 3) {
            usage(stderr);
            return 2;
        }
        return build(argv[2], output);
    }
    usage(stderr);
    return 2;
}
