/**
 * @file syscall_security.h
 * @brief Security and Buffer Overflow Protection for System Calls
 * @author Coal OS Kernel Team
 * @version 1.0
 * 
 * @details Provides comprehensive security checks and buffer overflow protection
 * mechanisms for all system call implementations.
 */

#ifndef SYSCALL_SECURITY_H
#define SYSCALL_SECURITY_H

//============================================================================
// Includes
//============================================================================
#include <kernel/memory/uaccess.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <libc/stdbool.h>

// Forward declaration from syscall_utils.h
int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen);

//============================================================================
// Security Configuration
//============================================================================
#define SYSCALL_MAX_PATH_LEN        4096    // Maximum path length
#define SYSCALL_MAX_FILENAME_LEN    255     // Maximum filename length
#define SYSCALL_MAX_ARG_LEN         131072  // Maximum argument length (128KB)
#define SYSCALL_MAX_ENV_LEN         131072  // Maximum environment length (128KB)
#define SYSCALL_MAX_BUFFER_LEN      1048576 // Maximum general buffer (1MB)
#define SYSCALL_MAX_ARGV_COUNT      1024    // Maximum number of argv entries
#define SYSCALL_MAX_ENVP_COUNT      1024    // Maximum number of envp entries

//============================================================================
// Security Check Macros
//============================================================================

/**
 * @brief Validate string length from user space
 * @param ptr User space pointer
 * @param max_len Maximum allowed length
 * @return true if valid, false otherwise
 */
static inline bool syscall_validate_string_len(const_userptr_t ptr, size_t max_len)
{
    if (!ptr || !access_ok(VERIFY_READ, ptr, 1)) {
        return false;
    }
    
    // Check string length without copying entire string
    size_t len = 0;
    while (len < max_len) {
        char c;
        if (copy_from_user(&c, (const_userptr_t)((const char*)ptr + len), 1) != 0) {
            return false; // Page fault or invalid memory
        }
        if (c == '\0') {
            return true; // Found null terminator within limit
        }
        len++;
    }
    
    return false; // String too long
}

/**
 * @brief Validate buffer boundaries
 * @param ptr Buffer pointer
 * @param size Buffer size
 * @param write true if buffer will be written to
 * @return true if valid, false otherwise
 */
static inline bool syscall_validate_buffer(userptr_t ptr, size_t size, bool write)
{
    // Check for integer overflow
    if (size > SYSCALL_MAX_BUFFER_LEN) {
        serial_printf("[syscall_security] Buffer size too large: %zu\n", size);
        return false;
    }
    
    // Check for wrap-around
    if ((uintptr_t)ptr + size < (uintptr_t)ptr) {
        serial_printf("[syscall_security] Buffer address wrap-around detected\n");
        return false;
    }
    
    // Validate access permissions
    int access_type = write ? VERIFY_WRITE : VERIFY_READ;
    if (!access_ok(access_type, ptr, size)) {
        serial_printf("[syscall_security] Invalid buffer access: ptr=0x%x, size=%zu\n", 
                     (uintptr_t)ptr, size);
        return false;
    }
    
    return true;
}

/**
 * @brief Safely copy path from user space with validation
 * @param user_path User space path pointer
 * @param kernel_buf Kernel buffer to copy into
 * @param buf_size Size of kernel buffer
 * @return 0 on success, negative error code on failure
 */
static inline int syscall_copy_path_from_user(const_userptr_t user_path, 
                                              char *kernel_buf, 
                                              size_t buf_size)
{
    if (!kernel_buf || buf_size == 0) {
        return -EINVAL;
    }
    
    // Ensure buffer is large enough for paths
    if (buf_size > SYSCALL_MAX_PATH_LEN) {
        buf_size = SYSCALL_MAX_PATH_LEN;
    }
    
    // Validate string length first
    if (!syscall_validate_string_len(user_path, buf_size)) {
        serial_printf("[syscall_security] Path validation failed\n");
        return -ENAMETOOLONG;
    }
    
    // Copy the path
    int result = strncpy_from_user_safe(user_path, kernel_buf, buf_size);
    if (result < 0) {
        return result;
    }
    
    // Additional validation: check for directory traversal attempts
    // TODO: Implement proper path traversal detection without strstr
    // For now, just do a simple check for ".."
    for (size_t i = 0; kernel_buf[i] != '\0' && i < buf_size - 2; i++) {
        if (kernel_buf[i] == '.' && kernel_buf[i+1] == '.') {
            serial_printf("[syscall_security] Potential directory traversal detected: %s\n", 
                         kernel_buf);
            // Note: This is just a warning for now, as .. is legitimate
            break;
        }
    }
    
    return 0;
}

/**
 * @brief Validate array of string pointers (for argv/envp)
 * @param user_array User space array of string pointers
 * @param max_count Maximum number of entries allowed
 * @param max_total_size Maximum total size of all strings
 * @return Number of entries on success, negative error code on failure
 */
static inline int syscall_validate_string_array(const_userptr_t user_array,
                                               size_t max_count,
                                               size_t max_total_size)
{
    if (!user_array) {
        return 0; // NULL array is valid (0 entries)
    }
    
    size_t count = 0;
    size_t total_size = 0;
    
    while (count < max_count) {
        // Read pointer from array
        char *str_ptr;
        if (copy_from_user(&str_ptr, 
                          (const_userptr_t)(((char **)user_array) + count), 
                          sizeof(char *)) != 0) {
            return -EFAULT;
        }
        
        // NULL pointer marks end of array
        if (!str_ptr) {
            break;
        }
        
        // Validate string
        if (!syscall_validate_string_len((const_userptr_t)str_ptr, 
                                        max_total_size - total_size)) {
            return -E2BIG; // Argument list too long
        }
        
        // Count approximate size (we'll get exact size during actual copy)
        count++;
    }
    
    if (count >= max_count) {
        return -E2BIG; // Too many arguments
    }
    
    return count;
}

//============================================================================
// Buffer Overflow Protection Helpers
//============================================================================

/**
 * @brief Safe integer addition with overflow check
 * @param a First operand
 * @param b Second operand
 * @param result Pointer to store result
 * @return true if no overflow, false if overflow would occur
 */
static inline bool safe_add(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b) {
        return false; // Would overflow
    }
    *result = a + b;
    return true;
}

/**
 * @brief Safe integer multiplication with overflow check
 * @param a First operand
 * @param b Second operand
 * @param result Pointer to store result
 * @return true if no overflow, false if overflow would occur
 */
static inline bool safe_mul(size_t a, size_t b, size_t *result)
{
    if (a == 0 || b == 0) {
        *result = 0;
        return true;
    }
    if (a > SIZE_MAX / b) {
        return false; // Would overflow
    }
    *result = a * b;
    return true;
}

#endif // SYSCALL_SECURITY_H