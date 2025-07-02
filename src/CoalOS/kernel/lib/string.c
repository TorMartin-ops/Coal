#include <kernel/lib/string.h> // Include the header this file implements
#include <libc/stdint.h> // For uintptr_t, uint8_t etc. used in optimized memcpy
#include <libc/stddef.h> // For size_t, NULL

// Helper definitions for optimized memcpy
typedef uintptr_t word_t; // Use native word size for potentially faster copies
#define WORD_SIZE sizeof(word_t)
#define WORD_MASK (WORD_SIZE - 1)

/* --- Memory Manipulation Functions --- */

void *memset(void *dest, int c, size_t n) {
    unsigned char *ptr = (unsigned char *)dest;
    unsigned char value = (unsigned char)c;

    for (size_t i = 0; i < n; i++) {
        ptr[i] = value;
    }

    return dest;
}

/**
 * @brief Copies n bytes from memory area src to memory area dest.
 * Uses word-sized copies for aligned sections for better performance.
 * @warning The memory areas must not overlap. Use memmove if overlap is possible.
 */
 void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}


void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    // If dest is before src or they don't overlap significantly,
    // a forward copy is safe.
    if (d <= s || d >= s + n) {
        // Can use memcpy (or the optimized forward copy logic within it)
        return memcpy(dest, src, n); // Assuming memcpy is defined (either builtin or custom)
    }

    // Otherwise, regions overlap and dest is *after* src (d > s).
    // Copy backward to avoid overwriting source data before it's read.
    // Start from the end of the buffers.
    d += n;
    s += n;
    while (n--) {
        *(--d) = *(--s);
    }

    return dest; // Per standard, return the original destination pointer
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *ptr = (const unsigned char *)s;
    unsigned char value = (unsigned char)c;

    for (size_t i = 0; i < n; i++) {
        if (ptr[i] == value) {
            return (void *)(ptr + i); // Cast okay: returning pointer within original buffer
        }
    }

    return NULL;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            // Return difference of the first non-matching bytes
            return p1[i] - p2[i];
        }
    }

    // All n bytes matched
    return 0;
}

/* --- String Manipulation Functions --- */

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    // Iterate while both strings have characters and they are equal
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    // Return the difference of the first non-matching characters
    // or the difference when one string ends (e.g., 'A' vs 'A\0' -> 0 - 'A')
    // Cast to unsigned char before subtraction as per C standard
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) {
        return 0;
    }

    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        // If we decrement n to 0 here, it means we compared n chars and they matched
        if (n == 0) break;
        s1++;
        s2++;
    }

    // If n is 0 here, it means the loop terminated because n characters were matched
    // Otherwise, the loop terminated because *s1 or *s2 was null, or *s1 != *s2
    // Return the difference of the characters at the point of termination
    // Cast to unsigned char before subtraction as per C standard
     return (n == (size_t)-1) ? 0 : *(const unsigned char*)s1 - *(const unsigned char*)s2;

     // Note: The n == (size_t)-1 check is a bit obscure way to check if n became 0
     // *after* the decrement in the loop condition when the characters matched.
     // A potentially clearer way:
     /*
     size_t i = 0;
     while (i < n && s1[i] && s2[i] && s1[i] == s2[i]) {
         i++;
     }
     if (i == n) {
         return 0; // Compared n characters and all matched
     } else {
         // Return difference at the point of mismatch or end-of-string
         return *(const unsigned char*)(s1 + i) - *(const unsigned char*)(s2 + i);
     }
     */
}


char *strcpy(char *dest, const char *src) {
    char *original_dest = dest;
    // Copy characters including the null terminator
    while ((*dest++ = *src++) != '\0');
    return original_dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *original_dest = dest;
    size_t i;

    // Copy at most n characters from src
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    // If src was shorter than n, pad the rest of dest with null bytes
    for (; i < n; i++) {
        dest[i] = '\0';
    }

    // Note: If src is n bytes or longer, dest might not be null-terminated!
    return original_dest;
}

char *strcat(char *dest, const char *src) {
    char *original_dest = dest;

    // Find the end of the destination string
    while (*dest != '\0') {
        dest++;
    }
    // Append the source string, including its null terminator
    while ((*dest++ = *src++) != '\0');

    return original_dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *original_dest = dest;

    // Find the end of the destination string
    while (*dest != '\0') {
        dest++;
    }

    // Append at most n characters from src
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    // Add the terminating null byte
    dest[i] = '\0';

    return original_dest;
}

char *strchr(const char *s, int c) {
    char char_to_find = (char)c;
    while (*s != '\0') {
        if (*s == char_to_find) {
            return (char *)s; // Cast away const-ness as per standard C library behavior
        }
        s++;
    }
    // If c was '\0', return pointer to the string's null terminator
    if (char_to_find == '\0') {
        return (char *)s;
    }
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last_match = NULL;
    char char_to_find = (char)c;

    // Iterate through the string, keeping track of the last match
    while (*s != '\0') {
        if (*s == char_to_find) {
            last_match = s;
        }
        s++;
    }
    // If c was '\0', return pointer to the string's null terminator
    if (char_to_find == '\0') {
        return (char *)s;
    }
    // Return the last match found, or NULL if none was found
    return (char *)last_match; // Cast away const-ness as per standard C library behavior
}


size_t strspn(const char *str, const char *accept) {
    size_t count = 0;
    const char *p;
    int found;

    while (str[count] != '\0') {
        found = 0;
        for (p = accept; *p != '\0'; p++) {
            if (str[count] == *p) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return count; // Character not in accept set
        }
        count++;
    }
    return count; // Reached end of str, all characters were in accept
}

char *strpbrk(const char *str, const char *accept) {
    const char *a;
    while (*str != '\0') {
        for (a = accept; *a != '\0'; a++) {
            if (*str == *a) {
                return (char *)str; // Found a matching character
            }
        }
        str++;
    }
    return NULL; // No character from accept found in str
}


// Static variable to hold the state for strtok
// WARNING: This makes strtok non-reentrant and not thread-safe!
static char *strtok_next = NULL;

char *strtok(char *str, const char *delim) {
    char *token_start;
    char *current_pos;

    // Use saved position if str is NULL
    current_pos = (str == NULL) ? strtok_next : str;

    // If current position is NULL, no more tokens
    if (current_pos == NULL) {
        return NULL;
    }

    // Skip leading delimiters
    current_pos += strspn(current_pos, delim);
    if (*current_pos == '\0') {
        strtok_next = NULL; // Reached end of string
        return NULL;
    }

    // Mark the beginning of the token
    token_start = current_pos;

    // Find the end of the token (first delimiter)
    current_pos = strpbrk(token_start, delim);

    if (current_pos == NULL) {
        // This token extends to the end of the string
        // Save NULL so the next call returns NULL
        strtok_next = NULL;
    } else {
        // Terminate the current token
        *current_pos = '\0';
        // Save the position after the delimiter for the next call
        strtok_next = current_pos + 1;
    }

    return token_start;
}