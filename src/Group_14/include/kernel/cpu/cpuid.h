#ifndef CPUID_H
#define CPUID_H

#include <kernel/core/types.h> // Include types for uint32_t etc.

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Executes the CPUID instruction.
 *
 * @param code The value to load into EAX before calling CPUID.
 * @param eax Output pointer for EAX register result.
 * @param ebx Output pointer for EBX register result.
 * @param ecx Output pointer for ECX register result.
 * @param edx Output pointer for EDX register result.
 */
static inline void cpuid(uint32_t code, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    asm volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx) // Outputs
        : "a"(code)                                     // Input (EAX)
        : "memory"                                      // Clobbers
    );
}

#ifdef __cplusplus
}
#endif

#endif // CPUID_H