/**
 * @file parser.c
 * @brief Command parsing implementation for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#include "parser.h"
#include "string_utils.h"
#include "io_utils.h"

//============================================================================
// Parser Implementation
//============================================================================

int parse_arguments(char *input, char **args, int max_args) {
    int argc = 0;
    char *p = input;
    bool in_quotes = false;
    bool in_single_quotes = false;
    
    if (!input || !args) return 0;
    
    // Skip leading whitespace
    while (is_whitespace(*p)) p++;
    
    while (*p && argc < max_args - 1) {
        // Start of new argument
        args[argc] = p;
        
        // Process until end of argument
        while (*p && (in_quotes || in_single_quotes || !is_whitespace(*p))) {
            if (*p == '\\' && *(p + 1)) {
                // Handle escape sequences
                p += 2;
            } else if (*p == '"' && !in_single_quotes) {
                in_quotes = !in_quotes;
                // Remove quotes by shifting
                char *src = p + 1;
                char *dst = p;
                while (*src) *dst++ = *src++;
                *dst = '\0';
            } else if (*p == '\'' && !in_quotes) {
                in_single_quotes = !in_single_quotes;
                // Remove quotes by shifting
                char *src = p + 1;
                char *dst = p;
                while (*src) *dst++ = *src++;
                *dst = '\0';
            } else {
                p++;
            }
        }
        
        if (*p) {
            *p++ = '\0'; // Null terminate argument
            argc++;
            
            // Skip whitespace between arguments
            while (is_whitespace(*p)) p++;
        } else if (args[argc][0] != '\0') {
            argc++; // Count last argument
        }
    }
    
    args[argc] = NULL; // Null terminate array
    return argc;
}

int parse_pipeline(char *input, pipeline_t *pipeline) {
    if (!input || !pipeline) return -1;
    
    pipeline->num_commands = 0;
    pipeline->background = false;
    
    // Check for background job indicator
    char *bg_marker = my_strchr(input, '&');
    if (bg_marker) {
        *bg_marker = '\0';
        pipeline->background = true;
    }
    
    // Split by pipes
    char *p = input;
    char *pipe_pos;
    
    while (p && pipeline->num_commands < MAX_ARGS) {
        pipe_pos = my_strchr(p, '|');
        if (pipe_pos) {
            *pipe_pos = '\0';
        }
        
        // Parse individual command
        if (parse_command(p, &pipeline->commands[pipeline->num_commands]) == 0) {
            pipeline->num_commands++;
        }
        
        if (pipe_pos) {
            p = pipe_pos + 1;
            // Skip whitespace after pipe
            while (is_whitespace(*p)) p++;
        } else {
            break;
        }
    }
    
    return (pipeline->num_commands > 0) ? 0 : -1;
}

int parse_command(char *input, command_t *cmd) {
    if (!input || !cmd) return -1;
    
    // Initialize command structure
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->append_output = false;
    cmd->background = false;
    cmd->argc = 0;
    
    // Temporary storage for parsing
    char temp[MAX_COMMAND_LENGTH];
    my_strcpy(temp, input);
    
    // Handle redirections
    char *p = temp;
    char *new_cmd = temp;
    char *write_pos = temp;
    
    while (*p) {
        if (*p == '<') {
            // Input redirection
            *p++ = '\0';
            while (is_whitespace(*p)) p++;
            cmd->input_file = p;
            while (*p && !is_whitespace(*p) && *p != '>' && *p != '<') p++;
            if (*p) *p++ = '\0';
        } else if (*p == '>' && *(p + 1) == '>') {
            // Append output redirection
            *p++ = '\0';
            *p++ = '\0';
            while (is_whitespace(*p)) p++;
            cmd->output_file = p;
            cmd->append_output = true;
            while (*p && !is_whitespace(*p) && *p != '>' && *p != '<') p++;
            if (*p) *p++ = '\0';
        } else if (*p == '>') {
            // Output redirection
            *p++ = '\0';
            while (is_whitespace(*p)) p++;
            cmd->output_file = p;
            cmd->append_output = false;
            while (*p && !is_whitespace(*p) && *p != '>' && *p != '<') p++;
            if (*p) *p++ = '\0';
        } else {
            // Regular character - copy to write position
            if (write_pos != p) {
                *write_pos = *p;
            }
            write_pos++;
            p++;
        }
    }
    *write_pos = '\0';
    
    // Parse arguments from the cleaned command
    cmd->argc = parse_arguments(new_cmd, cmd->args, MAX_ARGS);
    
    return (cmd->argc > 0) ? 0 : -1;
}

bool has_metacharacters(const char *str) {
    if (!str) return false;
    
    while (*str) {
        switch (*str) {
            case '|':
            case '<':
            case '>':
            case '&':
            case ';':
            case '(':
            case ')':
            case '*':
            case '?':
            case '[':
            case ']':
            case '$':
            case '`':
            case '\\':
            case '"':
            case '\'':
                return true;
        }
        str++;
    }
    return false;
}

int tokenize(char *input, char **tokens, int max_tokens) {
    return parse_arguments(input, tokens, max_tokens);
}

int parse_quotes(const char *str, char *output, size_t output_size) {
    if (!str || !output || output_size == 0) return -1;
    
    const char *src = str;
    char *dst = output;
    char *dst_end = output + output_size - 1;
    bool in_quotes = false;
    bool in_single_quotes = false;
    
    while (*src && dst < dst_end) {
        if (*src == '\\' && *(src + 1) && !in_single_quotes) {
            // Handle escape sequences
            src++; // Skip backslash
            switch (*src) {
                case 'n': *dst++ = '\n'; break;
                case 't': *dst++ = '\t'; break;
                case 'r': *dst++ = '\r'; break;
                case '\\': *dst++ = '\\'; break;
                case '"': *dst++ = '"'; break;
                case '\'': *dst++ = '\''; break;
                default: *dst++ = *src; break;
            }
            src++;
        } else if (*src == '"' && !in_single_quotes) {
            in_quotes = !in_quotes;
            src++;
        } else if (*src == '\'' && !in_quotes) {
            in_single_quotes = !in_single_quotes;
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    
    *dst = '\0';
    
    // Check for unclosed quotes
    if (in_quotes || in_single_quotes) {
        return -1;
    }
    
    return 0;
}