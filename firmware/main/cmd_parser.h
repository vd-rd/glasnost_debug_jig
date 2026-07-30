#pragma once

#include <stddef.h>

void cmd_parser_init(void);
void cmd_parser_handle_line(const char *line, char *response, size_t response_size);
