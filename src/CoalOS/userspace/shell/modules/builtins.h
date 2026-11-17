/**
 * @file builtins.h
 * @brief Built-in commands
 */

#ifndef BUILTINS_H
#define BUILTINS_H

#include "../shell_types.h"

typedef struct {
    const char *name;
    int (*function)(char **args);
    const char *help;
} builtin_cmd_t;

/** The built-in called name, or NULL. */
const builtin_cmd_t *find_builtin(const char *name);

#endif /* BUILTINS_H */
