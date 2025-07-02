/**
 * @file msr.c
 * @brief Model Specific Register (MSR) access implementation
 */

#include <kernel/cpu/msr.h>

/**
 * @brief Reads a Model Specific Register (MSR)
 */
uint64_t rdmsr(uint32_t msr_id) {
    uint32_t low, high;
    
    asm volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr_id)
    );
    
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Writes a value to a Model Specific Register (MSR)
 */
void wrmsr(uint32_t msr_id, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    
    asm volatile (
        "wrmsr"
        :
        : "c"(msr_id), "a"(low), "d"(high)
    );
}