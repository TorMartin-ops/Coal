/**
 * @file shell_main.c
 * @brief Main entry point and core logic for Coal OS Shell
 * @version 2.0
 * @author Modular refactoring
 */

#include "shell_types.h"
#include "modules/syscall_wrapper.h"
#include "modules/string_utils.h"
#include "modules/io_utils.h"
#include "modules/env_vars.h"
#include "modules/parser.h"
#include "modules/builtins.h"

//============================================================================
// Global State
//============================================================================

shell_state_t g_shell;
static char g_input_buffer[MAX_COMMAND_LENGTH];

//============================================================================
// Forward Declarations
//============================================================================

static void shell_init(void);
static void show_prompt(void);
static int read_command(char *buffer, size_t size);
static int execute_command(char *command);
static int execute_pipeline(pipeline_t *pipeline);
static int execute_simple_command(command_t *cmd);
static void add_to_history(const char *command);
static void reap_children(void);

//============================================================================
// Shell Entry Point
//============================================================================

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Initialize shell
    shell_init();
    
    // Display welcome message
    print_colored(COLOR_GREEN, "Coal OS Shell v2.0\n");
    print_str("Type 'help' for built-in commands\n\n");
    
    // Main shell loop
    while (g_shell.running) {
        // Reap zombie children
        reap_children();
        
        // Show prompt
        show_prompt();
        
        // Read command
        if (read_command(g_input_buffer, sizeof(g_input_buffer)) < 0) {
            continue;
        }
        
        // Skip empty commands
        char *trimmed = trim_whitespace(g_input_buffer);
        if (!trimmed || *trimmed == '\0') {
            continue;
        }
        
        // Add to history
        add_to_history(trimmed);
        
        // Expand environment variables
        char expanded[MAX_COMMAND_LENGTH];
        expand_variables(trimmed, expanded, sizeof(expanded));
        
        // Execute command
        g_shell.last_exit_status = execute_command(expanded);
    }
    
    return g_shell.last_exit_status;
}

//============================================================================
// Shell Initialization
//============================================================================

static void shell_init(void) {
    // Initialize shell state
    my_memset(&g_shell, 0, sizeof(g_shell));
    g_shell.running = true;
    g_shell.tab_completion_enabled = true;
    g_shell.history_navigation_enabled = true;
    g_shell.auto_cd_enabled = true;
    
    // Get initial working directory
    if (sys_getcwd(g_shell.cwd, sizeof(g_shell.cwd)) == 0) {
        my_strcpy(g_shell.cwd, "/");
    }
    
    // Initialize environment variables
    env_init();
    
    // Set up signal handlers (if supported)
    // sys_signal(SIGINT, SIG_IGN);  // Ignore Ctrl+C
    // sys_signal(SIGCHLD, sigchld_handler);  // Handle child termination
}

//============================================================================
// Command Execution
//============================================================================

static void show_prompt(void) {
    char *prompt = get_env("PS1");
    if (!prompt) prompt = "$ ";
    
    // Support simple prompt expansions
    char expanded_prompt[256];
    const char *src = prompt;
    char *dst = expanded_prompt;
    
    while (*src && dst < expanded_prompt + sizeof(expanded_prompt) - 1) {
        if (*src == '\\' && *(src + 1)) {
            src++; // Skip backslash
            switch (*src) {
                case 'w': // Working directory
                    {
                        const char *cwd = g_shell.cwd;
                        while (*cwd && dst < expanded_prompt + sizeof(expanded_prompt) - 1) {
                            *dst++ = *cwd++;
                        }
                    }
                    break;
                case 'u': // Username
                    {
                        char *user = get_env("USER");
                        if (user) {
                            while (*user && dst < expanded_prompt + sizeof(expanded_prompt) - 1) {
                                *dst++ = *user++;
                            }
                        }
                    }
                    break;
                case 'h': // Hostname
                    *dst++ = 'c';
                    *dst++ = 'o';
                    *dst++ = 'a';
                    *dst++ = 'l';
                    break;
                case '$': // $ for user, # for root
                    *dst++ = '$';
                    break;
                default:
                    *dst++ = *src;
                    break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    
    print_colored(COLOR_CYAN, expanded_prompt);
}

static int read_command(char *buffer, size_t size) {
    // Simple line reading for now
    // TODO: Implement advanced features like history navigation and tab completion
    ssize_t n = sys_read_terminal_line(buffer, size - 1);
    if (n < 0) {
        return -1;
    }
    
    // Remove newline if present
    if (n > 0 && buffer[n - 1] == '\n') {
        buffer[n - 1] = '\0';
    } else {
        buffer[n] = '\0';
    }
    
    return 0;
}

static int execute_command(char *command) {
    // Check for compound commands (;)
    char *semicolon = my_strchr(command, ';');
    if (semicolon) {
        *semicolon = '\0';
        execute_command(command);
        return execute_command(semicolon + 1);
    }
    
    // Parse pipeline
    pipeline_t pipeline;
    if (parse_pipeline(command, &pipeline) < 0) {
        error("syntax error");
        return 1;
    }
    
    return execute_pipeline(&pipeline);
}

static int execute_pipeline(pipeline_t *pipeline) {
    if (pipeline->num_commands == 1) {
        // Single command - check for built-ins
        command_t *cmd = &pipeline->commands[0];
        if (cmd->argc > 0) {
            const builtin_cmd_t *builtin = find_builtin(cmd->args[0]);
            if (builtin) {
                // Handle I/O redirection for built-ins
                int saved_stdin = -1;
                int saved_stdout = -1;
                
                if (cmd->input_file) {
                    saved_stdin = sys_dup2(STDIN_FILENO, 100);
                    int fd = sys_open(cmd->input_file, O_RDONLY, 0);
                    if (fd < 0) {
                        error("cannot open input file");
                        return 1;
                    }
                    sys_dup2(fd, STDIN_FILENO);
                    sys_close(fd);
                }
                
                if (cmd->output_file) {
                    saved_stdout = sys_dup2(STDOUT_FILENO, 101);
                    int flags = O_WRONLY | O_CREAT;
                    if (cmd->append_output) {
                        flags |= O_APPEND;
                    } else {
                        flags |= O_TRUNC;
                    }
                    int fd = sys_open(cmd->output_file, flags, 0644);
                    if (fd < 0) {
                        error("cannot open output file");
                        return 1;
                    }
                    sys_dup2(fd, STDOUT_FILENO);
                    sys_close(fd);
                }
                
                // Execute built-in
                int status = execute_builtin(builtin, cmd->args);
                
                // Restore I/O
                if (saved_stdin >= 0) {
                    sys_dup2(saved_stdin, STDIN_FILENO);
                    sys_close(saved_stdin);
                }
                if (saved_stdout >= 0) {
                    sys_dup2(saved_stdout, STDOUT_FILENO);
                    sys_close(saved_stdout);
                }
                
                return status;
            }
        }
        
        // Not a built-in - execute as external command
        return execute_simple_command(cmd);
    }
    
    // Multiple commands in pipeline
    // TODO: Implement pipe support
    error("pipelines not yet implemented");
    return 1;
}

static int execute_simple_command(command_t *cmd) {
    if (cmd->argc == 0) return 0;
    
    // Fork and execute
    pid_t pid = sys_fork();
    if (pid < 0) {
        error("fork failed");
        return 1;
    }
    
    if (pid == 0) {
        // Child process
        
        // Handle I/O redirection
        if (cmd->input_file) {
            int fd = sys_open(cmd->input_file, O_RDONLY, 0);
            if (fd < 0) {
                error("cannot open input file");
                sys_exit(1);
            }
            sys_dup2(fd, STDIN_FILENO);
            sys_close(fd);
        }
        
        if (cmd->output_file) {
            int flags = O_WRONLY | O_CREAT;
            if (cmd->append_output) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }
            int fd = sys_open(cmd->output_file, flags, 0644);
            if (fd < 0) {
                error("cannot open output file");
                sys_exit(1);
            }
            sys_dup2(fd, STDOUT_FILENO);
            sys_close(fd);
        }
        
        // Try to execute command
        // First try as-is
        sys_execve(cmd->args[0], cmd->args, NULL);
        
        // If that fails, try with /bin/ prefix
        char path[256] = "/bin/";
        my_strcat(path, cmd->args[0]);
        sys_execve(path, cmd->args, NULL);
        
        // If still fails, command not found
        error("command not found");
        sys_exit(127);
    }
    
    // Parent process
    if (!cmd->background) {
        // Wait for child
        int status;
        sys_waitpid(pid, &status, 0);
        return status;
    } else {
        // Background job
        print_str("[");
        print_int(pid);
        print_str("] ");
        print_int(pid);
        print_str("\n");
        
        // TODO: Add to job list
        return 0;
    }
}

//============================================================================
// History Management
//============================================================================

static void add_to_history(const char *command) {
    // Don't add empty commands or duplicates
    if (!command || *command == '\0') return;
    
    if (g_shell.history_count > 0 && 
        my_strcmp(g_shell.history[g_shell.history_count - 1], command) == 0) {
        return;
    }
    
    // Add to history (simplified - no dynamic allocation)
    static char history_storage[MAX_HISTORY][MAX_COMMAND_LENGTH];
    
    if (g_shell.history_count < MAX_HISTORY) {
        my_strcpy(history_storage[g_shell.history_count], command);
        g_shell.history[g_shell.history_count] = history_storage[g_shell.history_count];
        g_shell.history_count++;
    } else {
        // History full - shift everything up
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            my_strcpy(history_storage[i], history_storage[i + 1]);
            g_shell.history[i] = history_storage[i];
        }
        my_strcpy(history_storage[MAX_HISTORY - 1], command);
        g_shell.history[MAX_HISTORY - 1] = history_storage[MAX_HISTORY - 1];
    }
    
    g_shell.history_index = g_shell.history_count;
}

//============================================================================
// Process Management
//============================================================================

static void reap_children(void) {
    // Reap any zombie children
    pid_t pid;
    int status;
    
    while ((pid = sys_waitpid(-1, &status, 1)) > 0) { // WNOHANG = 1
        // Child reaped
        // TODO: Update job list
    }
}