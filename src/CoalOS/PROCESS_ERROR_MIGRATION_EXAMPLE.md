# Process Management Error Handling Migration Example

## Overview

This document demonstrates the migration from error-prone process management patterns to the standardized `error_t` system in Coal OS.

## Before: Problematic Error Handling Patterns

### Example 1: Process Creation (OLD)

```c
// From init.c - launch_program() function
void launch_program(const char *path, const char *description) {
    terminal_printf("[Kernel] Attempting to launch %s from '%s'...\n", description, path);
    
    pcb_t *proc_pcb = create_user_process(path);  // Returns NULL on failure
    if (proc_pcb) {
        if (scheduler_add_task(proc_pcb) == 0) {
            terminal_printf("  [OK] %s (PID %lu) scheduled successfully.\n", 
                           description, (unsigned long)proc_pcb->pid);
        } else {
            terminal_printf("  [ERROR] Failed to add %s to scheduler!\n", description);
            destroy_process(proc_pcb); 
        }
    } else {
        // NO ERROR CONTEXT - why did process creation fail?
        terminal_printf("  [ERROR] Failed to create process for %s from '%s'.\n", 
                       description, path);
    }
}
```

**Problems with OLD pattern:**
- ❌ No error context - was it file not found? Out of memory? Corrupted ELF?
- ❌ Cannot provide user-friendly error messages
- ❌ Difficult to implement retry logic or fallback strategies
- ❌ Hard to debug process creation failures in production

### Example 2: Memory Allocation (OLD)

```c
// From process_pcb_manager.c
pcb_t* process_create(const char* name) {
    pcb_t* proc = (pcb_t*)kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_printf("[Process] Failed to allocate PCB\n");  // No specific error
        return NULL;
    }
    // ... rest of initialization
    return proc;
}
```

**Problems with OLD pattern:**
- ❌ Generic "allocation failed" message
- ❌ No distinction between temporary memory pressure vs. system exhaustion
- ❌ Caller cannot implement appropriate error recovery

## After: Standardized Error Handling

### Example 1: Process Creation (NEW)

```c
// Using new standardized API
error_t launch_program_safe(const char *path, const char *description) {
    terminal_printf("[Kernel] Attempting to launch %s from '%s'...\n", description, path);
    
    pcb_t *proc_pcb = NULL;
    error_t err = create_user_process_safe(path, &proc_pcb);
    
    switch (err) {
        case E_SUCCESS:
            // Process created successfully, try to schedule it
            err = scheduler_add_task_safe(proc_pcb);
            if (err == E_SUCCESS) {
                terminal_printf("  [OK] %s (PID %u) scheduled successfully.\n", 
                               description, proc_pcb->pid);
                return E_SUCCESS;
            } else {
                terminal_printf("  [ERROR] Failed to schedule %s: %s\n", 
                               description, error_to_string(err));
                destroy_process(proc_pcb);
                return err;
            }
            
        case E_NOTFOUND:
            terminal_printf("  [ERROR] Executable file '%s' not found\n", path);
            return E_NOTFOUND;
            
        case E_NOMEM:
            terminal_printf("  [ERROR] Insufficient memory to create %s\n", description);
            // Could implement memory cleanup or retry logic here
            return E_NOMEM;
            
        case E_CORRUPT:
            terminal_printf("  [ERROR] Invalid or corrupted executable '%s'\n", path);
            return E_CORRUPT;
            
        default:
            terminal_printf("  [ERROR] Failed to create %s: %s\n", 
                           description, error_to_string(err));
            return err;
    }
}
```

**Benefits of NEW pattern:**
- ✅ Specific error codes enable targeted error handling
- ✅ Clear, actionable error messages for users
- ✅ Enables retry logic for transient failures (E_NOMEM)
- ✅ Proper error propagation through call chain
- ✅ Consistent error reporting format

### Example 2: Memory Allocation (NEW)

```c
// Using standardized process creation API
error_t create_service_process(const char* service_name, const char* exec_path, pcb_t** proc_out) {
    // Input validation with specific error codes
    if (!service_name || !exec_path || !proc_out) {
        return E_INVAL;
    }
    
    // Create PCB with detailed error context
    pcb_t* proc = NULL;
    error_t err = process_create_safe(service_name, &proc);
    if (err != E_SUCCESS) {
        serial_printf("[Service] Failed to create PCB for '%s': %s\n", 
                      service_name, error_to_string(err));
        return err;
    }
    
    // Allocate kernel stack with specific error handling
    err = allocate_kernel_stack_safe(proc);
    if (err != E_SUCCESS) {
        serial_printf("[Service] Failed to allocate kernel stack for '%s': %s\n", 
                      service_name, error_to_string(err));
        
        // Clean up PCB on stack allocation failure
        destroy_process(proc);
        return err;
    }
    
    // Initialize file descriptors
    process_init_fds(proc);
    
    // Load executable with error context
    err = create_user_process_safe(exec_path, &proc);
    if (err != E_SUCCESS) {
        serial_printf("[Service] Failed to load executable '%s' for service '%s': %s\n", 
                      exec_path, service_name, error_to_string(err));
        
        // Could implement fallback executable loading here
        destroy_process(proc);
        return err;
    }
    
    // Success
    *proc_out = proc;
    serial_printf("[Service] Successfully created service '%s' with PID %u\n", 
                  service_name, proc->pid);
    return E_SUCCESS;
}
```

## Migration Benefits Summary

### 1. Better Debugging
```c
// OLD: Generic failure
if (!proc) {
    // Why did it fail? Memory? File? Corruption?
    return NULL;
}

// NEW: Specific error context
error_t err = process_create_safe(name, &proc);
switch (err) {
    case E_NOMEM: /* Handle memory pressure */
    case E_OVERFLOW: /* Handle PID exhaustion */
    case E_INVAL: /* Handle invalid parameters */
}
```

### 2. Robust Error Recovery
```c
// OLD: No recovery possible
pcb_t* proc = create_user_process(path);
if (!proc) {
    return NULL;  // Give up
}

// NEW: Intelligent error recovery
error_t err = create_user_process_safe(path, &proc);
if (err == E_NOMEM) {
    // Try garbage collection and retry
    kernel_gc_memory();
    err = create_user_process_safe(path, &proc);
}
if (err == E_NOTFOUND) {
    // Try alternative executable location
    err = create_user_process_safe("/bin/fallback", &proc);
}
```

### 3. Consistent Error Propagation
```c
// OLD: Inconsistent error types
bool allocate_kernel_stack(pcb_t *proc);  // true/false
pcb_t* process_create(const char* name);   // pointer/NULL
int scheduler_add_task(pcb_t *pcb);        // 0/-1

// NEW: Consistent error types
error_t allocate_kernel_stack_safe(pcb_t *proc);
error_t process_create_safe(const char* name, pcb_t** proc_out);
error_t scheduler_add_task_safe(pcb_t *pcb);

// All use RETURN_IF_ERROR() for clean propagation
RETURN_IF_ERROR(process_create_safe(name, &proc));
RETURN_IF_ERROR(allocate_kernel_stack_safe(proc));
RETURN_IF_ERROR(scheduler_add_task_safe(proc));
```

### 4. Improved Testing
```c
// OLD: Hard to test error paths
void test_process_creation() {
    // How do you test memory exhaustion? File not found?
    // No way to distinguish error types
}

// NEW: Comprehensive error path testing
void test_process_creation_errors() {
    pcb_t* proc = NULL;
    
    // Test invalid input handling
    assert(process_create_safe(NULL, &proc) == E_INVAL);
    assert(process_create_safe("test", NULL) == E_INVAL);
    
    // Test memory exhaustion (mock kmalloc failure)
    mock_kmalloc_failure(true);
    assert(process_create_safe("test", &proc) == E_NOMEM);
    mock_kmalloc_failure(false);
    
    // Test PID overflow
    mock_pid_counter(UINT32_MAX);
    assert(process_create_safe("test", &proc) == E_OVERFLOW);
}
```

## Implementation Status

### ✅ Completed Standardized APIs

1. **`buddy_alloc_safe()`** - Memory allocation with specific error codes
2. **`process_create_safe()`** - PCB creation with detailed error context
3. **`get_current_process_safe()`** - Current process access with validation
4. **`allocate_kernel_stack_safe()`** - Kernel stack allocation with error types
5. **`create_user_process_safe()`** - User process creation with file validation

### 🔄 Integration Points

The new APIs are backward compatible and can be gradually adopted:

```c
// Immediate adoption in new code
error_t new_feature() {
    pcb_t* proc = NULL;
    RETURN_IF_ERROR(process_create_safe("service", &proc));
    RETURN_IF_ERROR(allocate_kernel_stack_safe(proc));
    return E_SUCCESS;
}

// Gradual migration of existing code
void legacy_function() {
    // Keep using old API for now
    pcb_t* proc = process_create("legacy");
    if (proc) {
        // Old logic...
    }
}
```

### 📈 Next Steps

1. **Scheduler Functions**: Convert `scheduler_add_task()` to return `error_t`
2. **Memory Management**: Standardize remaining paging and frame allocation functions
3. **Driver Initialization**: Convert bool returns to error_t with specific error codes
4. **Filesystem Operations**: Align VFS error handling with kernel error_t system

This migration provides a solid foundation for reliable, debuggable, and maintainable process management in Coal OS.