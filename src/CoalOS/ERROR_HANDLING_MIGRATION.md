# Coal OS Kernel Error Handling Standardization Guide

## Overview

This document provides guidelines for migrating from inconsistent error handling patterns to the standardized `error_t` system across the Coal OS kernel.

## Standardized Error Handling Principles

### 1. Consistent Return Types
- Use `error_t` for all functions that can fail
- Return `E_SUCCESS` on success, specific error codes on failure
- Use output parameters for actual results

### 2. Input Validation
- Always validate input parameters first
- Return `E_INVAL` for invalid inputs
- Use defensive programming practices

### 3. Error Propagation
- Use `RETURN_IF_ERROR()` macro for error propagation
- Provide meaningful error context in logs
- Don't lose error information in the call chain

## Migration Examples

### Example 1: Buddy Allocator Migration

#### OLD Pattern (Error-Prone):
```c
// In slab.c - slab_create_cache()
slab_cache_t *cache = (slab_cache_t *)buddy_alloc(sizeof(slab_cache_t));
if (!cache) { 
    /* ... error ... */ 
    return NULL; 
}

// Problems:
// 1. No error context - why did allocation fail?
// 2. Caller cannot distinguish between different failure types
// 3. NULL return forces all callers to use NULL checks
// 4. No standardized error reporting
```

#### NEW Pattern (Standardized):
```c
// Using new buddy_alloc_safe() API
error_t slab_create_cache_safe(const char* name, size_t user_obj_size, 
                               size_t align, size_t color_range,
                               slab_constructor_t constructor, 
                               slab_destructor_t destructor,
                               slab_cache_t** cache_out) {
    // Input validation
    if (!name || !cache_out) {
        return E_INVAL;
    }
    
    if (user_obj_size == 0 || user_obj_size > SLAB_MAX_OBJ_SIZE) {
        return E_INVAL;
    }

    // Calculate requirements
    size_t final_align = (align > 0) ? align : DEFAULT_ALIGNMENT;
    size_t internal_size_req = user_obj_size + SLAB_FOOTER_SIZE;
    size_t internal_slot_size = ALIGN_UP(internal_size_req, final_align);

    // Allocate cache descriptor with proper error handling
    slab_cache_t *cache = NULL;
    error_t err = buddy_alloc_safe(sizeof(slab_cache_t), (void**)&cache);
    if (err != E_SUCCESS) {
        // Log specific error with context
        serial_printf("[Slab] Failed to allocate cache descriptor for '%s': %s\n", 
                      name, error_to_string(err));
        return err; // Propagate specific error
    }

    // Initialize cache descriptor
    cache->name = name;
    cache->user_obj_size = user_obj_size;
    cache->internal_slot_size = internal_slot_size;
    cache->alignment = final_align;
    // ... rest of initialization

    // Validation checks
    if (SLAB_HEADER_SIZE + cache->internal_slot_size > PAGE_SIZE) {
        serial_printf("[Slab] Cache '%s': Page size too small (%zu + %zu > %zu)\n",
                      name, SLAB_HEADER_SIZE, cache->internal_slot_size, PAGE_SIZE);
        
        // Clean up and return specific error
        buddy_free_safe(cache); // Use safe free
        return E_NOSPC;
    }

    // Success
    *cache_out = cache;
    serial_printf("[Slab] Created cache '%s' (user=%zu, slot=%zu, align=%zu)\n",
                  name, cache->user_obj_size, cache->internal_slot_size, 
                  cache->alignment);
    return E_SUCCESS;
}

// Benefits:
// 1. Clear error context and specific error codes
// 2. Proper resource cleanup on failure
// 3. Consistent error reporting pattern
// 4. Input validation with meaningful errors
// 5. Standardized logging format
```

### Example 2: Process Management Migration

#### OLD Pattern (Error-Prone):
```c
// In process_pcb_manager.c
pcb_t* process_create(const char* name) {
    pcb_t *proc = kmalloc(sizeof(pcb_t));
    if (!proc) {
        serial_printf("[Process] Failed to allocate PCB\n");
        return NULL;
    }
    // ... initialization
    return proc;
}

// Problems:
// 1. No specific error information
// 2. Memory allocation failure reason unknown
// 3. Caller must check for NULL without context
```

#### NEW Pattern (Standardized):
```c
error_t process_create_safe(const char* name, pcb_t** proc_out) {
    // Input validation
    if (!name || !proc_out) {
        return E_INVAL;
    }
    
    if (strlen(name) == 0 || strlen(name) >= MAX_PROCESS_NAME_LEN) {
        return E_INVAL;
    }

    // Allocate PCB
    pcb_t *proc = NULL;
    error_t err = kmalloc_safe(sizeof(pcb_t), (void**)&proc);
    if (err != E_SUCCESS) {
        serial_printf("[Process] Failed to allocate PCB for '%s': %s\n", 
                      name, error_to_string(err));
        return err;
    }

    // Initialize PCB
    RETURN_IF_ERROR(process_init_pcb(proc, name));
    RETURN_IF_ERROR(process_init_memory(proc));
    RETURN_IF_ERROR(process_init_fds(proc));

    // Success
    *proc_out = proc;
    return E_SUCCESS;
}
```

### Example 3: Driver Initialization Migration

#### OLD Pattern (Mixed Return Types):
```c
// In keyboard.c
bool keyboard_init(void) {
    // ... initialization code
    if (some_failure) {
        return false;
    }
    return true;
}

int scheduler_add_task(pcb_t *pcb) {
    if (!pcb) return -1;
    // ... add task
    if (failure) return -1;
    return 0;
}
```

#### NEW Pattern (Standardized):
```c
error_t keyboard_init(void) {
    // Hardware initialization
    RETURN_IF_ERROR(keyboard_hw_detect());
    RETURN_IF_ERROR(keyboard_configure_controller());
    RETURN_IF_ERROR(keyboard_setup_interrupts());
    
    return E_SUCCESS;
}

error_t scheduler_add_task(pcb_t *pcb) {
    if (!pcb) {
        return E_INVAL;
    }
    
    RETURN_IF_ERROR(scheduler_validate_pcb(pcb));
    RETURN_IF_ERROR(scheduler_allocate_tcb(pcb));
    RETURN_IF_ERROR(scheduler_enqueue_task(pcb));
    
    return E_SUCCESS;
}
```

## Standardized Error Logging Macros

Add these to `error.h`:

```c
#define KERNEL_ERROR_LOG(fmt, ...) \
    serial_printf("[KERNEL_ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define KERNEL_WARN_LOG(fmt, ...) \
    serial_printf("[KERNEL_WARN] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

#define KERNEL_INFO_LOG(fmt, ...) \
    serial_printf("[KERNEL_INFO] %s: " fmt "\n", __func__, ##__VA_ARGS__)
```

## Migration Priorities

### Phase 1: Critical Infrastructure (Week 1)
1. **Memory Allocators**: buddy.c, kmalloc.c, slab.c
2. **Process Management**: process_pcb_manager.c

### Phase 2: System Services (Week 2)  
1. **Scheduler**: scheduler_core.c, scheduler_queues.c
2. **Memory Management**: paging_*.c, frame.c

### Phase 3: Drivers and Filesystem (Week 3)
1. **Hardware Drivers**: keyboard.c, timer/pit.c
2. **File System**: fat_dir_main.c, vfs modules

### Phase 4: Polish and Testing (Week 4)
1. **Error Message Standardization**
2. **Comprehensive Testing**
3. **Documentation Updates**

## Conversion Checklist

For each function being migrated:

- [ ] Change return type to `error_t`
- [ ] Add output parameters for actual results  
- [ ] Add input validation with `E_INVAL` returns
- [ ] Use `RETURN_IF_ERROR()` for error propagation
- [ ] Add meaningful error logging with context
- [ ] Update all callers to use new API
- [ ] Test error paths thoroughly
- [ ] Document error conditions in function headers

## Testing Error Paths

Each migrated function should be tested for:

1. **Invalid Input Handling**: Pass NULL, 0, or invalid parameters
2. **Resource Exhaustion**: Test behavior when memory/resources unavailable
3. **Error Propagation**: Verify errors bubble up correctly
4. **Resource Cleanup**: Ensure no leaks on error paths
5. **Error Logging**: Verify meaningful error messages

## Benefits of Standardization

1. **Improved Debugging**: Clear error context and specific error codes
2. **Better Reliability**: Consistent error handling reduces bugs
3. **Maintainability**: Standard patterns are easier to understand and modify
4. **Testing**: Standardized error paths are easier to test
5. **Documentation**: Clear error conditions in API documentation

## Backward Compatibility

During migration:

1. Keep old APIs functional for existing code
2. Add `_safe` variants with new error handling
3. Gradually migrate callers to new APIs
4. Mark old APIs as deprecated
5. Eventually remove old APIs after full migration

This approach ensures the system remains stable during the transition while providing clear migration path for all components.