/* sh: a small POSIX-style shell. Pipelines, lists, if/while/until/for/case, functions, command
 * substitution, redirections and here-documents, wildcards, history and line editing on a raw
 * terminal. This file is the entry point and the interactive loop. */

#include "sh.h"
#include <libc/termios.h>

static void on_sigint(int sig) {}

static void make_prompt(char *prompt, size_t cap) {
    const char *ps1 = var_get("PS1");
    if (ps1) {
        snprintf(prompt, cap, "%s", ps1);
        return;
    }
    const char *user = var_get("USER"), *home = var_get("HOME");
    char cwd[256], tilde[260];
    const char *shown = cwd;
    size_t hl = home ? strlen(home) : 0;
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    if (hl > 1 && !strncmp(cwd, home, hl) && (!cwd[hl] || cwd[hl] == '/')) {
        snprintf(tilde, sizeof tilde, "~%s", cwd + hl);
        shown = tilde;
    }
    snprintf(prompt, cap, "%s@coal:%s$ ", user ? user : "user", shown);
}

static void print_motd(void) {
    char b[512];
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) return;
    ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0) write(1, b, (size_t)n);
    close(fd);
}

static void append(char **text, size_t *len, const char *s) {
    size_t n = strlen(s);
    *text = xrealloc(*text, *len + n + 2);
    memcpy(*text + *len, s, n);
    (*text)[*len += n] = '\n';
    (*text)[++*len] = 0;
}

static int interact(int login) {
    char prompt[300], line[256];
    interactive = 1;
    signal(SIGINT, on_sigint);                  /* survive Ctrl-C; children reset it on exec */
    if (login) print_motd();
    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) {}      /* reap finished background jobs */
        make_prompt(prompt, sizeof prompt);
        char *text = NULL;
        size_t len = 0;
        const char *p = prompt;
        int r;
        while ((r = edit_line(p, line)) == 1) {
            history_add(line);
            append(&text, &len, line);
            if (!sh_incomplete(text)) break;
            p = "> ";                           /* the command continues on the next line */
        }
        if (r == 0) {
            write(1, "exit\n", 5);
            return last_status;
        }
        if (r == 1) sh_run(text);
        free(text);
    }
}

int main(int argc, char **argv) {
    const char *command = NULL;
    int login = argv[0][0] == '-', i = 1;
    var_import();
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        int want_command = 0;
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'c') want_command = 1;
            else if (*o == 'e') opt_errexit = 1;
            else if (*o == 'x') opt_xtrace = 1;
            else if (*o == 'l') login = 1;
            else {
                fprintf(stderr, "sh: -%c: unsupported option\nusage: sh [-ex] [-c command [name]] [file] [args]\n", *o);
                return 2;
            }
        }
        if (want_command) {
            if (++i == argc) {
                fprintf(stderr, "sh: -c: option requires an argument\n");
                return 2;
            }
            command = argv[i++];
            break;
        }
    }
    char cwd[256];
    if (!var_get("PWD") && getcwd(cwd, sizeof cwd)) var_set("PWD", cwd);

    if (command) {
        if (i < argc) sh_name = argv[i++];
        set_params(argv + i, argc - i);
        exit(sh_run(command));
    }
    if (i < argc) {                             /* a script file */
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", argv[i], strerror(errno));
            return 127;
        }
        sh_name = argv[i++];
        set_params(argv + i, argc - i);
        char *text = read_all(fd);
        close(fd);
        exit(sh_run(text));
    }
    struct termios t;
    if (tcgetattr(0, &t) == 0) return interact(login);
    exit(sh_run(read_all(0)));
}
