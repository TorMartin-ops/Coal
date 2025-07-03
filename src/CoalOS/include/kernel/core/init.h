/**
 * @file init.h
 * @brief Kernel Initialization Management
 * @author Refactored for SOLID principles
 * @version 1.0
 * 
 * @details Provides modular initialization functions that follow the Single
 * Responsibility Principle. Each initialization phase has a focused purpose
 * and clear error handling.
 */

#ifndef KERNEL_CORE_INIT_H
#define KERNEL_CORE_INIT_H

//============================================================================
// Includes
//============================================================================
#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <kernel/core/constants.h>
#include <libc/stdint.h>
#include <libc/stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//============================================================================
// Initialization Phase Results
//============================================================================

typedef struct init_result {
    error_t error_code;
    const char *phase_name;
    const char *error_message;
} init_result_t;

//============================================================================
// Multiboot Validation
//============================================================================

/**
 * @brief Validate multiboot environment and parameters
 * @param magic Multiboot magic number from bootloader
 * @param mb_info_phys_addr Physical address of multiboot info structure
 * @return init_result_t with validation status
 */
init_result_t init_validate_multiboot(uint32_t magic, uint32_t mb_info_phys_addr);

//============================================================================
// Core System Initialization
//============================================================================

/**
 * @brief Initialize basic I/O systems (serial, terminal)
 * @return init_result_t with initialization status
 */
init_result_t init_basic_io(void);

/**
 * @brief Initialize core CPU and memory systems
 * @param mb_info_phys_addr Physical address of multiboot info
 * @return init_result_t with initialization status
 */
init_result_t init_core_systems(uint32_t mb_info_phys_addr);

/**
 * @brief Initialize interrupt handling and timing
 * @return init_result_t with initialization status
 */
init_result_t init_interrupt_systems(void);

/**
 * @brief Initialize input devices and keyboard mapping
 * @return init_result_t with initialization status
 */
init_result_t init_input_systems(void);

//============================================================================
// Filesystem and Process Initialization
//============================================================================

/**
 * @brief Initialize filesystem and mount root
 * @return init_result_t with initialization status
 */
init_result_t init_filesystem(void);

/**
 * @brief Launch initial user processes
 * @param filesystem_ready Whether filesystem initialization succeeded
 * @return init_result_t with launch status
 */
init_result_t init_launch_processes(bool filesystem_ready);

//============================================================================
// Hardware Configuration
//============================================================================

/**
 * @brief Configure keyboard controller for proper interrupt operation
 * @return init_result_t with configuration status
 */
init_result_t init_configure_keyboard_controller(void);

/**
 * @brief Perform final system configuration before enabling interrupts
 * @return init_result_t with configuration status
 */
init_result_t init_finalize_configuration(void);

//============================================================================
// Main Initialization Orchestration
//============================================================================

/**
 * @brief Execute complete kernel initialization sequence
 * @param magic Multiboot magic number
 * @param mb_info_phys_addr Physical address of multiboot info
 * @return Does not return on success, panics on failure
 */
void kernel_main_init(uint32_t magic, uint32_t mb_info_phys_addr);

//============================================================================
// Utility Functions
//============================================================================

/**
 * @brief Print initialization result and handle errors
 * @param result Result of an initialization phase
 * @param continue_on_error Whether to continue if this phase fails
 * @return true if should continue, false if should halt
 */
bool init_handle_result(const init_result_t *result, bool continue_on_error);

/**
 * @brief Create a successful initialization result
 * @param phase_name Name of the initialization phase
 * @return init_result_t indicating success
 */
init_result_t init_success(const char *phase_name);

/**
 * @brief Create a failed initialization result
 * @param phase_name Name of the initialization phase
 * @param error_code Error code that occurred
 * @param error_message Descriptive error message
 * @return init_result_t indicating failure
 */
init_result_t init_failure(const char *phase_name, error_t error_code, const char *error_message);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_CORE_INIT_H