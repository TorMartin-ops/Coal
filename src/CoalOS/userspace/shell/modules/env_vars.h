/**
 * @file env_vars.h
 * @brief Environment variable management for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef ENV_VARS_H
#define ENV_VARS_H

#include "../shell_types.h"

//============================================================================
// Environment Variable Functions
//============================================================================

/**
 * @brief Initialize environment variables subsystem
 */
void env_init(void);

/**
 * @brief Get environment variable value
 * @param name Variable name
 * @return Variable value or NULL if not found
 */
char *get_env(const char *name);

/**
 * @brief Set environment variable
 * @param name Variable name
 * @param value Variable value
 */
void set_env(const char *name, const char *value);

/**
 * @brief Unset environment variable
 * @param name Variable name to remove
 */
void unset_env(const char *name);

/**
 * @brief Display all environment variables
 */
void show_env(void);

/**
 * @brief Expand variables in a string
 * @param input Input string with potential variables
 * @param output Output buffer for expanded string
 * @param output_size Size of output buffer
 * @return Pointer to output buffer
 */
char *expand_variables(const char *input, char *output, size_t output_size);

/**
 * @brief Export environment variable (built-in command)
 * @param args Command arguments
 * @return 0 on success, non-zero on error
 */
int builtin_export(char **args);

/**
 * @brief Unset environment variable (built-in command)
 * @param args Command arguments
 * @return 0 on success, non-zero on error
 */
int builtin_unset(char **args);

/**
 * @brief Show environment variables (built-in command)
 * @param args Command arguments
 * @return 0 on success, non-zero on error
 */
int builtin_env(char **args);

#endif // ENV_VARS_H