/**
 * @file constants.h
 * @brief System-wide constants and magic numbers definition
 * 
 * This file centralizes all magic numbers and constants used throughout
 * the Coal OS kernel to improve maintainability and reduce errors.
 * 
 * Recently added:
 * - Hardware timeout values (keyboard controller, delays)
 * - Common bit manipulation masks
 * - HAL (Hardware Abstraction Layer) constants
 * - FAT32 filesystem masks
 */

#ifndef KERNEL_CORE_CONSTANTS_H
#define KERNEL_CORE_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Memory Layout Constants
// =============================================================================

// Physical memory layout
#define KERNEL_PHYS_BASE            0x100000U      // 1MB - kernel physical start
#define KERNEL_VIRT_BASE            0xC0000000U    // 3GB - kernel virtual start
#define KERNEL_SPACE_VIRT_START     KERNEL_VIRT_BASE

// Virtual memory regions
#define USER_SPACE_START_VIRT       0x00001000U    // 4KB - user space start
#define USER_SPACE_END_VIRT         0xBFFFFFFFU    // Just below kernel space
#define KERNEL_STACK_VIRT_START     0xE0000000U    // Kernel stack region start
#define KERNEL_STACK_VIRT_END       0xF0000000U    // Kernel stack region end

// User stack layout (within user space)
#define USER_STACK_TOP_VIRT_ADDR    0xBFFF0000U    // Top of user stack (grows down)
#define USER_STACK_BOTTOM_VIRT      0xBF000000U    // Bottom of user stack region
#define USER_STACK_SIZE             0x00FF0000U    // ~16MB stack space

// Page sizes and alignment
#define PAGE_SIZE                   4096U          // 4KB pages
#define PAGE_SHIFT                  12U            // log2(PAGE_SIZE)
#define PAGE_MASK                   (PAGE_SIZE - 1)

// =============================================================================
// Hardware Constants
// =============================================================================

// VGA text mode
#define VGA_TEXT_BUFFER_ADDR        0xB8000U       // VGA text buffer address
#define VGA_TEXT_COLS               80U            // VGA text columns
#define VGA_TEXT_ROWS               25U            // VGA text rows
#define VGA_CMD_PORT                0x3D4U         // VGA command port
#define VGA_DATA_PORT               0x3D5U         // VGA data port

// Serial ports
#define SERIAL_COM1_BASE            0x3F8U         // COM1 base port
#define SERIAL_COM2_BASE            0x2F8U         // COM2 base port
#define SERIAL_COM3_BASE            0x3E8U         // COM3 base port
#define SERIAL_COM4_BASE            0x2E8U         // COM4 base port

// Keyboard controller
#define KEYBOARD_DATA_PORT          0x60U          // Keyboard data port
#define KEYBOARD_STATUS_PORT        0x64U          // Keyboard status/command port

// PIC (Programmable Interrupt Controller)
#define PIC1_COMMAND_PORT           0x20U          // Master PIC command
#define PIC1_DATA_PORT              0x21U          // Master PIC data
#define PIC2_COMMAND_PORT           0xA0U          // Slave PIC command
#define PIC2_DATA_PORT              0xA1U          // Slave PIC data

// PIT (Programmable Interval Timer)
#define PIT_CHANNEL0_DATA           0x40U          // PIT channel 0 data
#define PIT_CHANNEL1_DATA           0x41U          // PIT channel 1 data
#define PIT_CHANNEL2_DATA           0x42U          // PIT channel 2 data
#define PIT_COMMAND_PORT            0x43U          // PIT command port

// Standard I/O delay port
#define IO_DELAY_PORT               0x80U          // Port for I/O delays

// =============================================================================
// File System Constants
// =============================================================================

// Generic filesystem
#define FS_MAX_PATH_LENGTH          4096U          // Maximum path length
#define FS_MAX_FILENAME_LENGTH      255U           // Maximum filename length
#define MAX_FD                      16U            // Max file descriptors per process

// FAT filesystem
#define FAT_SECTOR_SIZE             512U           // FAT sector size
#define FAT_MAX_LFN_CHARS           255U           // Max long filename chars
#define FAT_ATTR_READ_ONLY          0x01U          // Read-only attribute
#define FAT_ATTR_HIDDEN             0x02U          // Hidden attribute
#define FAT_ATTR_SYSTEM             0x04U          // System attribute
#define FAT_ATTR_VOLUME_LABEL       0x08U          // Volume label attribute
#define FAT_ATTR_DIRECTORY          0x10U          // Directory attribute
#define FAT_ATTR_ARCHIVE            0x20U          // Archive attribute
#define FAT_ATTR_LFN                0x0FU          // Long filename attribute
#define FAT32_CLUSTER_MASK          0x0FFFFFFFU    // FAT32 cluster mask (28 bits)

// Boot sector
#define BOOT_SIGNATURE              0xAA55U        // Boot sector signature

// =============================================================================
// Process and Scheduling Constants
// =============================================================================

// Process limits
#define MAX_PROCESSES               256U           // Maximum number of processes
#define PROCESS_KSTACK_SIZE         (4 * PAGE_SIZE) // Kernel stack size per process
#define PROCESS_USTACK_SIZE         (4 * PAGE_SIZE) // User stack size per process
#define PROCESS_NAME_MAX_LENGTH     32U            // Maximum process name length

// Scheduling
#define SCHEDULER_PRIORITY_LEVELS   4U             // Number of priority levels
#define SCHEDULER_DEFAULT_PRIORITY  1U             // Default process priority
#define SCHEDULER_IDLE_PRIORITY     3U             // Idle task priority
#define SCHEDULER_KERNEL_PRIORITY   0U             // Kernel task priority

// Time slices (in milliseconds)
#define TIMESLICE_HIGH_PRIORITY     200U           // High priority time slice
#define TIMESLICE_NORMAL_PRIORITY   100U           // Normal priority time slice
#define TIMESLICE_LOW_PRIORITY      50U            // Low priority time slice
#define TIMESLICE_IDLE_PRIORITY     25U            // Idle priority time slice

// =============================================================================
// Memory Management Constants
// =============================================================================

// Buddy allocator
#define BUDDY_MIN_ORDER             12U            // Minimum order (4KB)
#define BUDDY_MAX_ORDER             20U            // Maximum order (1MB)
#define BUDDY_MIN_BLOCK_SIZE        (1U << BUDDY_MIN_ORDER)

// Kmalloc
#define KMALLOC_MIN_SIZE            16U            // Minimum allocation size
#define KMALLOC_MAX_SIZE            (1024U * 1024U) // Maximum allocation size (1MB)
#define KMALLOC_DEFAULT_ALIGNMENT   8U             // Default alignment

// Slab allocator
#define SLAB_MIN_OBJECT_SIZE        8U             // Minimum object size
#define SLAB_MAX_OBJECT_SIZE        4096U          // Maximum object size
#define SLAB_OBJECTS_PER_PAGE       (PAGE_SIZE / SLAB_MIN_OBJECT_SIZE)

// =============================================================================
// System Call Constants
// =============================================================================

#define SYSCALL_INTERRUPT_VECTOR   0x80U          // System call interrupt vector
#define MAX_SYSCALL_ARGS           6U             // Maximum syscall arguments

// System call numbers
#define SYS_EXIT                    1U
#define SYS_READ                    3U
#define SYS_WRITE                   4U
#define SYS_OPEN                    5U
#define SYS_CLOSE                   6U
#define SYS_LSEEK                   8U
#define SYS_GETPID                  20U

// =============================================================================
// Error Codes and Return Values
// =============================================================================

#define SUCCESS                     0
#define NULL_POINTER                ((void*)0)

// Standard POSIX-like error codes
#define EPERM                       1              // Operation not permitted
#define ENOENT                      2              // No such file or directory
#define EINTR                       4              // Interrupted system call
#define EIO                         5              // I/O error
#define ENXIO                       6              // No such device or address
#define ENOEXEC                     8              // Exec format error
#define EBADF                       9              // Bad file number
#define ENOMEM                      12             // Out of memory
#define EACCES                      13             // Permission denied
#define EFAULT                      14             // Bad address
#define EBUSY                       16             // Device or resource busy
#define EEXIST                      17             // File exists
#define ENOTDIR                     20             // Not a directory
#define EISDIR                      21             // Is a directory
#define EINVAL                      22             // Invalid argument
#define ENFILE                      23             // File table overflow
#define EMFILE                      24             // Too many open files
#define ENOSPC                      28             // No space left on device
#define EROFS                       30             // Read-only file system

// =============================================================================
// Buffer and Cache Constants
// =============================================================================

#define BUFFER_CACHE_SIZE           (1024U * 1024U) // 1MB buffer cache
#define BUFFER_CACHE_BLOCK_SIZE     512U           // Block size for cache
#define MAX_CACHED_BLOCKS           (BUFFER_CACHE_SIZE / BUFFER_CACHE_BLOCK_SIZE)

// Terminal and I/O
#define TERMINAL_BUFFER_SIZE        256U           // Terminal output buffer size
#define INPUT_BUFFER_SIZE           1024U          // Input buffer size
#define TAB_WIDTH                   4U             // Tab width in spaces

// =============================================================================
// CPU and Architecture Constants
// =============================================================================

// x86 specific
#define X86_EFLAGS_IF               (1U << 9)      // Interrupt enable flag
#define X86_EFLAGS_RESERVED         (1U << 1)      // Reserved flag (always 1)
#define USER_EFLAGS_DEFAULT         (X86_EFLAGS_IF | X86_EFLAGS_RESERVED)

// Common bit masks
#define BYTE_MASK                   0xFFU          // Full byte mask
#define NIBBLE_HIGH_MASK            0xF0U          // High nibble mask
#define NIBBLE_LOW_MASK             0x0FU          // Low nibble mask
#define WORD_MASK                   0xFFFFU        // Full word mask
#define DWORD_MASK                  0xFFFFFFFFU    // Full double word mask

// GDT selectors
#define GDT_KERNEL_CODE_SELECTOR    0x08U          // Kernel code segment
#define GDT_KERNEL_DATA_SELECTOR    0x10U          // Kernel data segment
#define GDT_USER_CODE_SELECTOR      0x18U          // User code segment
#define GDT_USER_DATA_SELECTOR      0x20U          // User data segment
#define GDT_TSS_SELECTOR            0x28U          // TSS selector

// IDT
#define IDT_ENTRIES                 256U           // Number of IDT entries

// =============================================================================
// Timing and Delays
// =============================================================================

#define TIMER_FREQUENCY_HZ          1000U          // Timer frequency (1kHz)
#define MS_PER_SECOND               1000U          // Milliseconds per second
#define US_PER_SECOND               1000000U       // Microseconds per second
#define NS_PER_SECOND               1000000000U    // Nanoseconds per second

// Hardware timeout values
#define KBC_WAIT_TIMEOUT_CYCLES     300000U        // Keyboard controller timeout
#define KBC_FLUSH_MAX_ATTEMPTS      100U           // Max keyboard flush attempts
#define SHORT_DELAY_CYCLES          15000U         // Short hardware delay cycles

// =============================================================================
// String and Text Constants
// =============================================================================

#define MAX_STRING_LENGTH           4096U          // Maximum string length
#define PRINTF_BUFFER_SIZE          256U           // Printf buffer size

// =============================================================================
// Hardware Abstraction Layer Constants
// =============================================================================

// HAL layer configuration
#define HAL_MAX_TIMERS              8U             // Maximum HAL timers
#define HAL_MAX_IRQS                256U           // Maximum IRQ numbers
#define HAL_DEFAULT_TIMER_FREQ      1000U          // Default timer frequency

// HAL timer types
#define HAL_TIMER_SYSTEM            0U             // System timer ID
#define HAL_TIMER_ONE_SHOT          1U             // One-shot timer type
#define HAL_TIMER_PERIODIC          2U             // Periodic timer type

// =============================================================================
// Validation and Limits
// =============================================================================

// Pointer validation
#define IS_KERNEL_ADDRESS(addr)     ((uintptr_t)(addr) >= KERNEL_VIRT_BASE)
#define IS_USER_ADDRESS(addr)       ((uintptr_t)(addr) < KERNEL_VIRT_BASE && \
                                     (uintptr_t)(addr) >= USER_SPACE_START_VIRT)

// Alignment macros
#define IS_ALIGNED(addr, align)     (((uintptr_t)(addr) & ((align) - 1)) == 0)
#define ALIGN_UP(addr, align)       (((uintptr_t)(addr) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(addr, align)     ((uintptr_t)(addr) & ~((align) - 1))

// Page alignment
#define PAGE_ALIGN_UP(addr)         ALIGN_UP(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr)       ALIGN_DOWN(addr, PAGE_SIZE)
#define IS_PAGE_ALIGNED(addr)       IS_ALIGNED(addr, PAGE_SIZE)

#ifdef __cplusplus
}
#endif

#endif // KERNEL_CORE_CONSTANTS_H