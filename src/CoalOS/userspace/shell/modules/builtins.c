/**
 * @file builtins.c
 * @brief Built-in commands
 */

#include "builtins.h"
#include "env_vars.h"

static int builtin_cd(char **args);
static int builtin_pwd(char **args);
static int builtin_exit(char **args);
static int builtin_echo(char **args);
static int builtin_clear(char **args);
static int builtin_history(char **args);
static int builtin_help(char **args);
static int builtin_kill(char **args);
static int builtin_cat(char **args);
static int builtin_mkdir(char **args);
static int builtin_rmdir(char **args);
static int builtin_rm(char **args);
static int builtin_touch(char **args);

static const builtin_cmd_t builtin_commands[] = {
    {"cd", builtin_cd, "Change directory (cd - goes back)"},
    {"pwd", builtin_pwd, "Print working directory"},
    {"exit", builtin_exit, "Exit the shell"},
    {"echo", builtin_echo, "Print arguments (-n: no newline)"},
    {"clear", builtin_clear, "Clear the screen"},
    {"history", builtin_history, "Show command history"},
    {"help", builtin_help, "Show this list"},
    {"kill", builtin_kill, "Send a signal: kill [-SIGNAL] pid"},
    {"cat", builtin_cat, "Print a file"},
    {"mkdir", builtin_mkdir, "Create a directory"},
    {"rmdir", builtin_rmdir, "Remove an empty directory"},
    {"rm", builtin_rm, "Remove a file"},
    {"touch", builtin_touch, "Create an empty file"},
    {"export", builtin_export, "Set a variable: export NAME=value"},
    {"unset", builtin_unset, "Remove a variable"},
    {"env", builtin_env, "Show all variables"},
    {NULL, NULL, NULL}
};

const builtin_cmd_t *find_builtin(const char *name) {
    for (const builtin_cmd_t *b = builtin_commands; b->name; b++) {
        if (strcmp(name, b->name) == 0) return b;
    }
    return NULL;
}

/* Parse a whole string as a decimal number. Returns false if it is not one. */
static bool parse_int(const char *s, long *value) {
    char *end;
    if (*s == '\0') return false;
    *value = strtol(s, &end, 10);
    return *end == '\0';
}

static int builtin_cd(char **args) {
    const char *path;
    char previous[MAX_PATH_LENGTH];

    if (!args[1]) {
        path = getenv("HOME");
        if (!path) path = "/";
    } else if (strcmp(args[1], "-") == 0) {
        if (g_shell.last_directory[0] == '\0') {
            shell_error("cd: no previous directory");
            return 1;
        }
        path = g_shell.last_directory;
    } else {
        path = args[1];
    }

    /* Remember where we were, but only keep it if the change works. */
    if (!getcwd(previous, sizeof(previous))) previous[0] = '\0';

    if (chdir(path) < 0) {
        shell_error("cd: no such directory");
        return 1;
    }

    strncpy(g_shell.last_directory, previous, sizeof(g_shell.last_directory) - 1);
    g_shell.last_directory[sizeof(g_shell.last_directory) - 1] = '\0';
    if (getcwd(previous, sizeof(previous))) setenv("PWD", previous, 1);
    return 0;
}

static int builtin_pwd(char **args) {
    char cwd[MAX_PATH_LENGTH];
    (void)args;
    if (!getcwd(cwd, sizeof(cwd))) {
        shell_error("pwd: cannot get current directory");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

static int builtin_exit(char **args) {
    long code = g_shell.last_exit_status;
    if (args[1] && !parse_int(args[1], &code)) {
        shell_error("exit: numeric argument required");
        return 1;
    }
    g_shell.running = false;
    g_shell.last_exit_status = (int)code;
    return (int)code;
}

static int builtin_echo(char **args) {
    bool newline = true;
    int i = 1;

    if (args[1] && strcmp(args[1], "-n") == 0) {
        newline = false;
        i = 2;
    }
    for (bool first = true; args[i]; i++, first = false) {
        if (!first) putchar(' ');
        write(STDOUT_FILENO, args[i], strlen(args[i]));
    }
    if (newline) putchar('\n');
    return 0;
}

static int builtin_clear(char **args) {
    (void)args;
    printf("\033[2J\033[H");
    return 0;
}

static int builtin_history(char **args) {
    (void)args;
    for (int i = 0; i < g_shell.history_count; i++) {
        printf("%d  %s\n", i + 1, g_shell.history[i]);
    }
    return 0;
}

static int builtin_help(char **args) {
    (void)args;
    printf("Built-in commands:\n");
    for (const builtin_cmd_t *b = builtin_commands; b->name; b++) {
        printf("  %-8s %s\n", b->name, b->help);
    }
    printf("\nAlso: cmd < in > out, cmd >> out, cmd &, $VAR, ${VAR}, $?, cmd1; cmd2\n"
           "Pipes parse but cannot run yet. Other names run /bin/NAME.elf.\n");
    return 0;
}

static int builtin_kill(char **args) {
    int sig = SIGTERM;
    int i = 1;
    long pid;

    if (args[1] && args[1][0] == '-') {
        long s;
        if (!parse_int(args[1] + 1, &s) || s <= 0) {
            shell_error("kill: invalid signal");
            return 1;
        }
        sig = (int)s;
        i = 2;
    }
    if (!args[i]) {
        shell_error("kill: missing pid");
        return 1;
    }
    if (!parse_int(args[i], &pid) || pid <= 0) {
        shell_error("kill: invalid pid");
        return 1;
    }
    if (kill((pid_t)pid, sig) < 0) {
        shell_error("kill: cannot send signal");
        return 1;
    }
    return 0;
}

static int builtin_cat(char **args) {
    char buffer[512];
    ssize_t n;

    if (!args[1]) {
        shell_error("cat: missing file operand");
        return 1;
    }
    int fd = open(args[1], O_RDONLY);
    if (fd < 0) {
        shell_error("cat: cannot open file");
        return 1;
    }
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, (size_t)n);
    }
    close(fd);
    if (n < 0) {
        shell_error("cat: read error");
        return 1;
    }
    return 0;
}

static int builtin_mkdir(char **args) {
    if (!args[1]) {
        shell_error("mkdir: missing directory name");
        return 1;
    }
    if (mkdir(args[1], 0755) < 0) {
        shell_error("mkdir: cannot create directory");
        return 1;
    }
    return 0;
}

static int builtin_rmdir(char **args) {
    if (!args[1]) {
        shell_error("rmdir: missing directory name");
        return 1;
    }
    if (rmdir(args[1]) < 0) {
        shell_error("rmdir: cannot remove directory");
        return 1;
    }
    return 0;
}

static int builtin_rm(char **args) {
    if (!args[1]) {
        shell_error("rm: missing file operand");
        return 1;
    }
    if (unlink(args[1]) < 0) {
        shell_error("rm: cannot remove file");
        return 1;
    }
    return 0;
}

static int builtin_touch(char **args) {
    if (!args[1]) {
        shell_error("touch: missing file operand");
        return 1;
    }
    int fd = open(args[1], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        shell_error("touch: cannot create file");
        return 1;
    }
    close(fd);
    return 0;
}
