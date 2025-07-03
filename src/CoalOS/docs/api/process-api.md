# Process Management API

This document describes the process and thread management APIs in Coal OS.

## Overview

Coal OS implements a Unix-like process model with:
- **Processes**: Independent execution contexts with separate address spaces
- **Threads**: Lightweight execution units within a process
- **Scheduler**: O(1) priority-based scheduler with 4 priority levels
- **Signals**: Unix-style signal delivery (future)

## Process Management

### Process Creation

```c
/**
 * @brief Create a new process
 * @param path Path to executable
 * @param argv Argument vector
 * @param envp Environment vector
 * @return Process ID or negative error code
 */
pid_t process_create(const char *path, char *const argv[], char *const envp[]);

/**
 * @brief Fork current process
 * @return PID to parent, 0 to child, negative on error
 */
pid_t process_fork(void);

/**
 * @brief Replace current process image
 * @param path Path to executable
 * @param argv Argument vector
 * @param envp Environment vector
 * @return Does not return on success, negative on error
 */
int process_exec(const char *path, char *const argv[], char *const envp[]);
```

### Process Control

```c
/**
 * @brief Get current process
 * @return Pointer to current process structure
 */
process_t* current_process(void);

/**
 * @brief Get process by PID
 * @param pid Process ID
 * @return Process pointer or NULL if not found
 */
process_t* process_get_by_pid(pid_t pid);

/**
 * @brief Terminate current process
 * @param status Exit status
 */
void __attribute__((noreturn)) process_exit(int status);

/**
 * @brief Terminate process by PID
 * @param pid Process ID
 * @param status Exit status
 * @return 0 on success, negative error code on failure
 */
int process_kill(pid_t pid, int status);

/**
 * @brief Wait for child process
 * @param pid PID to wait for (-1 for any child)
 * @param status Pointer to store exit status
 * @param options Wait options
 * @return PID of terminated child or negative error
 */
pid_t process_wait(pid_t pid, int *status, int options);
```

### Process Information

```c
/**
 * @brief Get current process ID
 * @return Current process ID
 */
pid_t process_getpid(void);

/**
 * @brief Get parent process ID
 * @return Parent process ID
 */
pid_t process_getppid(void);

/**
 * @brief Get process information
 * @param pid Process ID
 * @param info Structure to fill
 * @return 0 on success, negative error code on failure
 */
int process_get_info(pid_t pid, process_info_t *info);

typedef struct {
    pid_t pid;                // Process ID
    pid_t ppid;               // Parent PID
    uid_t uid;                // User ID
    gid_t gid;                // Group ID
    process_state_t state;    // Current state
    uint32_t priority;        // Priority level
    size_t memory_usage;      // Memory in bytes
    uint64_t cpu_time;        // CPU time in ticks
    char name[TASK_NAME_MAX]; // Process name
} process_info_t;
```

### Process States

```c
typedef enum {
    PROCESS_READY,      // Ready to run
    PROCESS_RUNNING,    // Currently running
    PROCESS_BLOCKED,    // Blocked on I/O or event
    PROCESS_SLEEPING,   // Sleeping (timed wait)
    PROCESS_ZOMBIE,     // Terminated, waiting for parent
    PROCESS_DEAD        // Can be cleaned up
} process_state_t;
```

## Thread Management

### Thread Creation

```c
/**
 * @brief Create a new thread
 * @param entry Thread entry point
 * @param arg Argument to pass to thread
 * @param flags Thread creation flags
 * @return Thread ID or negative error code
 */
tid_t thread_create(void (*entry)(void *), void *arg, uint32_t flags);

/**
 * @brief Get current thread
 * @return Pointer to current thread structure
 */
thread_t* current_thread(void);

/**
 * @brief Exit current thread
 * @param retval Return value
 */
void __attribute__((noreturn)) thread_exit(void *retval);

/**
 * @brief Wait for thread to terminate
 * @param tid Thread ID
 * @param retval Pointer to store return value
 * @return 0 on success, negative error code on failure
 */
int thread_join(tid_t tid, void **retval);
```

### Thread Flags

```c
#define THREAD_CREATE_DETACHED  0x01  // Thread cannot be joined
#define THREAD_CREATE_SUSPENDED 0x02  // Create in suspended state
#define THREAD_CREATE_KERNEL    0x04  // Kernel thread
```

## Scheduling

### Scheduler Control

```c
/**
 * @brief Yield CPU to another thread
 */
void scheduler_yield(void);

/**
 * @brief Sleep for specified milliseconds
 * @param ms Milliseconds to sleep
 */
void scheduler_sleep(uint32_t ms);

/**
 * @brief Set thread priority
 * @param tid Thread ID (0 for current)
 * @param priority New priority (0-3)
 * @return 0 on success, negative error code on failure
 */
int scheduler_set_priority(tid_t tid, uint32_t priority);

/**
 * @brief Get thread priority
 * @param tid Thread ID (0 for current)
 * @return Priority level or negative error
 */
int scheduler_get_priority(tid_t tid);
```

### Priority Levels

```c
#define PRIORITY_REALTIME  0   // Highest priority
#define PRIORITY_HIGH      1   // High priority
#define PRIORITY_NORMAL    2   // Default priority
#define PRIORITY_IDLE      3   // Lowest priority
```

### Scheduler Statistics

```c
typedef struct {
    uint64_t total_switches;   // Total context switches
    uint64_t voluntary_switches; // Voluntary yields
    uint64_t preemptions;      // Forced preemptions
    uint32_t ready_threads[4]; // Threads per priority
    uint32_t active_threads;   // Total active threads
    uint64_t idle_time;        // Time spent idle
} scheduler_stats_t;

/**
 * @brief Get scheduler statistics
 * @param stats Structure to fill
 */
void scheduler_get_stats(scheduler_stats_t *stats);
```

## Signal Handling (Future)

### Signal Operations

```c
/**
 * @brief Send signal to process
 * @param pid Process ID
 * @param sig Signal number
 * @return 0 on success, negative error code on failure
 */
int signal_send(pid_t pid, int sig);

/**
 * @brief Set signal handler
 * @param sig Signal number
 * @param handler Handler function
 * @return Previous handler
 */
sighandler_t signal_set_handler(int sig, sighandler_t handler);

/**
 * @brief Block/unblock signals
 * @param how SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK
 * @param set Signal set to modify
 * @param oldset Previous signal set (optional)
 * @return 0 on success, negative error code on failure
 */
int signal_procmask(int how, const sigset_t *set, sigset_t *oldset);
```

### Common Signals

```c
#define SIGKILL     9   // Kill (cannot be caught)
#define SIGTERM    15   // Terminate
#define SIGSTOP    19   // Stop process
#define SIGCONT    18   // Continue process
#define SIGCHLD    17   // Child terminated
```

## Synchronization

### Wait Queues

```c
/**
 * @brief Initialize wait queue
 * @param queue Wait queue to initialize
 */
void wait_queue_init(wait_queue_t *queue);

/**
 * @brief Wait on queue
 * @param queue Wait queue
 * @param condition Condition function
 * @param timeout_ms Timeout in milliseconds (0 = infinite)
 * @return 0 on success, -ETIMEDOUT on timeout
 */
int wait_queue_wait(wait_queue_t *queue, 
                   bool (*condition)(void *), 
                   uint32_t timeout_ms);

/**
 * @brief Wake one waiter
 * @param queue Wait queue
 */
void wait_queue_wake_one(wait_queue_t *queue);

/**
 * @brief Wake all waiters
 * @param queue Wait queue
 */
void wait_queue_wake_all(wait_queue_t *queue);
```

## Process Groups and Sessions

```c
/**
 * @brief Create new session
 * @return Session ID or negative error
 */
pid_t process_setsid(void);

/**
 * @brief Get session ID
 * @param pid Process ID (0 for current)
 * @return Session ID or negative error
 */
pid_t process_getsid(pid_t pid);

/**
 * @brief Set process group
 * @param pid Process ID (0 for current)
 * @param pgid Process group ID
 * @return 0 on success, negative error code on failure
 */
int process_setpgid(pid_t pid, pid_t pgid);

/**
 * @brief Get process group
 * @param pid Process ID (0 for current)
 * @return Process group ID or negative error
 */
pid_t process_getpgid(pid_t pid);
```

## Resource Limits

```c
/**
 * @brief Set resource limit
 * @param resource Resource type
 * @param rlim New limit
 * @return 0 on success, negative error code on failure
 */
int process_setrlimit(int resource, const struct rlimit *rlim);

/**
 * @brief Get resource limit
 * @param resource Resource type
 * @param rlim Structure to fill
 * @return 0 on success, negative error code on failure
 */
int process_getrlimit(int resource, struct rlimit *rlim);

// Resource types
#define RLIMIT_CPU      0   // CPU time in seconds
#define RLIMIT_FSIZE    1   // Maximum file size
#define RLIMIT_DATA     2   // Maximum data segment
#define RLIMIT_STACK    3   // Maximum stack size
#define RLIMIT_CORE     4   // Maximum core file size
#define RLIMIT_NOFILE   5   // Maximum open files
#define RLIMIT_AS       6   // Address space limit
```

## Usage Examples

### Creating a Process

```c
// Fork and exec pattern
pid_t pid = process_fork();
if (pid == 0) {
    // Child process
    char *argv[] = {"/bin/program", "arg1", NULL};
    char *envp[] = {"PATH=/bin", NULL};
    process_exec("/bin/program", argv, envp);
    // Should not reach here
    process_exit(1);
} else if (pid > 0) {
    // Parent process
    int status;
    process_wait(pid, &status, 0);
    kprintf("Child exited with status %d\n", status);
} else {
    // Error
    kprintf("Fork failed: %d\n", pid);
}
```

### Creating a Thread

```c
// Thread function
void worker_thread(void *arg) {
    int *data = (int *)arg;
    // Do work...
    thread_exit(NULL);
}

// Create thread
int data = 42;
tid_t tid = thread_create(worker_thread, &data, 0);
if (tid > 0) {
    // Wait for thread
    thread_join(tid, NULL);
}
```

### Using Wait Queues

```c
// Condition variable pattern
wait_queue_t queue;
volatile bool condition = false;

// Initialize
wait_queue_init(&queue);

// Waiting thread
bool check_condition(void *arg) {
    return condition;
}
wait_queue_wait(&queue, check_condition, 0);

// Signaling thread
condition = true;
wait_queue_wake_all(&queue);
```

### Priority Management

```c
// Boost priority for interactive task
scheduler_set_priority(0, PRIORITY_HIGH);

// Do time-critical work...

// Restore normal priority
scheduler_set_priority(0, PRIORITY_NORMAL);
```

## Performance Considerations

1. **Process creation**: Fork is expensive, use threads for parallelism
2. **Context switching**: Minimize by proper priority assignment
3. **Memory sharing**: Use shared memory for IPC when possible
4. **CPU affinity**: Pin threads to CPUs for cache locality (future)

## Debugging Support

```c
/**
 * @brief Dump process information
 * @param pid Process ID (0 for all)
 */
void process_dump(pid_t pid);

/**
 * @brief Get thread backtrace
 * @param tid Thread ID
 * @param buffer Buffer for addresses
 * @param size Buffer size
 * @return Number of frames captured
 */
int thread_backtrace(tid_t tid, void **buffer, int size);

/**
 * @brief Attach debugger to process
 * @param pid Process ID
 * @return 0 on success, negative error code on failure
 */
int process_debug_attach(pid_t pid);
```