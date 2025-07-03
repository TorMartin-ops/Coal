# Process Management

Coal OS implements a comprehensive process management system with efficient scheduling, signal handling, and inter-process communication.

## Overview

The process management subsystem handles:
- Process creation and termination
- CPU scheduling
- Context switching
- Signal delivery
- Process synchronization
- File descriptor management

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                User Processes                       │
├─────────────────────────────────────────────────────┤
│               System Call Interface                 │
├─────────────────────────────────────────────────────┤
│          Process Management Subsystem               │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────┐ │
│  │  Scheduler  │  │   Process   │  │   Signal   │ │
│  │    (O(1))   │  │  Creation   │  │  Handler   │ │
│  └─────────────┘  └─────────────┘  └────────────┘ │
├─────────────────────────────────────────────────────┤
│              Context Switching (ASM)                │
└─────────────────────────────────────────────────────┘
```

## Process Control Block (PCB)

Each process is represented by a PCB:

```c
typedef struct pcb {
    // Identity
    pid_t pid;                    // Process ID
    pid_t ppid;                   // Parent process ID
    pid_t pgid;                   // Process group ID
    uid_t uid;                    // User ID
    gid_t gid;                    // Group ID
    
    // Memory Management
    page_directory_t* page_directory;  // Virtual address space
    uint32_t brk;                     // Program break
    void* kernel_stack;               // Kernel stack pointer
    void* user_stack_top;             // User stack top
    
    // Execution State
    uint32_t entry_point;             // Program entry
    task_state_t state;               // Current state
    int exit_code;                    // Exit status
    
    // File Descriptors
    file_descriptor_t* fd_table[MAX_FD];  // Open files
    char* working_dir;                    // Current directory
    
    // Scheduling
    tcb_t* main_thread;              // Main thread TCB
    uint32_t cpu_time;               // CPU time used
    
    // Hierarchy
    struct pcb* parent;              // Parent process
    struct pcb* children;            // Child list
    struct pcb* siblings;            // Sibling list
} pcb_t;
```

## Task Control Block (TCB)

Thread-level control structure:

```c
typedef struct tcb {
    // Scheduling
    struct tcb* next;            // Next in queue
    task_state_t state;          // Thread state
    uint8_t priority;            // Base priority
    uint8_t effective_priority;  // With inheritance
    uint32_t time_slice;         // Quantum
    
    // Context
    context_t* context;          // CPU context
    pcb_t* process;             // Parent process
    
    // Synchronization
    void* wait_reason;          // What we're waiting for
    struct tcb* blocked_list;   // Tasks we're blocking
} tcb_t;
```

## Scheduler

Coal OS uses an O(1) scheduler with bitmap-based priority tracking:

### Features
- **O(1) Complexity**: Constant time task selection
- **Priority Levels**: 4 levels (0=highest, 3=idle)
- **Time Slicing**: Priority-based quantum
- **Priority Inheritance**: Prevents priority inversion
- **Interactive Boost**: Favors interactive tasks

### Implementation

#### Bitmap-Based Selection
```c
// Find highest priority with ready tasks
int bitmap_find_first_set(bitmap_t* bitmap) {
    for (int i = 0; i < PRIORITY_LEVELS; i++) {
        if (bitmap->bits[i / 32] & (1 << (i % 32))) {
            return i;
        }
    }
    return -1;
}
```

#### Time Slices
| Priority | Time Slice | Use Case |
|----------|------------|----------|
| 0        | 200ms      | Kernel tasks |
| 1        | 100ms      | Interactive |
| 2        | 50ms       | Normal |
| 3        | 25ms       | Idle/batch |

### Scheduling Algorithm
```c
tcb_t* schedule_next(void) {
    // 1. Check sleeping tasks for wakeup
    wakeup_sleeping_tasks();
    
    // 2. Find highest priority with ready tasks
    int priority = bitmap_find_first_set(&ready_bitmap);
    if (priority < 0) return idle_task;
    
    // 3. Get task from priority queue
    tcb_t* task = dequeue_ready(priority);
    
    // 4. Apply interactive boost if needed
    if (task->interactive_score > BOOST_THRESHOLD) {
        boost_priority(task);
    }
    
    return task;
}
```

## Process Creation

### Fork-Exec Model
```c
// Fork - create child process
pid_t fork(void) {
    pcb_t* parent = current_process();
    pcb_t* child = process_duplicate(parent);
    
    if (child) {
        // Parent returns child PID
        return child->pid;
    } else {
        // Child returns 0
        return 0;
    }
}

// Exec - replace process image
int exec(const char* path, char* const argv[]) {
    // Load new program
    elf_info_t info;
    if (elf_load(path, &info) < 0) {
        return -1;
    }
    
    // Replace address space
    process_replace_image(&info, argv);
    
    // Jump to entry point
    jump_to_user_mode(info.entry);
}
```

### Process States
```
    ┌─────┐ fork() ┌─────────┐
    │ NEW │──────>│ READY   │<──────┐
    └─────┘       └────┬────┘       │
                       │            │ wakeup
                   schedule     ┌───┴────┐
                       │        │SLEEPING│
                       ▼        └────────┘
                  ┌─────────┐       ▲
                  │ RUNNING │───────┘
                  └────┬────┘    sleep
                       │
                    exit/kill
                       │
                       ▼
                  ┌─────────┐
                  │ ZOMBIE  │
                  └─────────┘
```

## Context Switching

Efficient context switching in assembly:

```nasm
; Save current context
simple_switch:
    push ebp
    push ebx
    push esi
    push edi
    
    ; Switch stacks
    mov [eax], esp      ; Save old ESP
    mov esp, edx        ; Load new ESP
    
    ; Restore new context
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
```

### Context Switch Overhead
- ~2000 cycles on modern CPUs
- Includes TLB flush cost
- Mitigated by process affinity

## Signal Handling

POSIX-style signal implementation:

### Supported Signals
| Signal | Number | Default Action |
|--------|--------|----------------|
| SIGKILL | 9 | Terminate |
| SIGTERM | 15 | Terminate |
| SIGSTOP | 19 | Stop |
| SIGCONT | 18 | Continue |
| SIGCHLD | 17 | Ignore |
| SIGSEGV | 11 | Core dump |

### Signal Delivery
```c
void deliver_signal(tcb_t* task, int signum) {
    // Check if signal is blocked
    if (task->signal_mask & (1 << signum)) {
        // Queue for later delivery
        queue_pending_signal(task, signum);
        return;
    }
    
    // Save current context
    save_context_for_signal(task);
    
    // Set up signal handler frame
    setup_signal_frame(task, signum);
    
    // Return to user with handler
}
```

## Inter-Process Communication

### Pipes
```c
int pipe(int fd[2]) {
    pipe_t* p = pipe_create();
    fd[0] = process_add_fd(p->read_end);
    fd[1] = process_add_fd(p->write_end);
    return 0;
}
```

### Shared Memory
```c
void* shm_create(size_t size, int flags) {
    // Allocate physical frames
    void* phys = frame_alloc_contiguous(size / PAGE_SIZE);
    
    // Map into process space
    void* virt = process_map_shared(phys, size, flags);
    
    return virt;
}
```

## Process Synchronization

### Spinlocks
- Used for short critical sections
- Busy-wait implementation
- Interrupt-safe variants

### Mutexes
- For longer critical sections
- Sleep-based waiting
- Priority inheritance support

### Semaphores
- Counting semaphores
- Binary semaphores
- Timeout support

## Performance Monitoring

### Process Statistics
```c
typedef struct {
    uint64_t user_time;      // Time in user mode
    uint64_t kernel_time;    // Time in kernel mode
    uint64_t start_time;     // Process start time
    uint32_t voluntary_cs;   // Voluntary context switches
    uint32_t involuntary_cs; // Preemptions
    size_t memory_usage;     // Current memory usage
    size_t peak_memory;      // Peak memory usage
} process_stats_t;
```

### System-wide Stats
- Total processes
- Running/sleeping/zombie counts
- Context switches per second
- CPU utilization
- Load average

## Security Features

### Process Isolation
- Separate address spaces
- Kernel/user mode separation
- File descriptor limits
- Resource limits (rlimits)

### Capabilities
- Fine-grained permissions
- Privilege dropping
- Capability inheritance

## Debugging Support

### Process Inspection
```bash
# In kernel debugger
ps              # List all processes
pinfo <pid>     # Detailed process info
backtrace <pid> # Stack trace
memdump <pid>   # Memory dump
```

### Tracing
- System call tracing
- Signal tracing
- Context switch logging

## Future Enhancements

1. **SMP Support**: Multi-core scheduling
2. **Real-time**: RT scheduling classes
3. **Cgroups**: Resource control groups
4. **Namespaces**: Container support
5. **NUMA**: Non-uniform memory access