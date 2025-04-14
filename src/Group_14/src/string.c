#include "string.h"

void *memset(void *dest, int c, size_t n) {
    unsigned char *ptr = (unsigned char *)dest;
    unsigned char value = (unsigned char)c;
    
    for (size_t i = 0; i < n; i++) {
        ptr[i] = value;
    }
    
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    // If dest is before src or after src+n, we can use a forward copy
    if (d <= s || d >= s + n) {
        return memcpy(dest, src, n);
    }
    
    // Otherwise, we need to copy backward to avoid overwriting source data
    for (size_t i = n; i > 0; i--) {
        d[i-1] = s[i-1];
    }
    
    return dest;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *ptr = (const unsigned char *)s;
    unsigned char value = (unsigned char)c;
    
    for (size_t i = 0; i < n; i++) {
        if (ptr[i] == value) {
            return (void *)(ptr + i);
        }
    }
    
    return NULL;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    
    while (s[len] != '\0') {
        len++;
    }
    
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) {
        return 0; // Comparing zero characters always results in equality
    }

    // Iterate while characters match, n > 0, and neither string ends
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    // If n reached 0 or we hit the end of s1 simultaneously with s2 ending or matching, they are equal up to n
    // Otherwise, return the difference of the first non-matching characters (or terminating null).
    // The subtraction handles the case where one string ends before n characters are compared.
    // Note: We cast to unsigned char before subtraction, as per the C standard, to handle potential negative char values correctly.
    if (n == (size_t)-1) { // Check if n wrapped around due to n-- on the last iteration when they matched
        return 0; // Matched exactly n characters
    } else {
        return *(const unsigned char *)s1 - *(const unsigned char *)s2;
    }
}

char *strcpy(char *dest, const char *src) {
    char *original_dest = dest;
    
    while ((*dest++ = *src++) != '\0');
    
    return original_dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    
    // Copy from src to dest, up to n characters or until src is exhausted
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    
    // Pad remaining characters in dest with nulls
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *original_dest = dest;
    
    // Find the end of dest
    while (*dest) {
        dest++;
    }
    
    // Copy src to the end of dest
    while ((*dest++ = *src++) != '\0');
    
    return original_dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *original_dest = dest;
    size_t dest_len;
    
    // Find the end of dest
    while (*dest) {
        dest++;
    }
    dest_len = dest - original_dest;
    
    // Append at most n characters from src
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    
    // Add terminating null byte
    dest[i] = '\0';
    
    return original_dest;
}

char *strchr(const char *s, int c) {
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    
    // Also check for terminating null if c is '\0'
    if ((char)c == '\0') {
        return (char *)s;
    }
    
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    
    // Search for the last occurrence
    while (*s != '\0') {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    
    // Check terminating null if c is '\0'
    if ((char)c == '\0') {
        return (char *)s;
    }
    
    return (char *)last;
}

size_t strspn(const char *str, const char *accept) {
    size_t count = 0;
    
    while (str[count] != '\0') {
        // Check if current character is in accept
        size_t i = 0;
        while (accept[i] != '\0') {
            if (str[count] == accept[i]) {
                break;
            }
            i++;
        }
        
        // If we reached the end of accept, character is not accepted
        if (accept[i] == '\0') {
            return count;
        }
        
        count++;
    }
    
    return count;
}

char *strpbrk(const char *str, const char *accept) {
    while (*str != '\0') {
        // Check if current character is in accept
        const char *a = accept;
        while (*a != '\0') {
            if (*str == *a) {
                return (char *)str;
            }
            a++;
        }
        str++;
    }
    
    return NULL;
}

// Static variable for strtok's state
static char *strtok_next = NULL;

char *strtok(char *str, const char *delim) {
    char *token_start;
    
    // If str is NULL, use the saved position
    if (str == NULL) {
        str = strtok_next;
    }
    
    // If starting position is NULL, we're done
    if (str == NULL) {
        return NULL;
    }
    
    // Skip leading delimiters
    str += strspn(str, delim);
    if (*str == '\0') {
        strtok_next = NULL;
        return NULL;
    }
    
    // Find the end of the token
    token_start = str;
    str = strpbrk(token_start, delim);
    if (str == NULL) {
        // This is the last token
        strtok_next = NULL;
    } else {
        // Terminate the token and set the next position
        *str = '\0';
        strtok_next = str + 1;
    }
    
    return token_start;
}