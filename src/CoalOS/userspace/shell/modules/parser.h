/**
 * @file parser.h
 * @brief Command line parser
 */

#ifndef PARSER_H
#define PARSER_H

#include "../shell_types.h"

/**
 * Parse one command line up to the next unquoted ';' or the end of the input.
 *
 * Handles single and double quotes, backslash escapes, '|', '<', '>', '>>'
 * and a trailing '&'. All strings are copied into pipeline->buf, so the
 * input may be reused immediately.
 *
 * @param input    NUL-terminated command line (shorter than MAX_COMMAND_LENGTH)
 * @param pipeline Filled in on success. An empty line gives num_commands == 0.
 * @param rest     Set to the text after the ';' or to the terminating NUL
 * @return 0 on success, -1 on a syntax error
 */
int parse_pipeline(const char *input, pipeline_t *pipeline, const char **rest);

/** Remove leading and trailing white space in place; returns the new start. */
char *trim_whitespace(char *str);

#endif /* PARSER_H */
