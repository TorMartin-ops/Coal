#pragma once
#ifndef STRING_H
#define STRING_H

#include "types.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sets the first n bytes of the memory area pointed to by dest to the constant byte c.
 * @param dest Pointer to the memory area.
 * @param c The byte value to set.
 * @param n Number of bytes to set.
 * @return Pointer to dest.
 */
void *memset(void *dest, int c, size_t n);

/**
 * @brief Copies n bytes from memory area src to memory area dest.
 * The memory areas must not overlap.
 * @param dest Destination memory area.
 * @param src Source memory area.
 * @param n Number of bytes to copy.
 * @return Pointer to dest.
 */
void *memcpy(void *dest, const void *src, size_t n);

/**
 * @brief Copies n bytes from memory area src to memory area dest.
 * The memory areas may overlap.
 * @param dest Destination memory area.
 * @param src Source memory area.
 * @param n Number of bytes to copy.
 * @return Pointer to dest.
 */
void *memmove(void *dest, const void *src, size_t n);

/**
 * @brief Scans the initial n bytes of the memory area pointed to by s for the first instance of c.
 * @param s Memory area to scan.
 * @param c Character to search for (converted to unsigned char).
 * @param n Number of bytes to scan.
 * @return Pointer to the matching byte or NULL if c does not occur.
 */
void *memchr(const void *s, int c, size_t n);

/**
 * @brief Compares n bytes of memory areas s1 and s2.
 * @param s1 First memory area.
 * @param s2 Second memory area.
 * @param n Number of bytes to compare.
 * @return Integer less than, equal to, or greater than zero if s1 is found,
 *         respectively, to be less than, to match, or be greater than s2.
 */
int memcmp(const void *s1, const void *s2, size_t n);

/**
 * @brief Computes the length of the string s (excluding the terminating null byte).
 * @param s Null-terminated string.
 * @return Number of characters in s.
 */
size_t strlen(const char *s);

/**
 * @brief Compares the two strings s1 and s2.
 * @param s1 First null-terminated string.
 * @param s2 Second null-terminated string.
 * @return An integer less than, equal to, or greater than zero if s1 is found,
 *         respectively, to be less than, to match, or be greater than s2.
 */
int strcmp(const char *s1, const char *s2);

/**
 * @brief Compares up to n characters of the two strings s1 and s2.
 * @param s1 First null-terminated string.
 * @param s2 Second null-terminated string.
 * @param n Maximum number of characters to compare.
 * @return An integer less than, equal to, or greater than zero if s1 is found,
 *         respectively, to be less than, to match, or be greater than s2.
 */
int strncmp(const char *s1, const char *s2, size_t n);

/**
 * @brief Copies the string pointed to by src (including the terminating null byte) to dest.
 * @param dest Destination buffer.
 * @param src Source string.
 * @return Pointer to dest.
 */
char *strcpy(char *dest, const char *src);

/**
 * @brief Copies up to n characters from the string pointed to by src to dest.
 * If src is less than n characters long, the remainder of dest is padded with '\0'.
 * @param dest Destination buffer.
 * @param src Source string.
 * @param n Maximum number of characters to copy.
 * @return Pointer to dest.
 */
char *strncpy(char *dest, const char *src, size_t n);

/**
 * @brief Appends the string pointed to by src to the end of the string pointed to by dest.
 * @param dest Destination buffer (must be large enough).
 * @param src Source string.
 * @return Pointer to dest.
 */
char *strcat(char *dest, const char *src);

/**
 * @brief Appends up to n characters from the string pointed to by src to the end of dest.
 * @param dest Destination buffer.
 * @param src Source string.
 * @param n Maximum number of characters to append.
 * @return Pointer to dest.
 */
char *strncat(char *dest, const char *src, size_t n);

/**
 * @brief Returns a pointer to the first occurrence of character c in the string s.
 * @param s String to search.
 * @param c Character to search for.
 * @return Pointer to the first occurrence of c, or NULL if c is not found.
 */
char *strchr(const char *s, int c);

/**
 * @brief Returns a pointer to the last occurrence of character c in the string s.
 * @param s String to search.
 * @param c Character to search for.
 * @return Pointer to the last occurrence of c, or NULL if c is not found.
 */
char *strrchr(const char *s, int c);

/**
 * @brief Returns the length of the initial segment of str which consists entirely of characters in accept.
 * @param str String to be scanned.
 * @param accept String containing the characters to match.
 * @return Number of characters in the initial segment of str which consist only of characters from accept.
 */
size_t strspn(const char *str, const char *accept);

/**
 * @brief Finds the first occurrence in str of any character in accept.
 * @param str String to be scanned.
 * @param accept String containing the characters to search for.
 * @return Pointer to the first occurrence or NULL if no character from accept is found.
 */
char *strpbrk(const char *str, const char *accept);

/**
 * @brief Breaks string into a series of tokens using the delimiter delim.
 * @param str String to tokenize. In first call, this is the string to tokenize.
 *        In subsequent calls, this should be NULL to continue tokenizing.
 * @param delim String containing delimiter characters.
 * @return Pointer to the next token, or NULL if there are no more tokens.
 *         Note: This function maintains internal state between calls.
 */
char *strtok(char *str, const char *delim);

#ifdef __cplusplus
}
#endif

#endif /* STRING_H */