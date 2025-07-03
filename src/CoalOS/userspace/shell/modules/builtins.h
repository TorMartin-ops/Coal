/**
 * @file builtins.h
 * @brief Built-in commands for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef BUILTINS_H
#define BUILTINS_H

#include "../shell_types.h"

//============================================================================
// Built-in Command Structure
//============================================================================

typedef struct {
    const char *name;
    int (*function)(char **args);
    const char *help;
} builtin_cmd_t;

//============================================================================
// Built-in Command Functions
//============================================================================

/**
 * @brief Change directory
 */
int builtin_cd(char **args);

/**
 * @brief Print working directory
 */
int builtin_pwd(char **args);

/**
 * @brief Exit shell
 */
int builtin_exit(char **args);

/**
 * @brief Echo arguments
 */
int builtin_echo(char **args);

/**
 * @brief Clear screen
 */
int builtin_clear(char **args);

/**
 * @brief Show command history
 */
int builtin_history(char **args);

/**
 * @brief Show help
 */
int builtin_help(char **args);

/**
 * @brief Show/manage jobs
 */
int builtin_jobs(char **args);

/**
 * @brief Bring job to foreground
 */
int builtin_fg(char **args);

/**
 * @brief Send job to background
 */
int builtin_bg(char **args);

/**
 * @brief Kill a process
 */
int builtin_kill(char **args);

/**
 * @brief Simple file listing
 */
int builtin_ls(char **args);

/**
 * @brief Display file contents
 */
int builtin_cat(char **args);

/**
 * @brief Create directory
 */
int builtin_mkdir(char **args);

/**
 * @brief Remove directory
 */
int builtin_rmdir(char **args);

/**
 * @brief Remove file
 */
int builtin_rm(char **args);

/**
 * @brief Create empty file
 */
int builtin_touch(char **args);

/**
 * @brief Check if command is a built-in
 * @param cmd Command name
 * @return Built-in command entry or NULL
 */
const builtin_cmd_t *find_builtin(const char *cmd);

/**
 * @brief Execute built-in command
 * @param cmd Built-in command entry
 * @param args Command arguments
 * @return Command exit status
 */
int execute_builtin(const builtin_cmd_t *cmd, char **args);

/**
 * @brief Get list of all built-in commands
 * @return Array of built-in commands (NULL terminated)
 */
const builtin_cmd_t *get_builtins(void);

#endif // BUILTINS_H