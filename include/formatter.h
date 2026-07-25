#ifndef TIQ_FORMATTER_H
#define TIQ_FORMATTER_H

#include <stdbool.h>

typedef struct {
    bool use_tabs;
    int indent_width;
    int max_line_length;
    bool insert_final_newline;
} FormatterOptions;

void formatter_init_options(FormatterOptions *opts);
int format_file(const char *input, const char *output, FormatterOptions *opts);
int format_stdin_to_file(const char *output, FormatterOptions *opts);

#endif
