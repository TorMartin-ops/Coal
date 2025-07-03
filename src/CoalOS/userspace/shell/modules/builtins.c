/**
 * @file builtins.c
 * @brief Built-in commands implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "builtins.h"
#include "string_utils.h"
#include "io_utils.h"
#include "syscall_wrapper.h"
#include "env_vars.h"

// Import global shell state
extern shell_state_t g_shell;

//============================================================================
// Built-in Command Table
//============================================================================

static const builtin_cmd_t builtin_commands[] = {
    {"cd", builtin_cd, "Change directory"},
    {"pwd", builtin_pwd, "Print working directory"},
    {"exit", builtin_exit, "Exit shell"},
    {"echo", builtin_echo, "Echo arguments"},
    {"clear", builtin_clear, "Clear screen"},
    {"history", builtin_history, "Show command history"},
    {"help", builtin_help, "Show help"},
    {"jobs", builtin_jobs, "Show jobs"},
    {"fg", builtin_fg, "Bring job to foreground"},
    {"bg", builtin_bg, "Send job to background"},
    {"kill", builtin_kill, "Kill a process"},
    {"ls", builtin_ls, "List directory contents"},
    {"cat", builtin_cat, "Display file contents"},
    {"mkdir", builtin_mkdir, "Create directory"},
    {"rmdir", builtin_rmdir, "Remove directory"},
    {"rm", builtin_rm, "Remove file"},
    {"touch", builtin_touch, "Create empty file"},
    {"export", builtin_export, "Set environment variable"},
    {"unset", builtin_unset, "Unset environment variable"},
    {"env", builtin_env, "Show environment variables"},
    {NULL, NULL, NULL}
};

//============================================================================
// Built-in Command Implementations
//============================================================================

int builtin_cd(char **args) {
    const char *path;
    
    if (!args[1]) {
        // No argument - go to home directory
        path = get_env("HOME");
        if (!path) path = "/";
    } else if (my_strcmp(args[1], "-") == 0) {
        // cd - : go to previous directory
        path = g_shell.last_directory;
        if (!path || path[0] == '\0') {
            error("cd: no previous directory");
            return 1;
        }
    } else {
        path = args[1];
    }
    
    // Save current directory as last directory
    my_strcpy(g_shell.last_directory, g_shell.cwd);
    
    // Change directory
    if (sys_chdir(path) < 0) {
        error("cd: failed to change directory");
        return 1;
    }
    
    // Update current working directory
    if (sys_getcwd(g_shell.cwd, sizeof(g_shell.cwd)) == 0) {
        error("cd: failed to get current directory");
        return 1;
    }
    
    // Update PWD environment variable
    set_env("PWD", g_shell.cwd);
    
    return 0;
}

int builtin_pwd(char **args) {
    (void)args;
    print_str(g_shell.cwd);
    print_str("\n");
    return 0;
}

int builtin_exit(char **args) {
    int exit_code = 0;
    
    if (args[1]) {
        // Parse exit code
        exit_code = 0;
        const char *p = args[1];
        bool negative = false;
        
        if (*p == '-') {
            negative = true;
            p++;
        }
        
        while (*p >= '0' && *p <= '9') {
            exit_code = exit_code * 10 + (*p - '0');
            p++;
        }
        
        if (negative) exit_code = -exit_code;
    }
    
    g_shell.running = false;
    sys_exit(exit_code);
    return exit_code; // Never reached
}

int builtin_echo(char **args) {
    bool newline = true;
    int i = 1;
    
    // Check for -n flag
    if (args[1] && my_strcmp(args[1], "-n") == 0) {
        newline = false;
        i = 2;
    }
    
    // Print arguments
    bool first = true;
    while (args[i]) {
        if (!first) print_str(" ");
        print_str(args[i]);
        first = false;
        i++;
    }
    
    if (newline) print_str("\n");
    
    return 0;
}

int builtin_clear(char **args) {
    (void)args;
    clear_screen();
    return 0;
}

int builtin_history(char **args) {
    (void)args;
    
    for (int i = 0; i < g_shell.history_count; i++) {
        print_int(i + 1);
        print_str("  ");
        print_str(g_shell.history[i]);
        print_str("\n");
    }
    
    return 0;
}

int builtin_help(char **args) {
    (void)args;
    
    print_str("Coal OS Shell - Built-in Commands:\n\n");
    
    const builtin_cmd_t *cmd = builtin_commands;
    while (cmd->name) {
        print_str("  ");
        print_str(cmd->name);
        
        // Pad to align descriptions
        int len = my_strlen(cmd->name);
        for (int i = len; i < 12; i++) {
            print_str(" ");
        }
        
        print_str("- ");
        print_str(cmd->help);
        print_str("\n");
        cmd++;
    }
    
    print_str("\nOther features:\n");
    print_str("  - Command pipelines: cmd1 | cmd2 | cmd3\n");
    print_str("  - I/O redirection: cmd < input > output\n");
    print_str("  - Background jobs: cmd &\n");
    print_str("  - Environment variables: $VAR or ${VAR}\n");
    print_str("  - Command history: Up/Down arrows\n");
    print_str("  - Tab completion: Tab key\n");
    
    return 0;
}

int builtin_jobs(char **args) {
    (void)args;
    
    if (g_shell.num_jobs == 0) {
        print_str("No jobs\n");
        return 0;
    }
    
    for (int i = 0; i < g_shell.num_jobs; i++) {
        job_t *job = &g_shell.jobs[i];
        print_str("[");
        print_int(job->job_id);
        print_str("]  ");
        
        switch (job->state) {
            case JOB_RUNNING:
                print_str("Running    ");
                break;
            case JOB_STOPPED:
                print_str("Stopped    ");
                break;
            case JOB_DONE:
                print_str("Done       ");
                break;
        }
        
        print_str(job->command);
        if (job->background) print_str(" &");
        print_str("\n");
    }
    
    return 0;
}

int builtin_fg(char **args) {
    (void)args;
    error("fg: job control not implemented");
    return 1;
}

int builtin_bg(char **args) {
    (void)args;
    error("bg: job control not implemented");
    return 1;
}

int builtin_kill(char **args) {
    if (!args[1]) {
        error("kill: missing PID or job spec");
        return 1;
    }
    
    // Parse PID
    int pid = 0;
    const char *p = args[1];
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    
    if (*p != '\0') {
        error("kill: invalid PID");
        return 1;
    }
    
    // Default signal is SIGTERM
    int sig = SIGTERM;
    
    if (sys_kill(pid, sig) < 0) {
        error("kill: failed to send signal");
        return 1;
    }
    
    return 0;
}

int builtin_ls(char **args) {
    const char *path = ".";
    if (args[1]) path = args[1];
    
    // Simple ls implementation - just try to open directory
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) {
        error("ls: cannot access directory");
        return 1;
    }
    
    // TODO: Implement directory reading when getdents is available
    print_str("ls: directory listing not fully implemented\n");
    
    sys_close(fd);
    return 0;
}

int builtin_cat(char **args) {
    if (!args[1]) {
        error("cat: missing file operand");
        return 1;
    }
    
    int fd = sys_open(args[1], O_RDONLY, 0);
    if (fd < 0) {
        error("cat: cannot open file");
        return 1;
    }
    
    char buffer[512];
    ssize_t n;
    while ((n = sys_read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        print_str(buffer);
    }
    
    sys_close(fd);
    return 0;
}

int builtin_mkdir(char **args) {
    if (!args[1]) {
        error("mkdir: missing directory name");
        return 1;
    }
    
    // mkdir syscall not implemented yet
    error("mkdir: not implemented");
    return 1;
}

int builtin_rmdir(char **args) {
    if (!args[1]) {
        error("rmdir: missing directory name");
        return 1;
    }
    
    // rmdir syscall not implemented yet
    error("rmdir: not implemented");
    return 1;
}

int builtin_rm(char **args) {
    if (!args[1]) {
        error("rm: missing file operand");
        return 1;
    }
    
    // unlink syscall not implemented yet
    error("rm: not implemented");
    return 1;
}

int builtin_touch(char **args) {
    if (!args[1]) {
        error("touch: missing file operand");
        return 1;
    }
    
    // Create empty file
    int fd = sys_open(args[1], O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        error("touch: cannot create file");
        return 1;
    }
    
    sys_close(fd);
    return 0;
}

//============================================================================
// Built-in Command Lookup
//============================================================================

const builtin_cmd_t *find_builtin(const char *cmd) {
    const builtin_cmd_t *builtin = builtin_commands;
    while (builtin->name) {
        if (my_strcmp(cmd, builtin->name) == 0) {
            return builtin;
        }
        builtin++;
    }
    return NULL;
}

int execute_builtin(const builtin_cmd_t *cmd, char **args) {
    if (!cmd || !cmd->function) return -1;
    return cmd->function(args);
}

const builtin_cmd_t *get_builtins(void) {
    return builtin_commands;
}