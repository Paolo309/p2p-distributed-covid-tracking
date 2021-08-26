#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include <stdlib.h>
#include <stdio.h>
#include <wordexp.h>
#include <string.h>

#define MAX_CMD_LEN 128

char **get_command_line(int *argc);
void free_command_line(int argc, char **argv);

#endif
