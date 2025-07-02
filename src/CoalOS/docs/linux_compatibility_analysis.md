# Linux System Call Compatibility Analysis for CoalOS

## Current CoalOS System Call Implementation

### Implemented System Calls (29 total)

| Syscall | Number | Linux Equivalent | Status |
|---------|--------|------------------|---------|
| exit | 1 | exit (1) | ✓ Compatible |
| fork | 2 | fork (2) | ✓ Compatible |
| read | 3 | read (3) | ✓ Compatible |
| write | 4 | write (4) | ✓ Compatible |
| open | 5 | open (5) | ✓ Compatible |
| close | 6 | close (6) | ✓ Compatible |
| puts | 7 | - | ✗ Non-standard |
| execve | 11 | execve (11) | ✓ Compatible |
| chdir | 12 | chdir (12) | ✓ Compatible |
| waitpid | 17 | waitpid (7) | ✗ Wrong number |
| lseek | 19 | lseek (19) | ✓ Compatible |
| getpid | 20 | getpid (20) | ✓ Compatible |
| read_terminal_line | 21 | - | ✗ Non-standard |
| dup2 | 33 | dup2 (63) | ✗ Wrong number |
| kill | 37 | kill (62) | ✗ Wrong number |
| pipe | 42 | pipe (42) | ✓ Compatible |
| signal | 48 | signal (48) | ✓ Compatible |
| setpgid | 57 | setpgid (57) | ✓ Compatible |
| getppid | 64 | getppid (64) | ✓ Compatible |
| getpgrp | 65 | getpgrp (65) | ✓ Compatible |
| setsid | 66 | setsid (66) | ✓ Compatible |
| readdir | 89 | getdents (141) | ✗ Wrong interface |
| getpgid | 132 | getpgid (132) | ✓ Compatible |
| getsid | 147 | getsid (147) | ✓ Compatible |
| getcwd | 183 | getcwd (183) | ✓ Compatible |
| stat | 4 | stat (106) | ✗ Wrong number |
| tcsetpgrp | 410 | - | ✗ Non-standard number |
| tcgetpgrp | 411 | - | ✗ Non-standard number |

## Critical Missing Linux System Calls

### Essential for Basic POSIX Compliance

1. **Memory Management**
   - mmap (90) - Memory mapping
   - munmap (91) - Unmap memory
   - mprotect (125) - Set memory protection
   - brk (45) - Change data segment size

2. **File System Operations**
   - mkdir (39) - Create directory
   - rmdir (40) - Remove directory
   - unlink (10) - Delete file
   - rename (38) - Rename file
   - chmod (15) - Change file permissions
   - chown (16) - Change file ownership
   - fstat (108) - Get file status by fd
   - access (33) - Check file permissions
   - ioctl (54) - Device control

3. **Process Management**
   - vfork (190) - Create child process
   - wait4 (114) - Wait for process
   - clone (120) - Create new process/thread
   - ptrace (26) - Process trace

4. **Time Management**
   - time (13) - Get time
   - gettimeofday (78) - Get time of day
   - nanosleep (162) - High resolution sleep
   - clock_gettime (265) - Get clock time

5. **Signal Management**
   - sigaction (67) - Examine/change signal action
   - sigprocmask (126) - Examine/change blocked signals
   - sigpending (73) - Examine pending signals
   - sigsuspend (72) - Wait for signal

6. **Inter-Process Communication**
   - socketpair (8) - Create pair of connected sockets
   - shmget (29) - Get shared memory segment
   - shmat (30) - Attach shared memory
   - shmdt (67) - Detach shared memory
   - msgget (68) - Get message queue
   - msgsnd (69) - Send message
   - msgrcv (70) - Receive message

## System Call Number Corrections Needed

To achieve Linux compatibility, the following syscall numbers need adjustment:

```c
// Current -> Linux Standard
#define SYS_WAITPID   17  ->  7   // waitpid
#define SYS_DUP2      33  ->  63  // dup2
#define SYS_KILL      37  ->  62  // kill
#define SYS_STAT      4   ->  106 // stat
#define SYS_READDIR   89  ->  141 // getdents
```

## Proposed Implementation Priority

### Phase 1: Fix System Call Numbers
Adjust existing system calls to use Linux-standard numbers for binary compatibility.

### Phase 2: Implement Essential Missing Calls
1. Memory management (mmap, munmap, brk)
2. Basic file operations (mkdir, rmdir, unlink)
3. Process synchronization (wait4)
4. Time management (gettimeofday, nanosleep)

### Phase 3: Advanced Features
1. Signal handling (sigaction, sigprocmask)
2. IPC mechanisms (pipes already done, add shared memory)
3. Advanced process control (clone, ptrace)

## Architecture Considerations

1. **System Call Convention**: Linux x86 uses interrupt 0x80 with:
   - EAX: System call number
   - EBX, ECX, EDX, ESI, EDI, EBP: Arguments
   - Return value in EAX

2. **Error Handling**: Linux returns negative error codes (-ERRNO) in EAX

3. **User Space Validation**: All pointers from user space must be validated

4. **Compatibility Layer**: Consider implementing personality(2) for future compatibility modes