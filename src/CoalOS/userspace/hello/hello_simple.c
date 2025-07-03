/**
 * @file hello_simple.c
 * @brief Simple hello world program using Coal OS libc
 */

#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/unistd.h>

int main(void) {
    printf("Hello, Coal OS!\n");
    printf("Process ID: %d\n", getpid());
    printf("Demonstrating libc functionality...\n");
    
    // Test basic math
    int a = 10, b = 20;
    printf("Math test: %d + %d = %d\n", a, b, a + b);
    
    // Test memory allocation
    void *ptr = malloc(100);
    if (ptr) {
        printf("Memory allocation test: malloc(100) = %p\n", ptr);
        free(ptr);
        printf("Memory freed successfully\n");
    }
    
    printf("Hello world test completed!\n");
    exit(0);
    return 0;
}