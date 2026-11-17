/**
 * @file shell_main.c
 * @brief Coal OS shell: read-eval loop and command execution
 */

#include "shell_types.h"
#include <libc/syscall.h>
#include "modules/builtins.h"
#include "modules/env_vars.h"
#include "modules/parser.h"

#define SHELL_VERSION "0.1"

shell_state_t g_shell;
static pipeline_t g_pipeline;       /* too big for the stack, reused per command */

//============================================================================
// Prompt and input
//============================================================================

/* Append s to buf at *len, never writing past size - 1 bytes. */
static void put(char *buf, size_t size, size_t *len, const char *s) {
    while (*s && *len < size - 1) buf[(*len)++] = *s++;
}

static void show_prompt(void) {
    char prompt[256];
    size_t len = 0;
    const char *ps1 = getenv("PS1");
    if (!ps1) ps1 = "$ ";

    for (const char *p = ps1; *p; p++) {
        if (*p != '\\' || !p[1]) {
            char one[2] = { *p, '\0' };
            put(prompt, sizeof(prompt), &len, one);
            continue;
        }
        p++;
        switch (*p) {
            case 'w': {
                char cwd[MAX_PATH_LENGTH];
                put(prompt, sizeof(prompt), &len, getcwd(cwd, sizeof(cwd)) ? cwd : "?");
                break;
            }
            case 'u': {
                const char *user = getenv("USER");
                if (user) put(prompt, sizeof(prompt), &len, user);
                break;
            }
            case 'h': put(prompt, sizeof(prompt), &len, "coal"); break;
            case '$': put(prompt, sizeof(prompt), &len, "$"); break;
            default: {
                char one[2] = { *p, '\0' };
                put(prompt, sizeof(prompt), &len, one);
            }
        }
    }
    prompt[len] = '\0';
    write(STDOUT_FILENO, prompt, len);
}

/*
 * Read one line. Uses the terminal line-read system call and falls back to a
 * plain read() on stdin if the kernel does not provide it.
 * Returns the line length, or -1 at end of input or on a persistent error.
 */
static int read_command(char *buffer, size_t size) {
    static int errors = 0;
    int n = syscall(SYS_READ_TERMINAL_LINE, (int)(uintptr_t)buffer, (int)size - 1, 0);
    if (n < 0) n = (int)read(STDIN_FILENO, buffer, size - 1);

    if (n < 0) {
        if (++errors >= 3) return -1;   /* do not spin on a broken terminal */
        buffer[0] = '\0';
        return 0;
    }
    errors = 0;
    if (n == 0) return -1;              /* end of input */

    buffer[n] = '\0';
    if (n > 0 && buffer[n - 1] == '\n') buffer[n - 1] = '\0';
    return n;
}

static void add_to_history(const char *command) {
    if (g_shell.history_count > 0 &&
        strcmp(g_shell.history[g_shell.history_count - 1], command) == 0) {
        return;
    }
    if (g_shell.history_count == MAX_HISTORY) {
        memmove(g_shell.history[0], g_shell.history[1],
                (MAX_HISTORY - 1) * sizeof(g_shell.history[0]));
        g_shell.history_count--;
    }
    strncpy(g_shell.history[g_shell.history_count], command, MAX_COMMAND_LENGTH - 1);
    g_shell.history[g_shell.history_count][MAX_COMMAND_LENGTH - 1] = '\0';
    g_shell.history_count++;
}

//============================================================================
// Redirection
//============================================================================

/* Put back the shell's own stdin/stdout after a redirected built-in. */
static void redirect_end(const int saved[2]) {
    if (saved[0] >= 0) {
        dup2(saved[0], STDIN_FILENO);
        close(saved[0]);
    }
    if (saved[1] >= 0) {
        dup2(saved[1], STDOUT_FILENO);
        close(saved[1]);
    }
}

/* Make fd (opened with flags) the new fd target. */
static int redirect_one(const char *path, int flags, int target, int save_as, int *saved) {
    int fd = open(path, flags, 0644);
    if (fd < 0) return -1;
    if (save_as >= 0) {
        if (dup2(target, save_as) < 0) {
            close(fd);
            return -1;
        }
        *saved = save_as;
    }
    if (dup2(fd, target) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * Apply the redirections of cmd. If save is true the old stdin/stdout are kept
 * in saved[] so that redirect_end can restore them. On failure everything
 * done so far is undone.
 */
static int redirect_begin(const command_t *cmd, bool save, int saved[2]) {
    saved[0] = saved[1] = -1;

    if (cmd->input_file &&
        redirect_one(cmd->input_file, O_RDONLY, STDIN_FILENO,
                     save ? SAVED_STDIN_FD : -1, &saved[0]) < 0) {
        shell_error("cannot open input file");
        redirect_end(saved);
        return -1;
    }
    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_output ? O_APPEND : O_TRUNC);
        if (redirect_one(cmd->output_file, flags, STDOUT_FILENO,
                         save ? SAVED_STDOUT_FD : -1, &saved[1]) < 0) {
            shell_error("cannot open output file");
            redirect_end(saved);
            return -1;
        }
    }
    return 0;
}

//============================================================================
// Execution
//============================================================================

static int run_builtin(const builtin_cmd_t *builtin, command_t *cmd) {
    int saved[2];
    if (redirect_begin(cmd, true, saved) < 0) return 1;
    int status = builtin->function(cmd->args);
    redirect_end(saved);
    return status;
}

/* Try to execute name, then /bin/name, then /bin/name.elf. Returns only on failure. */
static void exec_program(char **args) {
    char path[MAX_PATH_LENGTH];

    execve(args[0], args, environ);
    if (strchr(args[0], '/')) return;

    if (snprintf(path, sizeof(path), "/bin/%s", args[0]) < (int)sizeof(path) - 1) {
        execve(path, args, environ);
    }
    if (snprintf(path, sizeof(path), "/bin/%s.elf", args[0]) < (int)sizeof(path) - 1) {
        execve(path, args, environ);
    }
}

static int run_external(command_t *cmd, bool background) {
    int saved[2];
    pid_t pid = fork();
    if (pid < 0) {
        shell_error("fork failed");
        return 1;
    }

    if (pid == 0) {
        if (redirect_begin(cmd, false, saved) < 0) _exit(1);
        exec_program(cmd->args);
        shell_error("command not found");
        _exit(127);
    }

    if (background) {
        g_shell.background_children++;
        printf("[bg] %d\n", pid);
        return 0;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    return status;
}

static int run_pipeline(pipeline_t *pl) {
    if (pl->num_commands == 0) return g_shell.last_exit_status;
    if (pl->num_commands > 1) {
        shell_error("pipes are not supported yet");
        return 1;
    }

    command_t *cmd = &pl->commands[0];
    const builtin_cmd_t *builtin = find_builtin(cmd->args[0]);
    if (builtin) return run_builtin(builtin, cmd);
    return run_external(cmd, pl->background);
}

/* Run every ';' separated command on the line. */
static int execute_line(const char *line) {
    int status = 0;
    while (*line) {
        const char *rest;
        if (parse_pipeline(line, &g_pipeline, &rest) < 0) {
            shell_error("syntax error");
            return 1;
        }
        status = run_pipeline(&g_pipeline);
        g_shell.last_exit_status = status;
        if (!g_shell.running) break;
        line = rest;
    }
    return status;
}

static void reap_children(void) {
    int status;
    while (g_shell.background_children > 0 && waitpid(-1, &status, WNOHANG) > 0) {
        g_shell.background_children--;
    }
}

//============================================================================
// Main
//============================================================================

int main(int argc, char **argv) {
    static char input[MAX_COMMAND_LENGTH];
    static char expanded[MAX_COMMAND_LENGTH];
    (void)argc;
    (void)argv;

    g_shell.running = true;
    env_init();

    printf("Coal OS shell " SHELL_VERSION "\nType 'help' for the built-in commands.\n\n");

    while (g_shell.running) {
        reap_children();
        show_prompt();

        if (read_command(input, sizeof(input)) < 0) break;

        char *line = trim_whitespace(input);
        if (*line == '\0') continue;

        add_to_history(line);
        expand_variables(line, expanded, sizeof(expanded));
        g_shell.last_exit_status = execute_line(expanded);
    }

    return g_shell.last_exit_status;
}
