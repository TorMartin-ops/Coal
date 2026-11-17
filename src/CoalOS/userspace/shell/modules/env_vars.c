/**
 * @file env_vars.c
 * @brief Environment variables and $-expansion
 */

#include "env_vars.h"

void env_init(void) {
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("HOME", "/", 1);
    setenv("SHELL", "/bin/shell.elf", 1);
    setenv("USER", "root", 1);
    setenv("TERM", "ansi", 1);
    setenv("PS1", "\\w$ ", 1);
}

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

/* Append s to the output, stopping at end. Returns the new write position. */
static char *append(char *dst, char *end, const char *s) {
    while (*s && dst < end) *dst++ = *s++;
    return dst;
}

void expand_variables(const char *input, char *output, size_t output_size) {
    if (output_size == 0) return;

    const char *src = input;
    char *dst = output;
    char *end = output + output_size - 1;

    bool in_single_quotes = false;
    bool in_double_quotes = false;

    while (*src && dst < end) {
        /* Quotes and backslashes are kept for the parser; they only decide
         * here whether a '$' is expanded. */
        if (*src == '\'' && !in_double_quotes) in_single_quotes = !in_single_quotes;
        if (*src == '"' && !in_single_quotes) in_double_quotes = !in_double_quotes;
        if (*src == '\\' && src[1] && !in_single_quotes) {
            *dst++ = *src++;            /* keep the backslash and the next char */
            if (dst < end) *dst++ = *src++;
            continue;
        }
        if (*src != '$' || in_single_quotes) {
            *dst++ = *src++;
            continue;
        }

        char name[64];
        size_t len = 0;
        const char *after;

        if (src[1] == '{') {
            const char *close = strchr(src + 2, '}');
            if (!close) {            /* no closing brace: keep the text */
                *dst++ = *src++;
                continue;
            }
            len = (size_t)(close - (src + 2));
            after = close + 1;
            if (len >= sizeof(name)) {   /* name too long: expands to nothing */
                src = after;
                continue;
            }
            memcpy(name, src + 2, len);
        } else if (src[1] == '?') {
            char status[16];
            snprintf(status, sizeof(status), "%d", g_shell.last_exit_status);
            dst = append(dst, end, status);
            src += 2;
            continue;
        } else if (is_name_start(src[1])) {
            const char *p = src + 1;
            while (is_name_char(*p)) p++;
            len = (size_t)(p - (src + 1));
            after = p;
            if (len >= sizeof(name)) {
                src = after;
                continue;
            }
            memcpy(name, src + 1, len);
        } else {                      /* a lone '$' */
            *dst++ = *src++;
            continue;
        }

        name[len] = '\0';
        const char *value = getenv(name);
        if (value) dst = append(dst, end, value);
        src = after;
    }

    *dst = '\0';
}

static void show_env(void) {
    for (char **e = environ; e && *e; e++) {
        printf("%s\n", *e);
    }
}

int builtin_export(char **args) {
    if (!args[1]) {
        show_env();
        return 0;
    }

    for (int i = 1; args[i]; i++) {
        char *equals = strchr(args[i], '=');
        int rc;
        if (equals) {
            /* setenv copies, so split a local copy of the name */
            char name[64];
            size_t n = (size_t)(equals - args[i]);
            if (n == 0 || n >= sizeof(name)) {
                shell_error("export: invalid name");
                return 1;
            }
            memcpy(name, args[i], n);
            name[n] = '\0';
            rc = setenv(name, equals + 1, 1);
        } else {
            rc = setenv(args[i], "", 0);
        }
        if (rc < 0) {
            shell_error("export: cannot set variable");
            return 1;
        }
    }
    return 0;
}

int builtin_unset(char **args) {
    if (!args[1]) {
        shell_error("unset: missing variable name");
        return 1;
    }
    for (int i = 1; args[i]; i++) unsetenv(args[i]);
    return 0;
}

int builtin_env(char **args) {
    (void)args;
    show_env();
    return 0;
}
