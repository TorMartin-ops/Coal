/**
 * @file string_utils.h
 * @brief String utility functions for Coal OS Shell
 * @version 1.0
 * @author Modular refactoring
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include "../shell_types.h"

//============================================================================
// String Functions
//============================================================================

/**
 * @brief Calculate string length
 * @param s String to measure
 * @return Length of string
 */
size_t my_strlen(const char *s);

/**
 * @brief Compare two strings
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
 */
int my_strcmp(const char *s1, const char *s2);

/**
 * @brief Compare two strings up to n characters
 * @param s1 First string
 * @param s2 Second string
 * @param n Maximum characters to compare
 * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
 */
int my_strncmp(const char *s1, const char *s2, size_t n);

/**
 * @brief Copy string
 * @param dest Destination buffer
 * @param src Source string
 * @return Pointer to dest
 */
char *my_strcpy(char *dest, const char *src);

/**
 * @brief Copy string up to n characters
 * @param dest Destination buffer
 * @param src Source string
 * @param n Maximum characters to copy
 * @return Pointer to dest
 */
char *my_strncpy(char *dest, const char *src, size_t n);

/**
 * @brief Concatenate strings
 * @param dest Destination string
 * @param src Source string to append
 * @return Pointer to dest
 */
char *my_strcat(char *dest, const char *src);

/**
 * @brief Find character in string
 * @param s String to search
 * @param c Character to find
 * @return Pointer to first occurrence or NULL
 */
char *my_strchr(const char *s, int c);

/**
 * @brief Set memory to value
 * @param s Memory to set
 * @param c Value to set
 * @param n Number of bytes
 * @return Pointer to s
 */
void *my_memset(void *s, int c, size_t n);

/**
 * @brief Check if character is whitespace
 * @param c Character to check
 * @return true if whitespace, false otherwise
 */
bool is_whitespace(char c);

/**
 * @brief Trim leading and trailing whitespace
 * @param str String to trim (modified in place)
 * @return Pointer to trimmed string
 */
char *trim_whitespace(char *str);

#endif // STRING_UTILS_H