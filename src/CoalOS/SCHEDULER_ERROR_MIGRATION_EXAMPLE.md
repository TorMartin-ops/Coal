# Scheduler Error Handling Migration Example

## Overview

This document demonstrates the migration from error-prone scheduler patterns to the standardized `error_t` system in Coal OS, completing the critical infrastructure error handling standardization.

## Before: Problematic Scheduler Error Handling

### Example 1: Task Addition (OLD)

```c
// From init.c - launch_program() function
void launch_program(const char *path, const char *description) {
    pcb_t *proc_pcb = create_user_process(path);
    if (proc_pcb) {
        if (scheduler_add_task(proc_pcb) == 0) {  // Returns 0/-1
            terminal_printf("  [OK] %s scheduled successfully.\n", description);
        } else {
            // Generic error - WHY did scheduling fail?
            terminal_printf("  [ERROR] Failed to add %s to scheduler!\n", description);
            destroy_process(proc_pcb); 
        }
    }
}
```

### Example 2: Scheduler Internal Functions (OLD)

```c
// From scheduler_core.c
int scheduler_core_add_task(pcb_t *pcb) {
    tcb_t *new_task = (tcb_t *)kmalloc(sizeof(tcb_t));
    if (!new_task) { 
        SCHED_ERROR("kmalloc TCB failed for PID %lu", pcb->pid); 
        return -1;  // No specific error context!
    }
    
    // ... initialization code ...
    
    if (!scheduler_queues_enqueue_ready_task(new_task)) {
        SCHED_ERROR("Failed to enqueue newly created task PID %lu!", new_task->pid);
        return -1;  // Same error code for different failures!
    }
    
    return 0;
}
```

**Problems with OLD pattern:**
- ❌ Generic `-1` return provides no error context
- ❌ Cannot distinguish between memory allocation vs. queue failures
- ❌ No input validation with specific error codes
- ❌ Difficult to implement appropriate error recovery
- ❌ Poor debugging experience in production

## After: Standardized Scheduler Error Handling

### Example 1: Task Addition (NEW)

```c
// Using new standardized scheduler API
error_t launch_program_safe(const char *path, const char *description) {
    terminal_printf("[Kernel] Attempting to launch %s from '%s'...\n", description, path);
    
    // Create process with standardized error handling
    pcb_t *proc_pcb = NULL;
    error_t err = create_user_process_safe(path, &proc_pcb);
    if (err != E_SUCCESS) {
        terminal_printf("  [ERROR] Failed to create %s: %s\n", 
                       description, error_to_string(err));
        return err;
    }
    
    // Add to scheduler with specific error handling
    err = scheduler_core_add_task_safe(proc_pcb);
    switch (err) {
        case E_SUCCESS:
            terminal_printf("  [OK] %s (PID %u) scheduled successfully.\n", 
                           description, proc_pcb->pid);
            return E_SUCCESS;
            
        case E_INVAL:
            terminal_printf("  [ERROR] Invalid process state for %s (PID %u)\n", 
                           description, proc_pcb->pid);
            destroy_process(proc_pcb);
            return E_INVAL;
            
        case E_NOMEM:
            terminal_printf("  [ERROR] Insufficient memory to schedule %s\n", description);
            destroy_process(proc_pcb);
            // Could implement memory cleanup and retry here
            return E_NOMEM;
            
        case E_FAULT:
            terminal_printf("  [ERROR] Scheduler queue corruption detected while adding %s\n", 
                           description);
            destroy_process(proc_pcb);
            // Could trigger scheduler integrity check here
            return E_FAULT;
            
        case E_EXIST:
            terminal_printf("  [ERROR] Task with PID %u already exists in scheduler\n", 
                           proc_pcb->pid);
            destroy_process(proc_pcb);
            return E_EXIST;
            
        default:
            terminal_printf("  [ERROR] Unexpected scheduler error for %s: %s\n", 
                           description, error_to_string(err));
            destroy_process(proc_pcb);
            return err;
    }
}
```

### Example 2: High-Level Scheduler Management (NEW)

```c
// Service management with comprehensive error handling
error_t start_system_service(const char *service_name, const char *exec_path) {
    // Validate inputs
    if (!service_name || !exec_path) {
        return E_INVAL;
    }
    
    serial_printf("[Service] Starting system service '%s'...\n", service_name);
    
    // Create process
    pcb_t *service_proc = NULL;
    RETURN_IF_ERROR(create_user_process_safe(exec_path, &service_proc));
    
    // Mark as system service (higher priority)
    service_proc->is_kernel_task = false; // User process but system service
    service_proc->service_flags |= SERVICE_FLAG_SYSTEM; // Custom flag
    
    // Add to scheduler with error handling
    error_t err = scheduler_core_add_task_safe(service_proc);
    if (err != E_SUCCESS) {
        serial_printf("[Service] Failed to schedule service '%s': %s\n", 
                      service_name, error_to_string(err));
        
        // Service-specific error recovery
        switch (err) {
            case E_NOMEM:
                // Try to free some memory and retry once
                kernel_emergency_gc();
                err = scheduler_core_add_task_safe(service_proc);
                if (err == E_SUCCESS) {
                    serial_printf("[Service] Service '%s' scheduled after memory cleanup\n", 
                                  service_name);
                    return E_SUCCESS;
                }
                break;
                
            case E_FAULT:
                // Scheduler corruption - trigger integrity check
                scheduler_integrity_check();
                break;
        }
        
        destroy_process(service_proc);
        return err;
    }
    
    serial_printf("[Service] System service '%s' started successfully (PID %u)\n", 
                  service_name, service_proc->pid);
    return E_SUCCESS;
}
```

### Example 3: Scheduler State Management (NEW)

```c
// Task monitoring with error validation
error_t monitor_task_health(uint32_t pid) {
    // Get current task safely
    tcb_t *current_task = NULL;
    error_t err = scheduler_core_get_current_task_safe(&current_task);
    
    switch (err) {
        case E_SUCCESS:
            // Normal operation
            break;
            
        case E_NOTFOUND:
            serial_printf("[Monitor] No current task - system may be idle\n");
            return E_NOTFOUND;
            
        case E_FAULT:
            serial_printf("[Monitor] CRITICAL: Current task state corruption detected!\n");
            // Could trigger system integrity check or recovery
            scheduler_emergency_recovery();
            return E_FAULT;
            
        default:
            serial_printf("[Monitor] Unexpected error getting current task: %s\n", 
                          error_to_string(err));
            return err;
    }
    
    // Validate task health
    if (current_task->pid == pid) {
        // Check for various health indicators
        if (current_task->ticks_remaining == 0 && current_task->state == TASK_RUNNING) {
            serial_printf("[Monitor] Warning: Task PID %u has consumed time slice but still running\n", pid);
            return E_TIMEOUT;
        }
        
        if (!current_task->process) {
            serial_printf("[Monitor] CRITICAL: Task PID %u has NULL process pointer\n", pid);
            return E_FAULT;
        }
        
        serial_printf("[Monitor] Task PID %u health: OK (State: %d, Ticks: %u)\n", 
                      pid, current_task->state, current_task->ticks_remaining);
        return E_SUCCESS;
    }
    
    return E_NOTFOUND; // Requested PID is not the current task
}
```

## Migration Benefits Analysis

### 1. Specific Error Diagnosis

**Before (Generic)**:
```c
if (scheduler_add_task(proc) != 0) {
    // Why did it fail? Memory? Corruption? Invalid state?
    printf("Scheduler failed\n");  // Useless!
}
```

**After (Specific)**:
```c
error_t err = scheduler_core_add_task_safe(proc);
switch (err) {
    case E_NOMEM:
        printf("Insufficient memory for scheduler - try cleanup\n");
        break;
    case E_FAULT:
        printf("Scheduler corruption detected - run integrity check\n");
        break;
    case E_INVAL:
        printf("Process %u missing required fields\n", proc->pid);
        break;
}
```

### 2. Intelligent Error Recovery

**Before (No Recovery)**:
```c
int result = scheduler_add_task(proc);
if (result != 0) {
    return -1;  // Give up immediately
}
```

**After (Smart Recovery)**:
```c
error_t err = scheduler_core_add_task_safe(proc);
if (err == E_NOMEM) {
    // Try memory cleanup and retry
    kernel_gc_memory();
    err = scheduler_core_add_task_safe(proc);
    if (err == E_SUCCESS) {
        serial_printf("Task scheduled after memory cleanup\n");
        return E_SUCCESS;
    }
}

if (err == E_FAULT) {
    // Scheduler corruption - attempt recovery
    scheduler_integrity_check();
    scheduler_rebuild_queues();
    err = scheduler_core_add_task_safe(proc);
}
```

### 3. Enhanced Debugging and Monitoring

**Before (Minimal Info)**:
```c
if (scheduler_add_task(proc) != 0) {
    // Log generic failure
    log("Scheduler add failed");
}
```

**After (Rich Context)**:
```c
error_t err = scheduler_core_add_task_safe(proc);
if (err != E_SUCCESS) {
    // Log specific failure with context
    log_scheduler_error(err, proc->pid, proc->name, 
                       get_memory_stats(), get_scheduler_stats());
    
    // Could trigger automated diagnostics
    if (err == E_FAULT) {
        dump_scheduler_state();
        check_memory_corruption();
    }
}
```

### 4. Consistent Error Propagation

**Before (Mixed Types)**:
```c
bool create_proc_result = create_process(name);
int sched_result = scheduler_add_task(proc);
void* mem_result = allocate_memory(size);
```

**After (Unified)**:
```c
RETURN_IF_ERROR(create_process_safe(name, &proc));
RETURN_IF_ERROR(scheduler_core_add_task_safe(proc));
RETURN_IF_ERROR(allocate_memory_safe(size, &ptr));
```

## Scheduler Error Types and Recovery Strategies

### Error Type: `E_NOMEM` (Memory Exhaustion)
```c
// Recovery strategies for memory pressure
if (err == E_NOMEM) {
    // 1. Trigger garbage collection
    kernel_gc_memory();
    
    // 2. Try to reclaim unused kernel stacks
    scheduler_cleanup_dead_tasks();
    
    // 3. Reduce cache sizes temporarily
    vfs_reduce_cache_size();
    
    // 4. Retry the operation
    err = scheduler_core_add_task_safe(proc);
}
```

### Error Type: `E_FAULT` (Scheduler Corruption)
```c
// Recovery strategies for scheduler corruption
if (err == E_FAULT) {
    // 1. Dump current scheduler state for analysis
    dump_scheduler_state();
    
    // 2. Validate all task structures
    scheduler_validate_all_tasks();
    
    // 3. Rebuild queues if corruption is limited
    scheduler_rebuild_queues();
    
    // 4. If severe, trigger controlled system restart
    if (corruption_severity > CRITICAL_THRESHOLD) {
        initiate_controlled_restart("Scheduler corruption");
    }
}
```

### Error Type: `E_INVAL` (Invalid Process State)
```c
// Recovery strategies for invalid process state
if (err == E_INVAL) {
    // 1. Validate process structure
    if (!validate_pcb_structure(proc)) {
        serial_printf("PCB PID %u has corrupted structure\n", proc->pid);
        return E_CORRUPT;
    }
    
    // 2. Check if required resources are allocated
    if (!proc->kernel_stack_vaddr_top) {
        err = allocate_kernel_stack_safe(proc);
        if (err == E_SUCCESS) {
            return scheduler_core_add_task_safe(proc); // Retry
        }
    }
}
```

## Integration with Existing Code

### Backward Compatibility Layer
```c
// Legacy wrapper for existing code
int scheduler_add_task(pcb_t *pcb) {
    error_t err = scheduler_core_add_task_safe(pcb);
    
    // Convert error_t to legacy return codes
    switch (err) {
        case E_SUCCESS: return 0;
        case E_NOMEM:   return -ENOMEM;
        case E_INVAL:   return -EINVAL;
        case E_FAULT:   return -EFAULT;
        default:        return -1;
    }
}
```

### Gradual Migration Path
```c
// Phase 1: New code uses safe APIs
error_t new_feature_scheduler_integration() {
    pcb_t *proc = NULL;
    RETURN_IF_ERROR(create_user_process_safe(path, &proc));
    RETURN_IF_ERROR(scheduler_core_add_task_safe(proc));
    return E_SUCCESS;
}

// Phase 2: Legacy code continues working
void legacy_feature() {
    pcb_t *proc = create_user_process(path);
    if (proc && scheduler_add_task(proc) == 0) {
        // Old logic continues to work
    }
}
```

## Testing the New Error Handling

### Unit Tests for Error Conditions
```c
void test_scheduler_error_handling() {
    pcb_t *proc = NULL;
    
    // Test invalid input handling
    assert(scheduler_core_add_task_safe(NULL) == E_INVAL);
    
    // Test memory exhaustion
    mock_kmalloc_failure(true);
    assert(scheduler_core_add_task_safe(valid_proc) == E_NOMEM);
    mock_kmalloc_failure(false);
    
    // Test missing PCB fields
    pcb_t incomplete_proc = {0};
    assert(scheduler_core_add_task_safe(&incomplete_proc) == E_INVAL);
    
    // Test queue corruption
    mock_queue_failure(true);
    assert(scheduler_core_add_task_safe(valid_proc) == E_FAULT);
    mock_queue_failure(false);
}
```

## Implementation Status Summary

### ✅ Completed Standardized Scheduler APIs

1. **`scheduler_core_add_task_safe()`** - Task addition with comprehensive validation:
   - `E_INVAL` for invalid PCB or missing required fields
   - `E_NOMEM` for TCB allocation failures
   - `E_FAULT` for queue operation failures
   - `E_EXIST` for duplicate PID detection

2. **`scheduler_select_next_task_safe()`** - Task selection with validation:
   - `E_INVAL` for null output parameter  
   - `E_NOTFOUND` for no available tasks
   - `E_FAULT` for corrupted task states

3. **`scheduler_core_get_current_task_safe()`** - Current task access with checks:
   - `E_INVAL` for null output parameter
   - `E_NOTFOUND` for no current task
   - `E_FAULT` for invalid task state

### 🔧 Key Improvements Over Legacy Code

- **Comprehensive Input Validation**: All PCB fields verified before task creation
- **Specific Error Codes**: Distinct errors for memory, corruption, and validation failures  
- **Resource Cleanup**: Proper cleanup on all error paths
- **Enhanced Logging**: Context-rich error messages with PID and failure details
- **Corruption Detection**: Validation of task state consistency

### 📈 System Reliability Impact

The scheduler error handling standardization provides:

1. **Better System Stability** - Early detection of invalid process states
2. **Improved Debugging** - Specific error codes pinpoint exact failure causes
3. **Proactive Monitoring** - Error patterns can indicate system health issues
4. **Recovery Capabilities** - Intelligent error handling enables graceful degradation

This completes the critical infrastructure error handling migration for Coal OS, providing a robust foundation for reliable process scheduling and system operation.