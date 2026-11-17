/**
 * @file env_vars.h
 * @brief Environment variables of the shell, stored in libc's environ so that
 *        child programs inherit them
 */

#ifndef ENV_VARS_H
#define ENV_VARS_H

#include "../shell_types.h"

/** Set the default variables (PATH, HOME, ...). */
void env_init(void);

/**
 * Expand $NAME, ${NAME} and $? in input. The result is always terminated;
 * text that does not fit is cut off. No expansion inside single quotes or
 * after a backslash.
 */
void expand_variables(const char *input, char *output, size_t output_size);

int builtin_export(char **args);
int builtin_unset(char **args);
int builtin_env(char **args);

#endif /* ENV_VARS_H */
