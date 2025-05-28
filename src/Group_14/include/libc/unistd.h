#ifndef _LIBC_UNISTD_H
#define _LIBC_UNISTD_H

#include <libc/stddef.h> // For size_t
#include <kernel/core/types.h>       // For ssize_t, off_t definition if not in stddef

#ifdef __cplusplus
extern "C" {
#endif

// === Corrected Function Prototypes ===
// Use int for file descriptor, ssize_t for read/write return count
// (ssize_t should be defined as int32_t in your types.h or stdint.h)
// Mode is typically int or mode_t (which is often unsigned int)
extern int   open  (const char *path, int flags, ... /* mode_t mode */); // Mode is optional, often int
extern ssize_t read  (int fd, void *buf, size_t count);
extern ssize_t write (int fd, const void *buf, size_t count);
extern int   close (int fd);
extern off_t lseek (int fd, off_t offset, int whence);
extern pid_t getpid(void); // Assuming pid_t is defined
extern void  exit  (int status); // Typically void return

// Add other standard POSIX functions as needed (fork, exec, etc.)

#ifdef __cplusplus
}
#endif

#endif // _LIBC_UNISTD_H