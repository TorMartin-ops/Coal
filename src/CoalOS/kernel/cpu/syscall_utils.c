/**
 * @file syscall_utils.c
 * @brief Shared Utilities for System Call Implementations
 * @author Refactored for SOLID principles
 * @version 6.0
 * 
 * @details Common utility functions used across multiple system call modules
 * including user space access, memory management, and argument parsing.
 */

//============================================================================
// Includes
//============================================================================
#include "syscall_utils.h"
#include <kernel/memory/uaccess.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/paging.h>
#include <kernel/memory/paging_process.h>
#include <kernel/memory/mm.h>
#include <kernel/process/process.h>
#include <kernel/fs/vfs/sys_file.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/fs/vfs/fs_errno.h>

//============================================================================
// Configuration
//============================================================================
// Note: KERNEL_SPACE_VIRT_START is already defined in paging.h

//============================================================================
// User Space Access Utilities Implementation
//============================================================================

int strncpy_from_user_safe(const_userptr_t u_src, char *k_dst, size_t maxlen)
{
    KERNEL_ASSERT(k_dst != NULL, "k_dst cannot be NULL in strncpy_from_user_safe");
    if (maxlen == 0) return -EINVAL;
    k_dst[0] = '\0';

    // Basic check, uaccess.c's access_ok will do more thorough VMA checks.
    if (!u_src || (uintptr_t)u_src >= KERNEL_SPACE_VIRT_START) {
        return -EFAULT;
    }
    
    // Check at least one byte can be touched by access_ok
    if (!access_ok(VERIFY_READ, u_src, 1)) {
        return -EFAULT;
    }

    size_t len = 0;
    while (len < maxlen - 1) { // Leave space for null terminator
        char current_char;
        if (copy_from_user((kernelptr_t)&current_char, 
                          (const_userptr_t)((const char*)u_src + len), 1) != 0) {
            k_dst[len] = '\0'; // Null-terminate on partial copy due to fault
            return -EFAULT;
        }
        k_dst[len] = current_char;
        if (current_char == '\0') {
            return 0; // Success, null terminator copied
        }
        len++;
    }

    k_dst[len] = '\0'; // Maxlen-1 chars copied, ensure null termination
    
    // Check if the original string was actually longer (meaning truncation occurred)
    char next_char_check;
    if (copy_from_user((kernelptr_t)&next_char_check, 
                      (const_userptr_t)((const char*)u_src + len), 1) == 0 && 
        next_char_check != '\0') {
        return -ENAMETOOLONG; // Original string was longer
    }
    
    return 0; // String fit or was shorter and NUL terminated within maxlen
}

//============================================================================
// Process Memory Management Implementation
//============================================================================

static bool copy_vma_tree_simple(mm_struct_t *child_mm, mm_struct_t *parent_mm)
{
    if (!child_mm || !parent_mm || !parent_mm->vma_tree.root) {
        return true; // Nothing to copy
    }
    
    // Use a simple approach: find all VMAs and copy them one by one
    // This is not the most efficient but avoids complex tree traversal
    
    // Start from 0 and scan through address space
    uintptr_t scan_addr = 0;
    int vmas_copied = 0;
    
    while (scan_addr < KERNEL_SPACE_VIRT_START) {
        vma_struct_t *parent_vma = find_vma(parent_mm, scan_addr);
        if (!parent_vma) {
            // No VMA at this address, skip to next potential VMA location
            scan_addr += PAGE_SIZE;
            continue;
        }
        
        // Copy this VMA to child
        vma_struct_t *child_vma = insert_vma(child_mm,
                                             parent_vma->vm_start,
                                             parent_vma->vm_end,
                                             parent_vma->vm_flags,
                                             parent_vma->page_prot,
                                             parent_vma->vm_file,
                                             parent_vma->vm_offset);
        
        if (!child_vma) {
            serial_printf("[Fork] Failed to copy VMA [0x%x-0x%x]\n", 
                          parent_vma->vm_start, parent_vma->vm_end);
            return false;
        }
        
        vmas_copied++;
        serial_printf("[Fork] Copied VMA [0x%x-0x%x] flags=0x%x\n",
                      parent_vma->vm_start, parent_vma->vm_end, parent_vma->vm_flags);
        
        // Move scan pointer to end of this VMA
        scan_addr = parent_vma->vm_end;
    }
    
    serial_printf("[Fork] Successfully copied %d VMAs\n", vmas_copied);
    return true;
}

// Note: copy_vma_tree_simple is implemented as static function above
// and used internally by copy_mm function

int copy_mm(pcb_t *parent, pcb_t *child)
{
    if (!parent || !child || !parent->mm) {
        return -EINVAL;
    }
    
    mm_struct_t *parent_mm = parent->mm;
    
    serial_printf("[Fork] Starting memory space duplication with COW support\n");
    
    // Clone the page directory for the child process
    uintptr_t child_pgd_phys = paging_clone_directory(parent_mm->pgd_phys);
    if (!child_pgd_phys) {
        serial_printf("[Fork] Failed to clone page directory\n");
        return -ENOMEM;
    }
    
    // Create new mm_struct for child
    mm_struct_t *child_mm = create_mm((uint32_t*)child_pgd_phys);
    if (!child_mm) {
        serial_printf("[Fork] Failed to create child mm_struct\n");
        // Clean up cloned page directory
        paging_free_user_space((uint32_t*)child_pgd_phys);
        return -ENOMEM;
    }
    
    // Copy memory region boundaries from parent
    child_mm->start_code = parent_mm->start_code;
    child_mm->end_code = parent_mm->end_code;
    child_mm->start_data = parent_mm->start_data;
    child_mm->end_data = parent_mm->end_data;
    child_mm->start_brk = parent_mm->start_brk;
    child_mm->end_brk = parent_mm->end_brk;
    child_mm->start_stack = parent_mm->start_stack;
    
    // Copy all VMAs from parent to child
    uintptr_t parent_flags = spinlock_acquire_irqsave(&parent_mm->lock);
    
    // Simplified VMA copying - iterate through tree and copy each VMA
    if (parent_mm->vma_tree.root) {
        if (!copy_vma_tree_simple(child_mm, parent_mm)) {
            spinlock_release_irqrestore(&parent_mm->lock, parent_flags);
            destroy_mm(child_mm);
            serial_printf("[Fork] Failed to copy VMAs\n");
            return -ENOMEM;
        }
    }
    
    spinlock_release_irqrestore(&parent_mm->lock, parent_flags);
    
    // Assign the new mm_struct to child
    child->mm = child_mm;
    child->page_directory_phys = child_mm->pgd_phys;
    
    serial_printf("[Fork] Successfully duplicated memory space: %d VMAs\n", child_mm->map_count);
    return 0;
}

//============================================================================
// File Descriptor Management Implementation  
//============================================================================

int copy_fd_table(pcb_t *parent, pcb_t *child)
{
    if (!parent || !child) {
        return -EINVAL;
    }
    
    uintptr_t irq_flags = spinlock_acquire_irqsave(&parent->fd_table_lock);
    
    for (int fd = 0; fd < MAX_FD; fd++) {
        sys_file_t *parent_sf = parent->fd_table[fd];
        if (parent_sf) {
            // Create new sys_file_t for child
            sys_file_t *child_sf = (sys_file_t*)kmalloc(sizeof(sys_file_t));
            if (!child_sf) {
                spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
                return -ENOMEM;
            }
            
            // Copy sys_file structure
            *child_sf = *parent_sf;
            
            // Child gets its own copy of the file structure
            file_t *child_file = (file_t*)kmalloc(sizeof(file_t));
            if (!child_file) {
                kfree(child_sf);
                spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
                return -ENOMEM;
            }
            
            // Copy file structure
            *child_file = *parent_sf->vfs_file;
            spinlock_init(&child_file->lock);
            child_sf->vfs_file = child_file;
            
            // Reference same vnode (files are shared between parent and child)
            // Note: In a full implementation, we'd need proper vnode reference counting
            
            child->fd_table[fd] = child_sf;
        }
    }
    
    spinlock_release_irqrestore(&parent->fd_table_lock, irq_flags);
    return 0;
}

//============================================================================
// Argument Parsing Implementation
//============================================================================

int parse_argv(uint32_t user_argv_ptr, char ***argv_out, int *argc_out)
{
    if (user_argv_ptr == 0) {
        *argv_out = NULL;
        *argc_out = 0;
        return 0;
    }
    
    userptr_t user_argv = (userptr_t)user_argv_ptr;
    if (!access_ok(VERIFY_READ, user_argv, sizeof(char*))) {
        return -EFAULT;
    }
    
    // Count argc and validate pointers
    int argc = 0;
    for (int i = 0; i < MAX_ARGS; i++) {
        char *user_arg_ptr;
        if (copy_from_user(&user_arg_ptr, (const_userptr_t)((char**)user_argv + i), sizeof(char*)) != 0) {
            return -EFAULT;
        }
        
        if (user_arg_ptr == NULL) {
            break; // End of argv array
        }
        argc++;
    }
    
    if (argc == 0) {
        *argv_out = NULL;
        *argc_out = 0;
        return 0;
    }
    
    // Allocate kernel argv array
    char **argv = (char**)kmalloc((argc + 1) * sizeof(char*));
    if (!argv) {
        return -ENOMEM;
    }
    
    // Copy each argument string
    for (int i = 0; i < argc; i++) {
        char *user_arg_ptr;
        copy_from_user(&user_arg_ptr, (const_userptr_t)((char**)user_argv + i), sizeof(char*));
        
        // Allocate buffer for this argument
        argv[i] = (char*)kmalloc(MAX_SYSCALL_STR_LEN);
        if (!argv[i]) {
            // Clean up on failure
            for (int j = 0; j < i; j++) {
                kfree(argv[j]);
            }
            kfree(argv);
            return -ENOMEM;
        }
        
        // Copy string from user space
        if (strncpy_from_user_safe((const_userptr_t)user_arg_ptr, argv[i], MAX_SYSCALL_STR_LEN) < 0) {
            // Clean up on failure
            for (int j = 0; j <= i; j++) {
                kfree(argv[j]);
            }
            kfree(argv);
            return -EFAULT;
        }
    }
    
    argv[argc] = NULL; // Null-terminate array
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

void free_argv(char **argv, int argc)
{
    if (!argv) return;
    
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            kfree(argv[i]);
        }
    }
    kfree(argv);
}

//============================================================================
// Process Lookup Implementation
//============================================================================

pcb_t *process_get_by_pid(uint32_t pid)
{
    // Simplified implementation - just return current process for now
    // In a real implementation, this would search through a process table
    if (pid == 0) return NULL;
    
    pcb_t *current = get_current_process();
    if (current && current->pid == pid) {
        return current;
    }
    
    // For now, always return NULL if not current process
    return NULL;
}