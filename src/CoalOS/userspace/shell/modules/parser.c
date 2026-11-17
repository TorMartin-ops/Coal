/**
 * @file parser.c
 * @brief Command line parser
 */

#include "parser.h"

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char *trim_whitespace(char *str) {
    while (is_blank(*str)) str++;
    size_t len = strlen(str);
    while (len > 0 && is_blank(str[len - 1])) len--;
    str[len] = '\0';
    return str;
}

/* Parser state: reads from src, writes unquoted text to pipeline->buf. */
typedef struct {
    const char *src;
    char *out;          /* next free byte in buf */
    char *out_end;      /* last usable byte; one is kept for the final NUL */
} lexer_t;

/*
 * Read one word, removing quotes and backslashes. Returns a pointer to the
 * NUL-terminated copy in buf, or NULL if there is no word, the quotes are
 * unclosed or the buffer is full.
 */
static char *read_word(lexer_t *lx, bool *had_word) {
    char *start = lx->out;
    char quote = 0;
    *had_word = false;

    for (;;) {
        char c = *lx->src;
        if (c == '\0') break;
        if (!quote && (is_blank(c) || c == '|' || c == '<' || c == '>' ||
                       c == '&' || c == ';')) {
            break;
        }

        lx->src++;
        *had_word = true;
        if (quote) {
            if (c == quote) {
                quote = 0;
                continue;
            }
            /* Inside double quotes a backslash escapes \ and " only. */
            if (quote == '"' && c == '\\' && (*lx->src == '\\' || *lx->src == '"')) {
                c = *lx->src++;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
            continue;
        } else if (c == '\\' && *lx->src) {
            c = *lx->src++;
        }

        if (lx->out >= lx->out_end) return NULL;
        *lx->out++ = c;
    }

    if (quote) return NULL;
    if (lx->out >= lx->out_end + 1) return NULL;
    *lx->out++ = '\0';
    return start;
}

static void skip_blanks(lexer_t *lx) {
    while (is_blank(*lx->src)) lx->src++;
}

int parse_pipeline(const char *input, pipeline_t *pl, const char **rest) {
    lexer_t lx;
    lx.src = input;
    lx.out = pl->buf;
    lx.out_end = pl->buf + sizeof(pl->buf) - 2;

    pl->num_commands = 0;
    pl->background = false;

    command_t *cmd = NULL;   /* command being built, NULL between pipes */
    bool after_pipe = false;

    for (;;) {
        skip_blanks(&lx);
        char c = *lx.src;

        if (c == '\0' || c == ';') {
            if (c == ';') lx.src++;
            break;
        }

        if (c == '|') {
            if (!cmd || cmd->argc == 0) goto syntax_error;
            lx.src++;
            cmd = NULL;
            after_pipe = true;
            continue;
        }

        if (c == '&') {
            lx.src++;
            skip_blanks(&lx);
            if (!cmd || cmd->argc == 0) goto syntax_error;
            if (*lx.src != '\0' && *lx.src != ';') goto syntax_error;
            pl->background = true;
            continue;
        }

        if (!cmd) {
            if (pl->num_commands >= MAX_COMMANDS) goto syntax_error;
            cmd = &pl->commands[pl->num_commands++];
            memset(cmd, 0, sizeof(*cmd));
            after_pipe = false;
        }

        if (c == '<' || c == '>') {
            bool append = false;
            lx.src++;
            if (c == '>' && *lx.src == '>') {
                append = true;
                lx.src++;
            }
            skip_blanks(&lx);
            bool had_word;
            char *name = read_word(&lx, &had_word);
            if (!name || !had_word) goto syntax_error;
            if (c == '<') {
                cmd->input_file = name;
            } else {
                cmd->output_file = name;
                cmd->append_output = append;
            }
            continue;
        }

        bool had_word;
        char *word = read_word(&lx, &had_word);
        if (!word) goto syntax_error;
        if (cmd->argc >= MAX_ARGS - 1) goto syntax_error;
        cmd->args[cmd->argc++] = word;
        cmd->args[cmd->argc] = NULL;
    }

    if (after_pipe) goto syntax_error;
    /* A command with only redirections and no program name is an error. */
    for (int i = 0; i < pl->num_commands; i++) {
        if (pl->commands[i].argc == 0) goto syntax_error;
    }
    *rest = lx.src;
    return 0;

syntax_error:
    /* Skip to the end of the line so the caller does not run the remainder. */
    *rest = lx.src + strlen(lx.src);
    pl->num_commands = 0;
    return -1;
}
