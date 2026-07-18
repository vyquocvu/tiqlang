#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int compile_file_to_c_stream(const char *input, FILE *out) {
    char *source = read_all(input);

    compile_to_c(input, source, out);
    free(source);
    if (ferror(out)) {
        fprintf(stderr, "tiq: cannot write generated C: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int emit_file(const char *input, const char *output) {
    FILE *out = output == NULL ? stdout : fopen(output, "wb");
    int result;

    if (out == NULL) {
        fprintf(stderr, "tiq: cannot create %s: %s\n", output, strerror(errno));
        return 1;
    }
    result = compile_file_to_c_stream(input, out);
    if (output != NULL && fclose(out) != 0) {
        fprintf(stderr, "tiq: cannot close %s: %s\n", output, strerror(errno));
        return 1;
    }
    if (output == NULL && fflush(out) != 0) {
        fprintf(stderr, "tiq: cannot flush generated C: %s\n", strerror(errno));
        return 1;
    }
    if (result != 0) return 1;
    return 0;
}

static int run_host_compiler(const char *cc, const char *source_path, const char *output_path) {
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        fprintf(stderr, "tiq: cannot start host C compiler: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        char *const args[] = {
            (char *)cc,
            (char *)"-std=c11",
            (char *)"-Os",
            (char *)"-x",
            (char *)"c",
            (char *)source_path,
            (char *)"-o",
            (char *)output_path,
            NULL
        };
        execvp(cc, args);
        fprintf(stderr, "tiq: cannot execute host C compiler %s: %s\n", cc, strerror(errno));
        _exit(127);
    }

    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) {
            fprintf(stderr, "tiq: cannot wait for host C compiler: %s\n", strerror(errno));
            return 1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "tiq: host C compiler failed\n");
        return 1;
    }
    return 0;
}

static char *temporary_c_template(void) {
    const char *dir = getenv("TMPDIR");
    const char *suffix = "tiq-c-XXXXXX";
    size_t dir_len;
    size_t suffix_len = strlen(suffix);
    int need_slash;
    char *path;

    if (dir == NULL || *dir == '\0') dir = "/tmp";
    dir_len = strlen(dir);
    need_slash = dir_len > 0U && dir[dir_len - 1U] != '/';
    path = malloc(dir_len + (need_slash ? 1U : 0U) + suffix_len + 1U);
    if (path == NULL) die("out of memory");
    memcpy(path, dir, dir_len);
    if (need_slash) {
        path[dir_len] = '/';
        dir_len++;
    }
    memcpy(path + dir_len, suffix, suffix_len + 1U);
    return path;
}

static int build(const char *input, const char *output) {
    const char *cc = getenv("CC");
    char *temp_name = temporary_c_template();
    int fd;
    FILE *temp_file;
    int result;

    if (cc == NULL || *cc == '\0') cc = "cc";
    fd = mkstemp(temp_name);
    if (fd < 0) {
        fprintf(stderr, "tiq: cannot create temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    temp_file = fdopen(fd, "wb");
    if (temp_file == NULL) {
        remove(temp_name);
        close(fd);
        fprintf(stderr, "tiq: cannot open temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    result = compile_file_to_c_stream(input, temp_file);
    if (fclose(temp_file) != 0) {
        remove(temp_name);
        fprintf(stderr, "tiq: cannot close temporary C file: %s\n", strerror(errno));
        free(temp_name);
        return 1;
    }
    if (result != 0) {
        remove(temp_name);
        free(temp_name);
        return 1;
    }

    result = run_host_compiler(cc, temp_name, output);
    if (remove(temp_name) != 0) {
        fprintf(stderr, "tiq: cannot remove temporary C file %s: %s\n", temp_name, strerror(errno));
        free(temp_name);
        return 1;
    }
    free(temp_name);
    return result;
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
