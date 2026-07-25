#ifndef TIQ_LSP_H
#define TIQ_LSP_H

#include <stdbool.h>

int lsp_server_run(const char *root_path, int stdin_fd, int stdout_fd);
int lsp_server_init(const char *root_path);
void lsp_server_shutdown(void);

#endif
