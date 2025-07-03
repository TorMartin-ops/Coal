/**
 * @file env_vars.c
 * @brief Environment variable management implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "env_vars.h"
#include "string_utils.h"
#include "io_utils.h"
#include "syscall_wrapper.h"

// Import global shell state
extern shell_state_t g_shell;

//============================================================================
// Internal Storage
//============================================================================

// Static storage for environment variable strings
static char env_storage[MAX_ENV_VARS][2][256]; // name and value for each var

//============================================================================
// Environment Variable Functions
//============================================================================

void env_init(void) {
    g_shell.num_env_vars = 0;
    
    // Set default environment variables
    set_env("PATH", "/bin:/usr/bin");
    set_env("HOME", "/");
    set_env("SHELL", "/bin/shell");
    set_env("USER", "root");
    set_env("TERM", "ansi");
    
    // Set prompt
    set_env("PS1", "$ ");
}

char *get_env(const char *name) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            return g_shell.env_vars[i].value;
        }
    }
    return NULL;
}

void set_env(const char *name, const char *value) {
    // Check if variable already exists
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            // Update existing variable
            my_strcpy(env_storage[i][1], value);
            g_shell.env_vars[i].value = env_storage[i][1];
            return;
        }
    }
    
    // Add new variable
    if (g_shell.num_env_vars < MAX_ENV_VARS) {
        int idx = g_shell.num_env_vars;
        my_strcpy(env_storage[idx][0], name);
        my_strcpy(env_storage[idx][1], value);
        g_shell.env_vars[idx].name = env_storage[idx][0];
        g_shell.env_vars[idx].value = env_storage[idx][1];
        g_shell.num_env_vars++;
    } else {
        error("environment variable table full");
    }
}

void unset_env(const char *name) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        if (my_strcmp(g_shell.env_vars[i].name, name) == 0) {
            // Remove by shifting remaining entries
            for (int j = i; j < g_shell.num_env_vars - 1; j++) {
                g_shell.env_vars[j] = g_shell.env_vars[j + 1];
                my_strcpy(env_storage[j][0], env_storage[j + 1][0]);
                my_strcpy(env_storage[j][1], env_storage[j + 1][1]);
                g_shell.env_vars[j].name = env_storage[j][0];
                g_shell.env_vars[j].value = env_storage[j][1];
            }
            g_shell.num_env_vars--;
            return;
        }
    }
}

void show_env(void) {
    for (int i = 0; i < g_shell.num_env_vars; i++) {
        print_str(g_shell.env_vars[i].name);
        print_str("=");
        print_str(g_shell.env_vars[i].value);
        print_str("\n");
    }
}

char *expand_variables(const char *input, char *output, size_t output_size) {
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
        } else if (*src == '$' && *(src + 1) == '?') {
            // Special variable $? for last exit status
            src += 2;
            char status_str[12];
            char *p = status_str + 11;
            *p = '\0';
            int status = g_shell.last_exit_status;
            if (status == 0) {
                *--p = '0';
            } else {
                while (status > 0) {
                    *--p = '0' + (status % 10);
                    status /= 10;
                }
            }
            while (*p && dst < dst_end) {
                *dst++ = *p++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    
    *dst = '\0';
    return output;
}

//============================================================================
// Built-in Commands
//============================================================================

int builtin_export(char **args) {
    if (!args[1]) {
        // No arguments - show all exported variables
        show_env();
        return 0;
    }
    
    // Parse VAR=VALUE format
    char *equals = my_strchr(args[1], '=');
    if (equals) {
        *equals = '\0';
        set_env(args[1], equals + 1);
        *equals = '='; // Restore original string
    } else {
        // Just VAR without value - set to empty string
        set_env(args[1], "");
    }
    
    return 0;
}

int builtin_unset(char **args) {
    if (!args[1]) {
        error("unset: missing variable name");
        return 1;
    }
    
    unset_env(args[1]);
    return 0;
}

int builtin_env(char **args) {
    (void)args;
    show_env();
    return 0;
}