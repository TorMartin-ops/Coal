/**
 * @file test_libc.c
 * @brief Simple test program to verify libc functionality
 */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/unistd.h>

int main(void) {
    printf("=== Coal OS libc Test Program ===\n");
    
    // Test printf variants
    printf("Integer: %d\n", 42);
    printf("Unsigned: %u\n", 123U);
    printf("Hex: 0x%x\n", 0xDEAD);
    printf("String: %s\n", "Hello, Coal OS!");
    printf("Character: %c\n", 'A');
    
    // Test string functions
    char buffer[100];
    strcpy(buffer, "Test ");
    strcat(buffer, "string");
    printf("String test: %s (length: %u)\n", buffer, (unsigned int)strlen(buffer));
    
    // Test memory allocation
    void *ptr = malloc(64);
    if (ptr) {
        printf("malloc(64) = %p\n", ptr);
        memset(ptr, 0xAA, 64);
        printf("memset success\n");
        free(ptr);
        printf("free() success\n");
    } else {
        printf("malloc failed\n");
    }
    
    // Test process info
    printf("Process ID: %d\n", getpid());
    
    printf("=== All tests completed ===\n");
    
    exit(0);
    return 0;
}