/**
 * @file parser.h
 * @brief Command parsing functionality for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef PARSER_H
#define PARSER_H

#include "../shell_types.h"

//============================================================================
// Parser Functions
//============================================================================

/**
 * @brief Parse command line into arguments
 * @param input Input command line
 * @param args Array to store argument pointers
 * @param max_args Maximum number of arguments
 * @return Number of arguments parsed
 */
int parse_arguments(char *input, char **args, int max_args);

/**
 * @brief Parse a pipeline command
 * @param input Input command line
 * @param pipeline Pipeline structure to fill
 * @return 0 on success, -1 on error
 */
int parse_pipeline(char *input, pipeline_t *pipeline);

/**
 * @brief Parse a single command with redirections
 * @param input Command string
 * @param cmd Command structure to fill
 * @return 0 on success, -1 on error
 */
int parse_command(char *input, command_t *cmd);

/**
 * @brief Check if a string contains shell metacharacters
 * @param str String to check
 * @return true if contains metacharacters
 */
bool has_metacharacters(const char *str);

/**
 * @brief Tokenize input respecting quotes and escapes
 * @param input Input string
 * @param tokens Array to store tokens
 * @param max_tokens Maximum number of tokens
 * @return Number of tokens
 */
int tokenize(char *input, char **tokens, int max_tokens);

/**
 * @brief Parse quotes and handle escape sequences
 * @param str String to parse
 * @param output Output buffer
 * @param output_size Size of output buffer
 * @return 0 on success, -1 on error
 */
int parse_quotes(const char *str, char *output, size_t output_size);

#endif // PARSER_H