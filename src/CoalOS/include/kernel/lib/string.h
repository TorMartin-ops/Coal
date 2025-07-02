/**
 * @file string.h
 * @brief Standard C string and memory manipulation functions.
 */

 #ifndef STRING_H
 #define STRING_H
 
 // Include necessary standard types
 #include <libc/stddef.h> // For size_t, NULL
 #include <libc/stdint.h> // For standard integer types (used implicitly)
 
 /* --- Memory Manipulation Functions --- */
 
 /**
  * @brief Fills the first n bytes of the memory area pointed to by s
  * with the constant byte c.
  * @param s Pointer to the memory area.
  * @param c The byte value to fill with.
  * @param n The number of bytes to fill.
  * @return A pointer to the memory area s.
  */
 void *memset(void *s, int c, size_t n);
 
 /**
  * @brief Copies n bytes from memory area src to memory area dest.
  * The memory areas must not overlap. Use memmove if they might overlap.
  * @param dest Pointer to the destination memory area.
  * @param src Pointer to the source memory area.
  * @param n The number of bytes to copy.
  * @return A pointer to the destination memory area dest.
  *
  * @note If using GCC or Clang, this will typically use the highly optimized
  * compiler builtin for best performance. The C implementation below
  * serves as a fallback or reference.
  */
 #ifdef __GNUC__
 // Use compiler builtin for GCC/Clang for optimized memcpy
 #define memcpy __builtin_memcpy
 void *memcpy(void *dest, const void *src, size_t n); // Declaration still needed for type checking? Maybe not with define? Check compiler docs. Better safe than sorry.
 #else
 // Standard C declaration (implementation should be in string.c)
 void *memcpy(void *dest, const void *src, size_t n);
 #endif
 
 /**
  * @brief Copies n bytes from memory area src to memory area dest.
  * The memory areas may overlap; the copy is done in a non-destructive
  * manner.
  * @param dest Pointer to the destination memory area.
  * @param src Pointer to the source memory area.
  * @param n The number of bytes to copy.
  * @return A pointer to the destination memory area dest.
  */
 void *memmove(void *dest, const void *src, size_t n);
 
 /**
  * @brief Scans the initial n bytes of the memory area pointed to by s
  * for the first instance of c (interpreted as an unsigned char).
  * @param s Pointer to the memory area to scan.
  * @param c The value to search for.
  * @param n The number of bytes to scan.
  * @return A pointer to the matching byte or NULL if the character does not
  * occur in the given memory area.
  */
 void *memchr(const void *s, int c, size_t n);
 
 /**
  * @brief Compares the first n bytes of the memory areas s1 and s2.
  * @param s1 Pointer to the first memory area.
  * @param s2 Pointer to the second memory area.
  * @param n The number of bytes to compare.
  * @return An integer less than, equal to, or greater than zero if s1 is found,
  * respectively, to be less than, to match, or be greater than s2.
  */
 int memcmp(const void *s1, const void *s2, size_t n);
 
 
 /* --- String Manipulation Functions --- */
 
 /**
  * @brief Calculates the length of the string pointed to by s,
  * excluding the terminating null byte ('\0').
  * @param s Pointer to the null-terminated string.
  * @return The number of bytes in the string pointed to by s.
  */
 size_t strlen(const char *s);
 
 /**
  * @brief Compares the two strings s1 and s2.
  * @param s1 Pointer to the first null-terminated string.
  * @param s2 Pointer to the second null-terminated string.
  * @return An integer less than, equal to, or greater than zero if s1 is found,
  * respectively, to be less than, to match, or be greater than s2.
  */
 int strcmp(const char *s1, const char *s2);
 
 /**
  * @brief Compares at most the first n bytes of the two strings s1 and s2.
  * @param s1 Pointer to the first null-terminated string.
  * @param s2 Pointer to the second null-terminated string.
  * @param n The maximum number of bytes to compare.
  * @return An integer less than, equal to, or greater than zero if s1 (or its
  * first n bytes) is found, respectively, to be less than, to match,
  * or be greater than s2 (or its first n bytes).
  */
 int strncmp(const char *s1, const char *s2, size_t n);
 
 /**
  * @brief Copies the string pointed to by src, including the terminating null
  * byte ('\0'), to the buffer pointed to by dest. The strings may not
  * overlap, and the destination buffer must be large enough.
  * @param dest Pointer to the destination buffer.
  * @param src Pointer to the source null-terminated string.
  * @return A pointer to the destination string dest.
  */
 char *strcpy(char *dest, const char *src);
 
 /**
  * @brief Copies at most n bytes of the string pointed to by src to the
  * buffer pointed to by dest. If the length of src is less than n,
  * the remainder of dest up to n bytes is filled with null bytes.
  * Otherwise, dest is not null-terminated.
  * @param dest Pointer to the destination buffer.
  * @param src Pointer to the source null-terminated string.
  * @param n The maximum number of bytes to copy from src.
  * @return A pointer to the destination string dest.
  */
 char *strncpy(char *dest, const char *src, size_t n);
 
 /**
  * @brief Appends the src string to the dest string, overwriting the
  * terminating null byte at the end of dest, and then adds a
  * terminating null byte. The destination buffer must be large enough.
  * The strings may not overlap.
  * @param dest Pointer to the destination null-terminated string.
  * @param src Pointer to the source null-terminated string to append.
  * @return A pointer to the resulting string dest.
  */
 char *strcat(char *dest, const char *src);
 
 /**
  * @brief Appends at most n bytes from the src string to the dest string,
  * overwriting the terminating null byte at the end of dest, and then
  * adds a terminating null byte. The destination buffer must be large enough.
  * @param dest Pointer to the destination null-terminated string.
  * @param src Pointer to the source null-terminated string to append.
  * @param n The maximum number of bytes to append from src.
  * @return A pointer to the resulting string dest.
  */
 char *strncat(char *dest, const char *src, size_t n);
 
 /**
  * @brief Locates the first occurrence of c (converted to a char) in the
  * string pointed to by s. The terminating null character is considered
  * part of the string.
  * @param s Pointer to the null-terminated string to search.
  * @param c The character to search for.
  * @return A pointer to the first occurrence of the character c in the string s,
  * or NULL if the character is not found.
  */
 char *strchr(const char *s, int c);
 
 /**
  * @brief Locates the last occurrence of c (converted to a char) in the
  * string pointed to by s. The terminating null character is considered
  * part of the string.
  * @param s Pointer to the null-terminated string to search.
  * @param c The character to search for.
  * @return A pointer to the last occurrence of the character c in the string s,
  * or NULL if the character is not found.
  */
 char *strrchr(const char *s, int c);
 
 /**
  * @brief Calculates the length of the initial segment of s which consists
  * entirely of bytes in accept.
  * @param s Pointer to the string to be scanned.
  * @param accept Pointer to the string containing the characters to match.
  * @return The number of bytes in the initial segment of s which consist
  * only of bytes from accept.
  */
 size_t strspn(const char *s, const char *accept);
 
 /**
  * @brief Locates the first occurrence in the string s of any of the bytes
  * in the string accept.
  * @param s Pointer to the string to be searched.
  * @param accept Pointer to the string containing the characters to search for.
  * @return A pointer to the byte in s that matches one of the bytes in accept,
  * or NULL if no such byte is found.
  */
 char *strpbrk(const char *s, const char *accept);
 
 /**
  * @brief Extracts tokens from strings. A sequence of calls to this function
  * breaks the string str into tokens, which are sequences of contiguous
  * characters separated by any of the characters that are part of delim.
  * @param str The string to be tokenized. On the first call, this should point
  * to the string. On subsequent calls, it should be NULL.
  * @param delim A string containing the delimiter characters.
  * @return A pointer to the next token, or NULL if there are no more tokens.
  * @warning This function modifies the input string str and is not thread-safe
  * due to its use of a static internal pointer.
  */
 char *strtok(char *str, const char *delim);
 
 
 #endif // STRING_H