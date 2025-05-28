#ifndef MSR_H
#define MSR_H

// Include necessary types (adjust path if your types.h is elsewhere)
#include <kernel/core/types.h> // Or use <stdint.h> if available and preferred

// --- Common MSR Definitions ---
#define MSR_EFER 0xC0000080 // Extended Feature Enable Register (for NXE, SCE, etc.)
// Add other MSRs if needed, e.g.:
// #define MSR_FS_BASE 0xC0000100
// #define MSR_GS_BASE 0xC0000101
// #define MSR_KERNEL_GS_BASE 0xC0000102 // For swapgs
// #define MSR_IA32_APIC_BASE 0x1B
// #define MSR_IA32_PAT 0x277

/**
 * @brief Reads a Model Specific Register (MSR).
 * Executes the RDMSR instruction.
 *
 * @param msr_id The 32-bit identifier of the MSR to read.
 * This value is loaded into the ECX register.
 * @return The 64-bit value read from the specified MSR.
 * The result is returned from the EDX:EAX registers.
 */
uint64_t rdmsr(uint32_t msr_id);

/**
 * @brief Writes a value to a Model Specific Register (MSR).
 * Executes the WRMSR instruction.
 *
 * @param msr_id The 32-bit identifier of the MSR to write.
 * This value is loaded into the ECX register.
 * @param value The 64-bit value to write to the specified MSR.
 * The low 32 bits are loaded into EAX, the high 32 bits into EDX.
 */
void wrmsr(uint32_t msr_id, uint64_t value);

#endif // MSR_H