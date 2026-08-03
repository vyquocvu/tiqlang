#ifndef TIQ_RUNTIME_PRELUDE_H
#define TIQ_RUNTIME_PRELUDE_H

// Core Language Runtime Prelude
//
// ARCHITECTURAL BOUNDARY:
// Contains ONLY essential primitive runtime definitions, scalar types, allocation
// helpers, and slice headers required by the Tiq core language semantics.
// All auxiliary system, networking, and serialization stubs have been moved
// to `runtime_aux.h` and will be rewritten natively in Tiq (`std/*.tiq`) in M19.

static const char TIQ_CORE_RUNTIME_PRELUDE[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include <strings.h>\n"
    "#include <stdint.h>\n"
    "#include <sys/stat.h>\n"
    "#include <sys/socket.h>\n"
    "#include <netdb.h>\n"
    "#include <netinet/in.h>\n"
    "#include <sys/event.h>\n"
    "#include <dirent.h>\n"
    "#include <unistd.h>\n"
    "#include <time.h>\n"
    "#include <dlfcn.h>\n"
    "typedef struct { const void *ptr; int len; } TiqSlice;\n"
    "typedef struct { int64_t value; int has_value; } TiqOption;\n"
    "typedef struct { int64_t value; int64_t error; int is_ok; } TiqResult;\n\n"

    "static void *tiq_alloc(size_t n) {\n"
    "    void *p = malloc(n);\n"
    "    if (!p) { fprintf(stderr, \"tiq: out of memory\\n\"); exit(1); }\n"
    "    return p;\n"
    "}\n\n"

    "static const char *tiq_str_dup(const char *s) {\n"
    "    size_t n = strlen(s);\n"
    "    char *b = (char *)tiq_alloc(n + 1);\n"
    "    memcpy(b, s, n + 1);\n"
    "    return b;\n"
    "}\n\n"

    "static int64_t tiq_argc = 0;\n"
    "static char **tiq_argv = 0;\n\n";

#endif
