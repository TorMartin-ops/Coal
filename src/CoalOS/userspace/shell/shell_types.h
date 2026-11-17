/**
 * @file shell_types.h
 * @brief Shared constants and types for the Coal OS shell
 */

#ifndef SHELL_TYPES_H
#define SHELL_TYPES_H

#include <libc/stdbool.h>
#include <libc/stddef.h>
#include <libc/stdint.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/signal.h>

#define MAX_COMMAND_LENGTH 1024
#define MAX_ARGS 32          /* arguments per command, including the NULL */
#define MAX_COMMANDS 8       /* commands per pipeline */
#define MAX_HISTORY 100
#define MAX_PATH_LENGTH 256

/* Descriptors that hold the shell's own stdin/stdout while a built-in runs
 * with redirected I/O. The kernel allows 16 descriptors per process. */
#define SAVED_STDIN_FD  14
#define SAVED_STDOUT_FD 15

#define WNOHANG 1

/* Print "shell: msg" on stderr. */
#define shell_error(msg) fprintf(stderr, "shell: %s\n", (msg))

/* One command of a pipeline. The strings point into pipeline_t.buf. */
typedef struct command {
    char *args[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
    bool append_output;
} command_t;

/* A parsed command line up to the next ';'. Owns all strings it points to. */
typedef struct pipeline {
    command_t commands[MAX_COMMANDS];
    int num_commands;
    bool background;
    char buf[MAX_COMMAND_LENGTH + 1];
} pipeline_t;

typedef struct {
    bool running;
    int last_exit_status;
    int background_children;            /* children not yet reaped */
    char last_directory[MAX_PATH_LENGTH];
    char history[MAX_HISTORY][MAX_COMMAND_LENGTH];
    int history_count;
} shell_state_t;

extern shell_state_t g_shell;

#endif /* SHELL_TYPES_H */
