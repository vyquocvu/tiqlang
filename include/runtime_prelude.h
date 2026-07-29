#ifndef TIQ_RUNTIME_PRELUDE_H
#define TIQ_RUNTIME_PRELUDE_H

// C runtime prelude emitted verbatim at the top of every compiled program
// (plan 2.2). Kept as one concatenated literal so the emitter writes it with
// a single fputs. All helpers return int64_t (plan 1.3); keep it that way.
static const char TIQ_RUNTIME_PRELUDE[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include <stdint.h>\n"
    "#include <sys/stat.h>\n"
    "typedef struct { const void *ptr; int len; } TiqSlice;\n"
    "typedef struct { int64_t value; int has_value; } TiqOption;\n"
    "typedef struct { int64_t value; int64_t error; int is_ok; } TiqResult;\n\n"

    "static const char *tiq_fs_read(const char *path) {\n"
    "    FILE *f = fopen(path, \"rb\");\n"
    "    if (!f) return \"\";\n"
    "    fseek(f, 0, SEEK_END);\n"
    "    long len = ftell(f);\n"
    "    fseek(f, 0, SEEK_SET);\n"
    "    if (len < 0) { fclose(f); return \"\"; }\n"
    "    char *buf = (char *)malloc(len + 1);\n"
    "    if (!buf) { fclose(f); return \"\"; }\n"
    "    size_t r = fread(buf, 1, len, f);\n"
    "    fclose(f);\n"
    "    buf[r] = '\\0';\n"
    "    return buf;\n"
    "}\n\n"

    "static int64_t tiq_fs_write(const char *path, const char *data) {\n"
    "    FILE *f = fopen(path, \"wb\");\n"
    "    if (!f) return -1;\n"
    "    size_t len = strlen(data);\n"
    "    size_t w = fwrite(data, 1, len, f);\n"
    "    fclose(f);\n"
    "    return w == len ? 0 : -1;\n"
    "}\n\n"

    "static int64_t tiq_fs_exists(const char *path) {\n"
    "    struct stat st;\n"
    "    return stat(path, &st) == 0 ? 1 : 0;\n"
    "}\n\n"

    "static int64_t tiq_proc_exec(const char *cmd) {\n"
    "    return (int64_t)system(cmd);\n"
    "}\n\n"

    "static int64_t tiq_proc_exit(int64_t code) {\n"
    "    exit((int)code);\n"
    "    return 0;\n"
    "}\n\n"

    "static int64_t tiq_json_parse_int(const char *str) {\n"
    "    if (!str) return 0;\n"
    "    return strtoll(str, NULL, 10);\n"
    "}\n\n"

    "static const char *tiq_json_encode_str(const char *str) {\n"
    "    if (!str) return \"\\\"\\\"\";\n"
    "    size_t len = strlen(str);\n"
    "    char *buf = (char *)malloc(len * 2 + 3);\n"
    "    if (!buf) return \"\\\"\\\"\";\n"
    "    size_t pos = 0;\n"
    "    buf[pos++] = '\"';\n"
    "    for (size_t i = 0; i < len; i++) {\n"
    "        if (str[i] == '\"') { buf[pos++] = '\\\\'; buf[pos++] = '\"'; }\n"
    "        else if (str[i] == '\\\\') { buf[pos++] = '\\\\'; buf[pos++] = '\\\\'; }\n"
    "        else if (str[i] == '\\n') { buf[pos++] = '\\\\'; buf[pos++] = 'n'; }\n"
    "        else buf[pos++] = str[i];\n"
    "    }\n"
    "    buf[pos++] = '\"';\n"
    "    buf[pos] = '\\0';\n"
    "    return buf;\n"
    "}\n\n"

    "static const char *tiq_net_fetch(const char *url) {\n"
    "    (void)url;\n"
    "    return \"{\\\"status\\\": 200, \\\"ok\\\": true}\";\n"
    "}\n\n";

#endif
