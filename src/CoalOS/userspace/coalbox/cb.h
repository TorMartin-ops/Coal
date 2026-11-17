/* Helpers shared by the coalbox applets. */
#ifndef COALBOX_CB_H
#define COALBOX_CB_H

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>
#include <libc/fcntl.h>
#include <libc/errno.h>
#include <libc/ctype.h>
#include <libc/sys/stat.h>

extern const char *prog;                /* name of the running applet */

/* Prints "prog: what 'path': strerror(errno)" (or "prog: path: ..." when
 * what is NULL) to stderr and returns 1. */
int cb_err(const char *what, const char *path);
int cb_usage(const char *args);         /* "prog: usage: prog args", returns 1 */
int cb_write_all(int fd, const void *buf, size_t n);
char *cb_join(const char *dir, const char *name);
const char *cb_base(const char *path);
/* Reads a whole (small) file into buf, NUL terminated; returns length or -1. */
long cb_slurp(const char *path, char *buf, size_t cap);
/* Names in a directory except . and ..; NULL on error. Free with cb_free_names. */
char **cb_list_dir(const char *path, int *count);
void cb_free_names(char **names, int count);

#define DECL(n) int n##_main(int argc, char **argv);
#include "applets.h"
APPLET_LIST(DECL)
#undef DECL

#endif
