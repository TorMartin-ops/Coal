/**
 * @file test_suite.h
 * @brief Comprehensive test suite for CoalOS kernel validation
 * @author Test Framework for architectural improvements
 */

#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include <libc/stdint.h>
#include <libc/stdbool.h>
#include <libc/stddef.h>

// Test result structure
typedef struct {
    const char *test_name;
    bool passed;
    const char *failure_reason;
    uint32_t execution_time_ms;
} test_result_t;

// Test function signature
typedef bool (*test_func_t)(test_result_t *result);

// Test categories
typedef enum {
    TEST_CATEGORY_MEMORY,
    TEST_CATEGORY_PROCESS,
    TEST_CATEGORY_SYSCALL,
    TEST_CATEGORY_FS,
    TEST_CATEGORY_SYNC,
    TEST_CATEGORY_SCHEDULER,
    TEST_CATEGORY_COUNT
} test_category_t;

// Test suite structure
typedef struct {
    const char *name;
    test_func_t func;
    test_category_t category;
    bool critical;  // If true, failure stops further testing
} test_case_t;

// Test framework functions
void test_init(void);
void test_run_all(void);
void test_run_category(test_category_t category);
void test_print_results(void);

// Memory management tests
bool test_memory_buddy_allocator(test_result_t *result);
bool test_memory_kmalloc_basic(test_result_t *result);
bool test_memory_kmalloc_stress(test_result_t *result);
bool test_memory_page_mapping(test_result_t *result);
bool test_memory_page_fault_handler(test_result_t *result);
bool test_memory_vma_operations(test_result_t *result);
bool test_memory_frame_allocator(test_result_t *result);
bool test_memory_slab_allocator(test_result_t *result);

// Process management tests
bool test_process_creation(test_result_t *result);
bool test_process_fork(test_result_t *result);
bool test_process_exec(test_result_t *result);
bool test_process_exit(test_result_t *result);
bool test_process_wait(test_result_t *result);
bool test_process_signals(test_result_t *result);
bool test_process_groups(test_result_t *result);
bool test_process_sessions(test_result_t *result);

// Scheduler tests
bool test_scheduler_basic(test_result_t *result);
bool test_scheduler_priority(test_result_t *result);
bool test_scheduler_fairness(test_result_t *result);
bool test_scheduler_context_switch(test_result_t *result);
bool test_scheduler_sleep_wakeup(test_result_t *result);
bool test_scheduler_load_balancing(test_result_t *result);

// System call tests
bool test_syscall_basic_io(test_result_t *result);
bool test_syscall_file_operations(test_result_t *result);
bool test_syscall_process_operations(test_result_t *result);
bool test_syscall_error_handling(test_result_t *result);
bool test_syscall_user_validation(test_result_t *result);

// File system tests
bool test_fs_vfs_mount(test_result_t *result);
bool test_fs_file_operations(test_result_t *result);
bool test_fs_directory_operations(test_result_t *result);
bool test_fs_path_resolution(test_result_t *result);
bool test_fs_concurrent_access(test_result_t *result);

// Synchronization tests
bool test_sync_spinlock_basic(test_result_t *result);
bool test_sync_spinlock_contention(test_result_t *result);
bool test_sync_irq_safety(test_result_t *result);
bool test_sync_deadlock_prevention(test_result_t *result);

// Performance benchmark tests
bool test_perf_context_switch_latency(test_result_t *result);
bool test_perf_syscall_overhead(test_result_t *result);
bool test_perf_memory_allocation(test_result_t *result);
bool test_perf_file_io_throughput(test_result_t *result);

// Stress tests
bool test_stress_memory_exhaustion(test_result_t *result);
bool test_stress_process_creation(test_result_t *result);
bool test_stress_file_operations(test_result_t *result);
bool test_stress_scheduler_load(test_result_t *result);

// Utility functions for tests
void test_assert(bool condition, const char *msg, test_result_t *result);
void test_assert_equals(uint32_t expected, uint32_t actual, const char *msg, test_result_t *result);
void test_assert_not_null(void *ptr, const char *msg, test_result_t *result);
uint32_t test_get_timestamp_ms(void);

#endif /* TEST_SUITE_H */