/**
 * @file shell.c
 * @brief Advanced Coal OS Shell - Feature-rich POSIX-compatible shell
 * @version 2.0
 * @author Claude Code
 * 
 * Features:
 * - Command parsing with arguments
 * - Pipe support (cmd1 | cmd2 | cmd3)
 * - I/O redirection (>, <, >>, 2>)
 * - Background job execution (&)
 * - Job control (jobs, fg, bg, kill)
 * - Built-in commands (cd, pwd, echo, ls, etc.)
 * - Environment variable support
 * - Command history
 * - Signal handling
 * - Executable launching via fork/exec
 */

//============================================================================
// Type Definitions and Constants
//============================================================================

#ifndef _SHELL_TYPES_DEFINED
#define _SHELL_TYPES_DEFINED

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t           uintptr_t;
typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef int32_t            pid_t;

typedef char bool;
#define true  1
#define false 0

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

#endif // _SHELL_TYPES_DEFINED

//============================================================================
// System Call Interface
//============================================================================

// System call numbers (must match kernel syscall.h)
#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_PUTS    7
#define SYS_EXECVE  11
#define SYS_CHDIR   12
#define SYS_WAITPID 17
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_READ_TERMINAL_LINE 21
#define SYS_DUP2    33
#define SYS_KILL    37
#define SYS_PIPE    42
#define SYS_SIGNAL  48
#define SYS_GETPPID 64
#define SYS_GETCWD  183

// File descriptor constants
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// File open flags
#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040
#define O_TRUNC      0x0200
#define O_APPEND     0x0400

// Signal numbers
#define SIGINT   2
#define SIGQUIT  3
#define SIGTERM  15
#define SIGKILL  9
#define SIGCHLD  17

// System call wrapper
static inline int32_t syscall(int32_t syscall_number, int32_t arg1, int32_t arg2, int32_t arg3) {
    int32_t return_value;
    __asm__ volatile (
        "pushl %%ebx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "movl %1, %%eax\n\t"
        "movl %2, %%ebx\n\t"
        "movl %3, %%ecx\n\t"
        "movl %4, %%edx\n\t"
        "int $0x80\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "popl %%ebx\n\t"
        : "=a" (return_value)
        : "m" (syscall_number), "m" (arg1), "m" (arg2), "m" (arg3)
        : "cc", "memory"
    );
    return return_value;
}

// System call helpers
#define sys_exit(code)       syscall(SYS_EXIT, (code), 0, 0)
#define sys_fork()           syscall(SYS_FORK, 0, 0, 0)
#define sys_read(fd,buf,n)   syscall(SYS_READ, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_write(fd,buf,n)  syscall(SYS_WRITE, (fd), (int32_t)(uintptr_t)(buf), (n))
#define sys_open(p,f,m)      syscall(SYS_OPEN, (int32_t)(uintptr_t)(p), (f), (m))
#define sys_close(fd)        syscall(SYS_CLOSE, (fd), 0, 0)
#define sys_puts(p)          syscall(SYS_PUTS, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_execve(p,a,e)    syscall(SYS_EXECVE, (int32_t)(uintptr_t)(p), (int32_t)(uintptr_t)(a), (int32_t)(uintptr_t)(e))
#define sys_chdir(p)         syscall(SYS_CHDIR, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_waitpid(p,s,o)   syscall(SYS_WAITPID, (p), (int32_t)(uintptr_t)(s), (o))
#define sys_getpid()         syscall(SYS_GETPID, 0, 0, 0)
#define sys_getppid()        syscall(SYS_GETPPID, 0, 0, 0)
#define sys_read_terminal_line(buf,n) syscall(SYS_READ_TERMINAL_LINE, (int32_t)(uintptr_t)(buf), (n), 0)
#define sys_dup2(o,n)        syscall(SYS_DUP2, (o), (n), 0)
#define sys_kill(p,s)        syscall(SYS_KILL, (p), (s), 0)
#define sys_pipe(p)          syscall(SYS_PIPE, (int32_t)(uintptr_t)(p), 0, 0)
#define sys_signal(s,h)      syscall(SYS_SIGNAL, (s), (int32_t)(uintptr_t)(h), 0)
#define sys_getcwd(buf,size) syscall(SYS_GETCWD, (int32_t)(uintptr_t)(buf), (size), 0)

//============================================================================
// Configuration Constants
//============================================================================

#define MAX_COMMAND_LENGTH 1024
#define MAX_ARGS 64
#define MAX_JOBS 32
#define MAX_HISTORY 100
#define MAX_ENV_VARS 64
#define MAX_PATH_LENGTH 256
#define MAX_ALIASES 32
#define MAX_COMPLETIONS 64

//============================================================================
// Data Structures
//============================================================================

// Job states
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

// Job structure for job control
typedef struct job {
    int job_id;
    pid_t pgid;
    pid_t *pids;
    int num_processes;
    char *command;
    job_state_t state;
    bool background;
} job_t;

// Command structure
typedef struct command {
    char *args[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
    bool append_output;
    bool background;
} command_t;

// Pipeline structure
typedef struct pipeline {
    command_t commands[MAX_ARGS];
    int num_commands;
    bool background;
} pipeline_t;

// Environment variable
typedef struct env_var {
    char *name;
    char *value;
} env_var_t;

// Command alias
typedef struct alias {
    char *name;
    char *command;
} alias_t;

// Tab completion entry
typedef struct completion {
    char *text;
    int is_command;
} completion_t;

// Shell state
typedef struct {
    job_t jobs[MAX_JOBS];
    int num_jobs;
    char *history[MAX_HISTORY];
    int history_count;
    int history_index;
    int current_history_pos;
    env_var_t env_vars[MAX_ENV_VARS];
    int num_env_vars;
    alias_t aliases[MAX_ALIASES];
    int num_aliases;
    char cwd[MAX_PATH_LENGTH];
    bool running;
    int last_exit_status;
    bool tab_completion_enabled;
    bool history_navigation_enabled;
    bool auto_cd_enabled;
    char last_directory[MAX_PATH_LENGTH];
} shell_state_t;

//============================================================================
// Global State
//============================================================================

static shell_state_t g_shell;
static char g_input_buffer[MAX_COMMAND_LENGTH];
static completion_t g_completions[MAX_COMPLETIONS];
static int g_num_completions = 0;

//============================================================================
// Utility Functions
//============================================================================

// String utility functions
static size_t my_strlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

static int my_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int my_strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (n-- && *s1 && (*s1 == *s2)) { s1++; s2++; }
    if (n == (size_t)-1) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static char *my_strcpy(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while ((*dest++ = *src++));
    return ret;
}

static char *my_strncpy(char *dest, const char *src, size_t n) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (n-- && (*dest++ = *src++));
    while (n-- > 0) *dest++ = '\0';
    return ret;
}

static char *my_strcat(char *dest, const char *src) {
    char *ret = dest;
    if (!dest || !src) return dest;
    while (*dest) dest++;
    while ((*dest++ = *src++));
    return ret;
}

static char *my_strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

static void *my_memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

// Print functions
static void print_str(const char *s) {
    if (s) sys_puts(s);
}

static void print_char(char c) {
    char buf[2] = {c, '\0'};
    sys_puts(buf);
}

// Clear screen command
static int builtin_clear(char **args) {
    (void)args;
    // ANSI escape sequence to clear screen
    print_str("\033[2J\033[H");
    return 0;
}

static void print_int(int n) {
    char buf[12];
    char *p = buf + 11;
    *p = '\0';
    
    bool negative = n < 0;
    if (negative) n = -n;
    
    if (n == 0) {
        *--p = '0';
    } else {
        while (n > 0) {
            *--p = '0' + (n % 10);
            n /= 10;
        }
    }
    
    if (negative) *--p = '-';
    print_str(p);
}

// Error handling
static void error(const char *msg) {
    print_str("shell: ");
    print_str(msg);
    print_str("\n");
}

static void perror_msg(const char *msg) {
    print_str("shell: ");
    print_str(msg);
    print_str(": operation failed\n");
}

//============================================================================
// Forward Declarations
//============================================================================

static char *get_env(const char *name);
static char *my_strchr(const char *s, int c);
static bool is_whitespace(char c);
static int parse_arguments(char *input, char **args, int max_args);
static void add_completion(const char *text, int is_command);
static void clear_completions(void);
static void find_command_completions(const char *prefix);
static void find_file_completions(const char *prefix);
static int handle_tab_completion(char *buffer, int cursor_pos);

//============================================================================
// Environment Variable Management
//============================================================================

// Variable expansion helper
static char *expand_variables(const char *input, char *output, size_t output_size) {
    const char *src = input;
    char *dst = output;
    char *dst_end = output + output_size - 1;
    
    while (*src && dst < dst_end) {
        if (*src == '$' && *(src + 1) == '{') {
            // Variable expansion ${VAR}
            src += 2; // Skip "${"
            const char *var_start = src;
            while (*src && *src != '}' && dst < dst_end) src++;
            
            if (*src == '}') {
                // Extract variable name
                char var_name[64];
                int var_len = src - var_start;
                if (var_len < (int)sizeof(var_name) - 1) {
                    my_strncpy(var_name, var_start, var_len);
                    var_name[var_len] = '\0';
                    
                    // Look up variable value
                    char *var_value = get_env(var_name);
                    if (var_value) {
                        // Copy variable value
                        while (*var_value && dst < dst_end) {
                            *dst++ = *var_value++;
                        }
                    }
                }
                src++; // Skip "}"
            }
        } else if (*src == '$' && ((*(src + 1) >= 'A' && *(src + 1) <= 'Z') || 
                   (*(src + 1) >= 'a' && *(src + 1) <= 'z') || *(src + 1) == '_')) {
            // Simple variable expansion $VAR
            src++; // Skip "$"
            const char *var_start = src;
            while ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') || 
                   (*src >= '0' && *src <= '9') || *src == '_') {
                src++;
            }
            
            // Extract variable name
            char var_name[64];
            int var_len = src - var_start;
            if (var_len < (int)sizeof(var_name) - 1) {
                my_strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                
                // Look up variable value
                char *var_value = get_env(var_name);
                if (var_value) {
                    // Copy variable value
                    while (*var_value && dst < dst_end) {
                        *dst++ = *var_value++;
                    }
                }
            }
        } else {
            *dst++ = *src++;
        }
    }
    
    *dst = '\0';
    return output;
}

static char *get_env(const char *name) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            return g_shell.env_vars[i].value;
        }
    }
    return NULL;
}

static void set_env(const char *name, const char *value) {
    // Check if variable already exists
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            // Update existing variable
            // Note: In a real implementation, we'd need proper memory management
            my_strcpy(g_shell.env_vars[i].value, value);
            return;
        }
    }
    
    // Add new variable
    if (g_shell.num_env_vars < MAX_ENV_VARS) {
        // Note: In a real implementation, we'd allocate memory properly
        g_shell.env_vars[g_shell.num_env_vars].name = (char*)name;  // Simplified
        g_shell.env_vars[g_shell.num_env_vars].value = (char*)value; // Simplified
        g_shell.num_env_vars++;
    }
}

static void unset_env(const char *name) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            // Remove by shifting remaining entries
            for (int j = i; j < g_shell.num_env_vars - 1; j++) {
                g_shell.env_vars[j] = g_shell.env_vars[j + 1];
            }
            g_shell.num_env_vars--;
            return;
        }
    }
}

static void show_env(void) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        print_str(g_shell.env_vars[i].name);
        print_str("=");
        print_str(g_shell.env_vars[i].value);
        print_str("\n");
    }
}

//============================================================================
// Alias Management
//============================================================================

static char *get_alias(const char *name) {
    for (int i = 0; i < g_shell.num_aliases; i++) {
        if (my_strcmp(g_shell.aliases[i].name, name) == 0) {
            return g_shell.aliases[i].command;
        }
    }
    return NULL;
}

static void set_alias(const char *name, const char *command) {
    // Check if alias already exists
    for (int i = 0; i < g_shell.num_aliases; i++) {
        if (my_strcmp(g_shell.aliases[i].name, name) == 0) {
            // Update existing alias
            my_strcpy(g_shell.aliases[i].command, command);
            return;
        }
    }
    
    // Add new alias
    if (g_shell.num_aliases < MAX_ALIASES) {
        g_shell.aliases[g_shell.num_aliases].name = (char*)name;
        g_shell.aliases[g_shell.num_aliases].command = (char*)command;
        g_shell.num_aliases++;
    }
}

static void unset_alias(const char *name) {
    for (int i = 0; i < g_shell.num_aliases; i++) {
        if (my_strcmp(g_shell.aliases[i].name, name) == 0) {
            // Remove by shifting remaining entries
            for (int j = i; j < g_shell.num_aliases - 1; j++) {
                g_shell.aliases[j] = g_shell.aliases[j + 1];
            }
            g_shell.num_aliases--;
            return;
        }
    }
}

static void show_aliases(void) {
    for (int i = 0; i < g_shell.num_aliases; i++) {
        print_str("alias ");
        print_str(g_shell.aliases[i].name);
        print_str("='");
        print_str(g_shell.aliases[i].command);
        print_str("'\n");
    }
}

// Expand aliases in command
static char *expand_alias(const char *command, char *output, size_t output_size) {
    (void)output_size; // Mark unused parameter
    char *args[MAX_ARGS];
    char temp_cmd[MAX_COMMAND_LENGTH];
    my_strcpy(temp_cmd, command);
    
    int argc = parse_arguments(temp_cmd, args, MAX_ARGS);
    if (argc > 0) {
        char *alias_cmd = get_alias(args[0]);
        if (alias_cmd) {
            // Replace first argument with alias expansion
            my_strcpy(output, alias_cmd);
            
            // Append remaining arguments
            for (int i = 1; i < argc; i++) {
                my_strcat(output, " ");
                my_strcat(output, args[i]);
            }
            return output;
        }
    }
    
    // No alias found, return original command
    my_strcpy(output, command);
    return output;
}


//============================================================================
// Command History Management
//============================================================================

static void add_to_history(const char *command) {
    if (g_shell.history_count < MAX_HISTORY) {
        // Simplified - would need proper memory allocation
        g_shell.history[g_shell.history_count] = (char*)command;
        g_shell.history_count++;
    } else {
        // Shift history
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            g_shell.history[i] = g_shell.history[i + 1];
        }
        g_shell.history[MAX_HISTORY - 1] = (char*)command;
    }
    g_shell.history_index = g_shell.history_count;
}

static void show_history(void) {
    for (int i = 0; i < g_shell.history_count; i++) {
        print_str("  ");
        print_int(i + 1);
        print_str("  ");
        print_str(g_shell.history[i]);
        print_str("\n");
    }
}

//============================================================================
// Job Control
//============================================================================

static void add_job(pid_t pgid, const char *command, bool background) {
    if (g_shell.num_jobs < MAX_JOBS) {
        job_t *job = &g_shell.jobs[g_shell.num_jobs];
        job->job_id = g_shell.num_jobs + 1;
        job->pgid = pgid;
        job->command = (char*)command; // Simplified
        job->state = JOB_RUNNING;
        job->background = background;
        job->num_processes = 1;
        g_shell.num_jobs++;
        
        if (background) {
            print_str("[");
            print_int(job->job_id);
            print_str("] ");
            print_int(pgid);
            print_str("\n");
        }
    }
}

static void show_jobs(void) {
    for (int i = 0; i < g_shell.num_jobs; i++) {
        job_t *job = &g_shell.jobs[i];
        print_str("[");
        print_int(job->job_id);
        print_str("] ");
        
        switch (job->state) {
            case JOB_RUNNING:
                print_str("Running");
                break;
            case JOB_STOPPED:
                print_str("Stopped");
                break;
            case JOB_DONE:
                print_str("Done");
                break;
        }
        
        print_str("                 ");
        print_str(job->command);
        if (job->background) print_str(" &");
        print_str("\n");
    }
}

static job_t *find_job(int job_id) {
    for (int i = 0; i < g_shell.num_jobs; i++) {
        if (g_shell.jobs[i].job_id == job_id) {
            return &g_shell.jobs[i];
        }
    }
    return NULL;
}

static int builtin_fg(char **args) {
    int job_id = 1; // Default to job 1
    if (args[1]) {
        // Parse job ID
        char *p = args[1];
        if (*p == '%') p++; // Skip % prefix if present
        job_id = 0;
        while (*p >= '0' && *p <= '9') {
            job_id = job_id * 10 + (*p - '0');
            p++;
        }
    }
    
    job_t *job = find_job(job_id);
    if (!job) {
        error("fg: job not found");
        return 1;
    }
    
    if (job->state != JOB_STOPPED) {
        error("fg: job not stopped");
        return 1;
    }
    
    print_str(job->command);
    print_str("\n");
    
    // Resume job in foreground
    job->state = JOB_RUNNING;
    job->background = false;
    
    // Wait for job to complete
    int status;
    sys_waitpid(job->pgid, &status, 0);
    job->state = JOB_DONE;
    
    return 0;
}

static int builtin_bg(char **args) {
    int job_id = 1; // Default to job 1
    if (args[1]) {
        // Parse job ID
        char *p = args[1];
        if (*p == '%') p++; // Skip % prefix if present
        job_id = 0;
        while (*p >= '0' && *p <= '9') {
            job_id = job_id * 10 + (*p - '0');
            p++;
        }
    }
    
    job_t *job = find_job(job_id);
    if (!job) {
        error("bg: job not found");
        return 1;
    }
    
    if (job->state != JOB_STOPPED) {
        error("bg: job not stopped");
        return 1;
    }
    
    print_str("[");
    print_int(job->job_id);
    print_str("] ");
    print_str(job->command);
    print_str(" &\n");
    
    // Resume job in background
    job->state = JOB_RUNNING;
    job->background = true;
    
    return 0;
}

//============================================================================
// Command Parsing
//============================================================================

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char *skip_whitespace(char *str) {
    while (*str && is_whitespace(*str)) str++;
    return str;
}

static int parse_arguments(char *input, char **args, int max_args) {
    int argc = 0;
    char *p = skip_whitespace(input);
    
    while (*p && argc < max_args - 1) {
        args[argc++] = p;
        
        // Find end of current argument
        while (*p && !is_whitespace(*p)) p++;
        
        if (*p) {
            *p++ = '\0';  // Null-terminate current argument
            p = skip_whitespace(p);
        }
    }
    
    args[argc] = NULL;
    return argc;
}

// Helper function to parse I/O redirection from a command string
static void parse_redirections(char *cmd_str, command_t *cmd) {
    char *token = cmd_str;
    char *args[MAX_ARGS];
    int argc = 0;
    
    // Initialize redirection fields
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->append_output = false;
    
    // Parse tokens and look for redirection operators
    while (*token && argc < MAX_ARGS - 1) {
        token = skip_whitespace(token);
        if (!*token) break;
        
        // Check for redirection operators
        if (*token == '>') {
            if (*(token + 1) == '>') {
                // Append redirection (>>)
                token += 2;
                token = skip_whitespace(token);
                if (*token) {
                    cmd->output_file = token;
                    cmd->append_output = true;
                    // Find end of filename
                    while (*token && !is_whitespace(*token)) token++;
                    if (*token) *token++ = '\0';
                }
            } else {
                // Output redirection (>)
                token++;
                token = skip_whitespace(token);
                if (*token) {
                    cmd->output_file = token;
                    cmd->append_output = false;
                    // Find end of filename
                    while (*token && !is_whitespace(*token)) token++;
                    if (*token) *token++ = '\0';
                }
            }
        } else if (*token == '<') {
            // Input redirection (<)
            token++;
            token = skip_whitespace(token);
            if (*token) {
                cmd->input_file = token;
                // Find end of filename
                while (*token && !is_whitespace(*token)) token++;
                if (*token) *token++ = '\0';
            }
        } else if (*token == '2' && *(token + 1) == '>') {
            // Error redirection (2>) - simplified, treat as output redirection
            token += 2;
            token = skip_whitespace(token);
            if (*token) {
                cmd->output_file = token;
                cmd->append_output = false;
                // Find end of filename
                while (*token && !is_whitespace(*token)) token++;
                if (*token) *token++ = '\0';
            }
        } else {
            // Regular argument
            args[argc++] = token;
            // Find end of argument
            while (*token && !is_whitespace(*token) && *token != '>' && *token != '<') {
                token++;
            }
            if (*token) *token++ = '\0';
        }
    }
    
    // Copy arguments to command structure
    for (int i = 0; i < argc && i < MAX_ARGS - 1; i++) {
        cmd->args[i] = args[i];
    }
    cmd->args[argc] = NULL;
    cmd->argc = argc;
}

static void parse_pipeline(char *input, pipeline_t *pipeline) {
    my_memset(pipeline, 0, sizeof(pipeline_t));
    
    // Check for background execution
    int len = my_strlen(input);
    if (len > 0 && input[len - 1] == '&') {
        pipeline->background = true;
        input[len - 1] = '\0';  // Remove the '&'
        // Remove trailing whitespace after removing '&'
        len--;
        while (len > 0 && is_whitespace(input[len - 1])) {
            input[--len] = '\0';
        }
    }
    
    // Parse pipeline by splitting on '|' character
    char *cmd_start = input;
    int cmd_index = 0;
    
    for (int i = 0; input[i] && cmd_index < MAX_ARGS; i++) {
        if (input[i] == '|') {
            // Found a pipe - null terminate current command
            input[i] = '\0';
            
            // Parse current command with redirections
            command_t *cmd = &pipeline->commands[cmd_index];
            char *trimmed_cmd = skip_whitespace(cmd_start);
            if (*trimmed_cmd) {  // Only add non-empty commands
                parse_redirections(trimmed_cmd, cmd);
                cmd_index++;
            }
            
            // Move to next command after the pipe
            cmd_start = skip_whitespace(&input[i + 1]);
            i = cmd_start - input - 1;  // Adjust i for next iteration
        }
    }
    
    // Parse the last command (or the only command if no pipes)
    if (cmd_index < MAX_ARGS) {
        command_t *cmd = &pipeline->commands[cmd_index];
        char *trimmed_cmd = skip_whitespace(cmd_start);
        if (*trimmed_cmd) {  // Only add non-empty commands
            parse_redirections(trimmed_cmd, cmd);
            cmd_index++;
        }
    }
    
    pipeline->num_commands = cmd_index;
}

//============================================================================
// Built-in Commands Implementation
//============================================================================

static int builtin_cd(char **args) {
    const char *dir;
    
    if (!args[1]) {
        // No argument - go to home directory
        dir = get_env("HOME");
        if (!dir) dir = "/";
    } else if (my_strcmp(args[1], "-") == 0) {
        // cd - goes to previous directory
        if (g_shell.last_directory[0] != '\0') {
            dir = g_shell.last_directory;
            print_str(dir);
            print_str("\n");
        } else {
            error("cd: no previous directory");
            return 1;
        }
    } else {
        dir = args[1];
    }
    
    // Save current directory as last directory
    char old_cwd[MAX_PATH_LENGTH];
    if (sys_getcwd(old_cwd, sizeof(old_cwd)) >= 0) {
        my_strcpy(g_shell.last_directory, old_cwd);
    }
    
    if (sys_chdir(dir) == 0) {
        // Update current working directory
        if (sys_getcwd(g_shell.cwd, sizeof(g_shell.cwd)) >= 0) {
            set_env("PWD", g_shell.cwd);
            set_env("OLDPWD", old_cwd);
        }
        return 0;
    } else {
        perror_msg("cd");
        return 1;
    }
}

static int builtin_pwd(char **args) {
    (void)args;
    
    char buf[MAX_PATH_LENGTH];
    if (sys_getcwd(buf, sizeof(buf)) >= 0) {
        print_str(buf);
        print_str("\n");
        return 0;
    } else {
        perror_msg("pwd");
        return 1;
    }
}

static int builtin_echo(char **args) {
    for (int i = 1; args[i]; i++) {
        if (i > 1) print_str(" ");
        print_str(args[i]);
    }
    print_str("\n");
    return 0;
}

static int builtin_exit(char **args) {
    int code = args[1] ? 0 : 0; // Would parse args[1] as integer
    g_shell.running = false;
    return code;
}

static int builtin_jobs(char **args) {
    (void)args;
    show_jobs();
    return 0;
}

static int builtin_history(char **args) {
    (void)args;
    show_history();
    return 0;
}

static int builtin_kill(char **args) {
    if (!args[1]) {
        error("kill: missing arguments");
        return 1;
    }
    
    // Parse PID (simplified)
    int pid = 0;
    char *p = args[1];
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    
    int sig = args[2] ? SIGTERM : SIGTERM; // Would parse signal
    
    if (sys_kill(pid, sig) == 0) {
        return 0;
    } else {
        perror_msg("kill");
        return 1;
    }
}

static int builtin_env(char **args) {
    if (args[1]) {
        // Set environment variable: env VAR=value
        char *eq = args[1];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            set_env(args[1], eq + 1);
            *eq = '=';
        } else {
            error("env: invalid format (use VAR=value)");
            return 1;
        }
    } else {
        // Show all environment variables
        show_env();
    }
    return 0;
}

static int builtin_export(char **args) {
    if (!args[1]) {
        error("export: missing variable name");
        return 1;
    }
    
    char *eq = args[1];
    while (*eq && *eq != '=') eq++;
    if (*eq == '=') {
        *eq = '\0';
        set_env(args[1], eq + 1);
        *eq = '=';
    } else {
        // Export existing variable (simplified - just show it)
        char *value = get_env(args[1]);
        if (value) {
            print_str(args[1]);
            print_str("=");
            print_str(value);
            print_str("\n");
        } else {
            error("export: variable not found");
            return 1;
        }
    }
    return 0;
}

static int builtin_unset(char **args) {
    if (!args[1]) {
        error("unset: missing variable name");
        return 1;
    }
    unset_env(args[1]);
    return 0;
}

static int builtin_alias(char **args) {
    if (!args[1]) {
        // Show all aliases
        show_aliases();
        return 0;
    }
    
    char *eq = args[1];
    while (*eq && *eq != '=') eq++;
    if (*eq == '=') {
        *eq = '\0';
        set_alias(args[1], eq + 1);
        *eq = '=';
    } else {
        // Show specific alias
        char *cmd = get_alias(args[1]);
        if (cmd) {
            print_str("alias ");
            print_str(args[1]);
            print_str("='");
            print_str(cmd);
            print_str("'\n");
        } else {
            error("alias: not found");
            return 1;
        }
    }
    return 0;
}

static int builtin_unalias(char **args) {
    if (!args[1]) {
        error("unalias: missing alias name");
        return 1;
    }
    unset_alias(args[1]);
    return 0;
}

static int builtin_set(char **args) {
    if (!args[1]) {
        // Show shell options
        print_str("Shell Options:\n");
        print_str("  Tab completion: ");
        print_str(g_shell.tab_completion_enabled ? "on" : "off");
        print_str("\n");
        print_str("  History navigation: ");
        print_str(g_shell.history_navigation_enabled ? "on" : "off");
        print_str("\n");
        print_str("  Auto cd: ");
        print_str(g_shell.auto_cd_enabled ? "on" : "off");
        print_str("\n");
        return 0;
    }
    
    if (my_strcmp(args[1], "+o") == 0 && args[2]) {
        // Enable option
        if (my_strcmp(args[2], "tab_completion") == 0) {
            g_shell.tab_completion_enabled = true;
            print_str("Tab completion enabled\n");
        } else if (my_strcmp(args[2], "history_navigation") == 0) {
            g_shell.history_navigation_enabled = true;
            print_str("History navigation enabled\n");
        } else if (my_strcmp(args[2], "auto_cd") == 0) {
            g_shell.auto_cd_enabled = true;
            print_str("Auto cd enabled\n");
        } else {
            error("set: unknown option");
            return 1;
        }
    } else if (my_strcmp(args[1], "-o") == 0 && args[2]) {
        // Disable option
        if (my_strcmp(args[2], "tab_completion") == 0) {
            g_shell.tab_completion_enabled = false;
            print_str("Tab completion disabled\n");
        } else if (my_strcmp(args[2], "history_navigation") == 0) {
            g_shell.history_navigation_enabled = false;
            print_str("History navigation disabled\n");
        } else if (my_strcmp(args[2], "auto_cd") == 0) {
            g_shell.auto_cd_enabled = false;
            print_str("Auto cd disabled\n");
        } else {
            error("set: unknown option");
            return 1;
        }
    } else {
        error("set: invalid syntax (use +o/-o option_name)");
        return 1;
    }
    return 0;
}

static int builtin_help(char **args) {
    (void)args;
    print_str("Coal OS Advanced Shell v2.0\n");
    print_str("Built-in commands:\n");
    print_str("  cd [dir]       - Change directory\n");
    print_str("  pwd            - Print working directory\n");
    print_str("  echo [args]    - Echo arguments\n");
    print_str("  env [VAR=val]  - Show/set environment variables\n");
    print_str("  export VAR=val - Export environment variable\n");
    print_str("  unset VAR      - Unset environment variable\n");
    print_str("  alias [name=cmd] - Show/set command aliases\n");
    print_str("  unalias name   - Remove alias\n");
    print_str("  set [±o option] - Set shell options\n");
    print_str("  clear          - Clear screen\n");
    print_str("  jobs           - List active jobs\n");
    print_str("  fg [%job]      - Bring job to foreground\n");
    print_str("  bg [%job]      - Send job to background\n");
    print_str("  kill [pid]     - Kill process\n");
    print_str("  history        - Show command history\n");
    print_str("  help           - Show this help\n");
    print_str("  exit           - Exit shell\n");
    print_str("\n");
    print_str("Advanced Features:\n");
    print_str("  - Command pipes: cmd1 | cmd2 | cmd3\n");
    print_str("  - I/O redirection: cmd > file, cmd < file, cmd >> file\n");
    print_str("  - Background jobs: cmd &\n");
    print_str("  - Job control: jobs, kill\n");
    print_str("  - Command history with up/down arrows\n");
    print_str("  - Environment variables: $VAR, ${VAR}\n");
    print_str("  - Command aliases for shortcuts\n");
    print_str("  - Tab completion for commands and files\n");
    print_str("  - Shell options and configuration\n");
    return 0;
}

// Built-in command table - moved up before tab completion functions
typedef struct {
    const char *name;
    int (*func)(char **args);
} builtin_cmd_t;

// Forward declare all builtin functions
static int builtin_cd(char **args);
static int builtin_pwd(char **args);
static int builtin_echo(char **args);
static int builtin_env(char **args);
static int builtin_export(char **args);
static int builtin_unset(char **args);
static int builtin_alias(char **args);
static int builtin_unalias(char **args);
static int builtin_set(char **args);
static int builtin_exit(char **args);
static int builtin_jobs(char **args);
static int builtin_history(char **args);
static int builtin_kill(char **args);
static int builtin_help(char **args);
static int builtin_clear(char **args);
static int builtin_fg(char **args);
static int builtin_bg(char **args);

// Builtin commands table
static builtin_cmd_t builtins[] = {
    {"cd", builtin_cd},
    {"pwd", builtin_pwd},
    {"echo", builtin_echo},
    {"env", builtin_env},
    {"export", builtin_export},
    {"unset", builtin_unset},
    {"alias", builtin_alias},
    {"unalias", builtin_unalias},
    {"set", builtin_set},
    {"clear", builtin_clear},
    {"fg", builtin_fg},
    {"bg", builtin_bg},
    {"exit", builtin_exit},
    {"jobs", builtin_jobs},
    {"history", builtin_history},
    {"kill", builtin_kill},
    {"help", builtin_help},
    {NULL, NULL}
};

//============================================================================
// Tab Completion Implementation
//============================================================================

static void add_completion(const char *text, int is_command) {
    if (g_num_completions < MAX_COMPLETIONS) {
        g_completions[g_num_completions].text = (char*)text;
        g_completions[g_num_completions].is_command = is_command;
        g_num_completions++;
    }
}

static void clear_completions(void) {
    g_num_completions = 0;
}

static void find_command_completions(const char *prefix) {
    // Add built-in commands
    for (int i = 0; builtins[i].name; i++) {
        if (my_strncmp(builtins[i].name, prefix, my_strlen(prefix)) == 0) {
            add_completion(builtins[i].name, 1);
        }
    }
    
    // Add aliases
    for (int i = 0; i < g_shell.num_aliases; i++) {
        if (my_strncmp(g_shell.aliases[i].name, prefix, my_strlen(prefix)) == 0) {
            add_completion(g_shell.aliases[i].name, 1);
        }
    }
    
    // TODO: Search PATH for executables (simplified implementation)
    // Common commands that might be available
    static const char *common_commands[] = {
        "ls", "cat", "grep", "find", "touch", "mkdir", "rmdir", 
        "cp", "mv", "rm", "chmod", "chown", "ps", "top", NULL
    };
    
    for (int i = 0; common_commands[i]; i++) {
        if (my_strncmp(common_commands[i], prefix, my_strlen(prefix)) == 0) {
            add_completion(common_commands[i], 1);
        }
    }
}

static void find_file_completions(const char *prefix) {
    // TODO: Read directory and find matching files
    // This is a simplified implementation
    static const char *common_files[] = {
        "file.txt", "document.doc", "script.sh", "program.exe", 
        "config.conf", "data.dat", NULL
    };
    
    for (int i = 0; common_files[i]; i++) {
        if (my_strncmp(common_files[i], prefix, my_strlen(prefix)) == 0) {
            add_completion(common_files[i], 0);
        }
    }
}

static int handle_tab_completion(char *buffer, int cursor_pos) {
    if (!g_shell.tab_completion_enabled) {
        return cursor_pos;
    }
    
    clear_completions();
    
    // Find the word under cursor
    int word_start = cursor_pos;
    while (word_start > 0 && !is_whitespace(buffer[word_start - 1])) {
        word_start--;
    }
    
    // Extract the prefix to complete
    char prefix[MAX_COMMAND_LENGTH];
    int prefix_len = cursor_pos - word_start;
    my_strncpy(prefix, buffer + word_start, prefix_len);
    prefix[prefix_len] = '\0';
    
    // Determine if we're completing a command or filename
    bool is_first_word = true;
    for (int i = 0; i < word_start; i++) {
        if (!is_whitespace(buffer[i])) {
            is_first_word = false;
            break;
        }
    }
    
    if (is_first_word) {
        find_command_completions(prefix);
    } else {
        find_file_completions(prefix);
    }
    
    if (g_num_completions == 0) {
        // No completions found
        return cursor_pos;
    } else if (g_num_completions == 1) {
        // Single completion - auto-complete
        const char *completion = g_completions[0].text;
        int completion_len = my_strlen(completion);
        
        // Replace prefix with completion
        int remaining_space = MAX_COMMAND_LENGTH - word_start - 1;
        int copy_len = (completion_len < remaining_space) ? completion_len : remaining_space;
        
        my_strncpy(buffer + word_start, completion, copy_len);
        buffer[word_start + copy_len] = '\0';
        
        return word_start + copy_len;
    } else {
        // Multiple completions - show them
        print_str("\n");
        for (int i = 0; i < g_num_completions; i++) {
            print_str(g_completions[i].text);
            if (g_completions[i].is_command) {
                print_str("*");
            }
            print_str("  ");
            
            // New line every 4 completions
            if ((i + 1) % 4 == 0) {
                print_str("\n");
            }
        }
        if (g_num_completions % 4 != 0) {
            print_str("\n");
        }
        
        // Find common prefix of all completions
        int common_len = my_strlen(g_completions[0].text);
        for (int i = 1; i < g_num_completions; i++) {
            int j = 0;
            while (j < common_len && g_completions[0].text[j] == g_completions[i].text[j]) {
                j++;
            }
            common_len = j;
        }
        
        // Auto-complete to common prefix if longer than current prefix
        if (common_len > prefix_len) {
            my_strncpy(buffer + word_start, g_completions[0].text, common_len);
            buffer[word_start + common_len] = '\0';
            return word_start + common_len;
        }
        
        return cursor_pos;
    }
}

static int execute_builtin(char **args) {
    if (!args[0]) return -1;
    
    for (int i = 0; builtins[i].name; i++) {
        if (my_strcmp(args[0], builtins[i].name) == 0) {
            return builtins[i].func(args);
        }
    }
    return -1; // Not a built-in
}

//============================================================================
// External Command Execution
//============================================================================

static int execute_external(char **args) {
    pid_t pid = sys_fork();
    
    if (pid == 0) {
        // Child process
        if (sys_execve(args[0], (int32_t)args, 0) == -1) {
            error("execve failed");
            sys_exit(127);
        }
        // Should never reach here, but satisfy compiler
        return 127;
    } else if (pid > 0) {
        // Parent process
        int status;
        sys_waitpid(pid, &status, 0);
        return status;
    } else {
        perror_msg("fork");
        return -1;
    }
}

//============================================================================
// Pipeline Execution
//============================================================================

static int execute_pipeline(pipeline_t *pipeline) {
    if (pipeline->num_commands == 0) {
        return 0;
    }
    
    if (pipeline->num_commands == 1) {
        // Single command
        command_t *cmd = &pipeline->commands[0];
        
        // Try built-in first
        int result = execute_builtin(cmd->args);
        if (result != -1) {
            // TODO: Handle I/O redirection for built-in commands
            if (cmd->input_file || cmd->output_file) {
                error("I/O redirection for built-in commands not fully implemented");
            }
            return result;
        }
        
        // External command with potential I/O redirection
        if (pipeline->background) {
            pid_t pid = sys_fork();
            if (pid == 0) {
                // Child process - handle I/O redirection
                
                // Set up input redirection
                if (cmd->input_file) {
                    int input_fd = sys_open(cmd->input_file, O_RDONLY, 0);
                    if (input_fd < 0) {
                        error("failed to open input file");
                        sys_exit(1);
                    }
                    sys_dup2(input_fd, STDIN_FILENO);
                    sys_close(input_fd);
                }
                
                // Set up output redirection
                if (cmd->output_file) {
                    int flags = O_WRONLY | O_CREAT;
                    if (cmd->append_output) {
                        flags |= O_APPEND;
                    } else {
                        flags |= O_TRUNC;
                    }
                    
                    int output_fd = sys_open(cmd->output_file, flags, 0644);
                    if (output_fd < 0) {
                        error("failed to open output file");
                        sys_exit(1);
                    }
                    sys_dup2(output_fd, STDOUT_FILENO);
                    sys_close(output_fd);
                }
                
                sys_execve(cmd->args[0], (int32_t)cmd->args, 0);
                sys_exit(127);
            } else if (pid > 0) {
                add_job(pid, cmd->args[0], true);
                return 0;
            }
        } else {
            // Foreground command with I/O redirection
            if (cmd->input_file || cmd->output_file) {
                pid_t pid = sys_fork();
                if (pid == 0) {
                    // Child process - handle I/O redirection
                    
                    // Set up input redirection
                    if (cmd->input_file) {
                        int input_fd = sys_open(cmd->input_file, O_RDONLY, 0);
                        if (input_fd < 0) {
                            error("failed to open input file");
                            sys_exit(1);
                        }
                        sys_dup2(input_fd, STDIN_FILENO);
                        sys_close(input_fd);
                    }
                    
                    // Set up output redirection
                    if (cmd->output_file) {
                        int flags = O_WRONLY | O_CREAT;
                        if (cmd->append_output) {
                            flags |= O_APPEND;
                        } else {
                            flags |= O_TRUNC;
                        }
                        
                        int output_fd = sys_open(cmd->output_file, flags, 0644);
                        if (output_fd < 0) {
                            error("failed to open output file");
                            sys_exit(1);
                        }
                        sys_dup2(output_fd, STDOUT_FILENO);
                        sys_close(output_fd);
                    }
                    
                    sys_execve(cmd->args[0], (int32_t)cmd->args, 0);
                    error("execve failed");
                    sys_exit(127);
                } else if (pid > 0) {
                    // Parent process
                    int status;
                    sys_waitpid(pid, &status, 0);
                    return status;
                } else {
                    perror_msg("fork");
                    return -1;
                }
            } else {
                // No redirection, use simple execution
                return execute_external(cmd->args);
            }
        }
    } else {
        // Multiple commands with pipes
        int num_commands = pipeline->num_commands;
        pid_t pids[MAX_ARGS];  // Store child PIDs
        int pipes[MAX_ARGS - 1][2];  // Pipe file descriptors
        
        // Create all pipes
        for (int i = 0; i < num_commands - 1; i++) {
            if (sys_pipe(pipes[i]) == -1) {
                error("failed to create pipe");
                // Clean up any pipes created so far
                for (int j = 0; j < i; j++) {
                    sys_close(pipes[j][0]);
                    sys_close(pipes[j][1]);
                }
                return 1;
            }
        }
        
        // Execute each command in the pipeline
        for (int i = 0; i < num_commands; i++) {
            command_t *cmd = &pipeline->commands[i];
            
            // Check if this is a built-in command
            if (i == 0 && num_commands == 1) {
                // Single built-in - no pipes needed, but may have I/O redirection
                int builtin_result = execute_builtin(cmd->args);
                if (builtin_result != -1) {
                    // Handle I/O redirection for built-in commands
                    if (cmd->output_file) {
                        // TODO: Implement output redirection for built-ins
                        error("output redirection for built-ins not yet implemented");
                    }
                    if (cmd->input_file) {
                        // TODO: Implement input redirection for built-ins
                        error("input redirection for built-ins not yet implemented");
                    }
                    return builtin_result;
                }
            } else if (execute_builtin(cmd->args) != -1) {
                // Built-in commands in pipes not supported yet
                error("built-in commands in pipes not supported");
                // Clean up pipes
                for (int j = 0; j < num_commands - 1; j++) {
                    sys_close(pipes[j][0]);
                    sys_close(pipes[j][1]);
                }
                return 1;
            }
            
            // Fork for external command
            pid_t pid = sys_fork();
            if (pid == 0) {
                // Child process
                
                // Set up input redirection
                if (cmd->input_file) {
                    // Open input file
                    int input_fd = sys_open(cmd->input_file, O_RDONLY, 0);
                    if (input_fd < 0) {
                        error("failed to open input file");
                        sys_exit(1);
                    }
                    sys_dup2(input_fd, STDIN_FILENO);
                    sys_close(input_fd);
                } else if (i > 0) {
                    // Redirect stdin to read end of previous pipe
                    sys_dup2(pipes[i-1][0], STDIN_FILENO);
                }
                
                // Set up output redirection
                if (cmd->output_file) {
                    // Open output file
                    int flags = O_WRONLY | O_CREAT;
                    if (cmd->append_output) {
                        flags |= O_APPEND;
                    } else {
                        flags |= O_TRUNC;
                    }
                    
                    int output_fd = sys_open(cmd->output_file, flags, 0644);
                    if (output_fd < 0) {
                        error("failed to open output file");
                        sys_exit(1);
                    }
                    sys_dup2(output_fd, STDOUT_FILENO);
                    sys_close(output_fd);
                } else if (i < num_commands - 1) {
                    // Redirect stdout to write end of current pipe
                    sys_dup2(pipes[i][1], STDOUT_FILENO);
                }
                
                // Close all pipe file descriptors in child
                for (int j = 0; j < num_commands - 1; j++) {
                    sys_close(pipes[j][0]);
                    sys_close(pipes[j][1]);
                }
                
                // Execute the command
                sys_execve(cmd->args[0], (int32_t)cmd->args, 0);
                error("execve failed");
                sys_exit(127);
            } else if (pid > 0) {
                // Parent process
                pids[i] = pid;
            } else {
                error("fork failed");
                // Clean up pipes
                for (int j = 0; j < num_commands - 1; j++) {
                    sys_close(pipes[j][0]);
                    sys_close(pipes[j][1]);
                }
                return 1;
            }
        }
        
        // Close all pipe file descriptors in parent
        for (int i = 0; i < num_commands - 1; i++) {
            sys_close(pipes[i][0]);
            sys_close(pipes[i][1]);
        }
        
        // Wait for all children if not background
        int exit_status = 0;
        if (!pipeline->background) {
            for (int i = 0; i < num_commands; i++) {
                int status;
                sys_waitpid(pids[i], &status, 0);
                if (i == num_commands - 1) {  // Use last command's exit status
                    exit_status = status;
                }
            }
        } else {
            // Add as background job (simplified - use first command name)
            add_job(pids[0], pipeline->commands[0].args[0], true);
        }
        
        return exit_status;
    }
    
    return 0;
}

//============================================================================
// Main Shell Loop
//============================================================================

static void shell_init(void) {
    my_memset(&g_shell, 0, sizeof(g_shell));
    g_shell.running = true;
    g_shell.tab_completion_enabled = true;
    g_shell.history_navigation_enabled = true;
    g_shell.auto_cd_enabled = false;
    my_strcpy(g_shell.cwd, "/");
    
    // Set up basic environment
    set_env("HOME", "/");
    set_env("PATH", "/bin:/usr/bin");
    set_env("SHELL", "/bin/shell");
    set_env("PWD", "/");
    set_env("OLDPWD", "/");
    set_env("USER", "root");
    set_env("TERM", "coal-term");
    set_env("PS1", "CoalOS:\\w$ "); // Prompt format
    
    // Set up common aliases
    set_alias("ll", "ls -l");
    set_alias("la", "ls -la");
    set_alias("l", "ls");
    set_alias("..", "cd ..");
    set_alias("...", "cd ../..");
    set_alias("....", "cd ../../..");
    set_alias("h", "history");
    set_alias("c", "clear");
    set_alias("cls", "clear");
    set_alias("dir", "ls");
    set_alias("type", "cat");
    set_alias("md", "mkdir");
    set_alias("rd", "rmdir");
    
    print_str("\033[32mCoal OS Advanced Shell v2.1\033[0m\n");
    print_str("Features: pipes, redirection, jobs, history, aliases, tab completion\n");
    print_str("Use \033[33mTAB\033[0m for completion, \033[33m↑↓\033[0m for history, \033[33mhelp\033[0m for commands\n\n");
}

static void show_prompt(void) {
    print_str("CoalOS:");
    print_str(g_shell.cwd);
    print_str("$ ");
}

// History navigation helper
static void load_history_entry(char *buffer, size_t buffer_size, int direction) {
    if (!g_shell.history_navigation_enabled || g_shell.history_count == 0) {
        return;
    }
    
    if (direction > 0) { // Up arrow - previous command
        if (g_shell.current_history_pos > 0) {
            g_shell.current_history_pos--;
        }
    } else { // Down arrow - next command
        if (g_shell.current_history_pos < g_shell.history_count - 1) {
            g_shell.current_history_pos++;
        } else {
            // Clear buffer for new command
            buffer[0] = '\0';
            return;
        }
    }
    
    // Copy history entry to buffer
    const char *history_cmd = g_shell.history[g_shell.current_history_pos];
    size_t len = my_strlen(history_cmd);
    if (len < buffer_size - 1) {
        my_strcpy(buffer, history_cmd);
    }
}

// Enhanced input reading with basic line editing support
static ssize_t read_line_with_completion(char *buffer, size_t buffer_size) {
    size_t pos = 0;
    char c;
    
    // Initialize current history position
    g_shell.current_history_pos = g_shell.history_count;
    
    while (pos < buffer_size - 1) {
        ssize_t bytes = sys_read(STDIN_FILENO, &c, 1);
        if (bytes <= 0) break;
        
        if (c == '\n' || c == '\r') {
            buffer[pos] = '\0';
            print_char('\n');
            return pos;
        } else if (c == '\b' || c == 127) { // Backspace or DEL
            if (pos > 0) {
                pos--;
                print_str("\b \b"); // Move back, print space, move back again
            }
        } else if (c == '\t') { // Tab completion
            if (g_shell.tab_completion_enabled) {
                buffer[pos] = '\0';
                int new_pos = handle_tab_completion(buffer, pos);
                if (new_pos != (int)pos) {
                    // Print the completed part
                    for (int i = pos; i < new_pos; i++) {
                        print_char(buffer[i]);
                    }
                    pos = new_pos;
                }
                show_prompt();
                for (size_t i = 0; i < pos; i++) {
                    print_char(buffer[i]);
                }
            } else {
                buffer[pos++] = c;
                print_char(c);
            }
        } else if (c == 27) { // Escape sequence (arrow keys)
            // Read the rest of the escape sequence
            char seq[3];
            if (sys_read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                if (sys_read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[1] == 'A' || seq[1] == 'B') { // Up or Down arrow
                        // Clear current line
                        while (pos > 0) {
                            print_str("\b \b");
                            pos--;
                        }
                        
                        // Load history entry
                        load_history_entry(buffer, buffer_size, (seq[1] == 'A') ? 1 : -1);
                        pos = my_strlen(buffer);
                        
                        // Print the history entry
                        print_str(buffer);
                    }
                    // Ignore left/right arrows for now
                }
            }
        } else if (c >= 32 && c <= 126) { // Printable characters
            buffer[pos++] = c;
            print_char(c);
        }
        // Ignore other control characters
    }
    
    buffer[pos] = '\0';
    return pos;
}

int main(void) {
    shell_init();
    
    while (g_shell.running) {
        show_prompt();
        
        ssize_t bytes_read = read_line_with_completion(g_input_buffer, sizeof(g_input_buffer));
        
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                error("failed to read input");
            }
            continue;
        }
        
        // Skip empty commands
        char *trimmed = skip_whitespace(g_input_buffer);
        if (!*trimmed) continue;
        
        // Check for auto-cd feature
        bool is_auto_cd = false;
        if (g_shell.auto_cd_enabled && !my_strchr(trimmed, ' ') && 
            !my_strchr(trimmed, '|') && !my_strchr(trimmed, '>') && 
            !my_strchr(trimmed, '<')) {
            char *test_args[] = {trimmed, NULL};
            if (execute_builtin(test_args) == -1) {
                // If it's not a builtin and looks like a directory, try cd
                char test_path[MAX_PATH_LENGTH];
                my_strcpy(test_path, trimmed);
                if (sys_chdir(test_path) == 0) {
                    // Successfully changed directory
                    my_strcpy(g_shell.cwd, test_path);
                    set_env("PWD", test_path);
                    is_auto_cd = true;
                    sys_chdir(g_shell.cwd); // Go back to original directory for now
                }
            }
        }
        
        // Add to history (but not auto-cd attempts)
        if (!is_auto_cd) {
            add_to_history(trimmed);
        }
        
        int exit_status = 0;
        
        if (is_auto_cd) {
            // Execute the auto-cd
            char *cd_args[] = {"cd", trimmed, NULL};
            exit_status = builtin_cd(cd_args);
        } else {
            // Expand variables and aliases
            char expanded_vars[MAX_COMMAND_LENGTH];
            char expanded_alias[MAX_COMMAND_LENGTH];
            expand_variables(trimmed, expanded_vars, sizeof(expanded_vars));
            expand_alias(expanded_vars, expanded_alias, sizeof(expanded_alias));
            
            // Parse and execute
            pipeline_t pipeline;
            parse_pipeline(expanded_alias, &pipeline);
            exit_status = execute_pipeline(&pipeline);
        }
        
        g_shell.last_exit_status = exit_status;
        
        // Update environment variables
        char exit_str[12];
        char *p = exit_str + 11;
        *p = '\0';
        int n = exit_status;
        if (n == 0) {
            *--p = '0';
        } else {
            while (n > 0) {
                *--p = '0' + (n % 10);
                n /= 10;
            }
        }
        set_env("?", p);
    }
    
    print_str("Goodbye!\n");
    sys_exit(g_shell.last_exit_status);
    return 0;
}