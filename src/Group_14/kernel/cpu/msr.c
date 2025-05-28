#include <kernel/cpu/msr.h>

/**
 * @brief Reads a Model Specific Register (MSR).
 * Executes the RDMSR instruction.
 */
uint64_t rdmsr(uint32_t msr_id) {
    uint32_t low, high;
    // The 'volatile' keyword prevents the compiler from optimizing the assembly away
    // 'asm' is used for inline assembly
    asm volatile (
        "rdmsr"         // The assembly instruction itself
        : "=a" (low),   // Output operand: low 32 bits go into 'low' (binds to EAX)
          "=d" (high)  // Output operand: high 32 bits go into 'high' (binds to EDX)
        : "c" (msr_id)  // Input operand: 'msr_id' goes into ECX register
        // No clobber list needed here as the outputs cover EAX/EDX and input covers ECX
    );
    // Combine the 32-bit low and high parts into a 64-bit value
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Writes a value to a Model Specific Register (MSR).
 * Executes the WRMSR instruction.
 */
void wrmsr(uint32_t msr_id, uint64_t value) {
    // Split the 64-bit value into low and high 32-bit parts
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile (
        "wrmsr"         // The assembly instruction itself
        : // No output operands
        : "c" (msr_id), // Input operand: 'msr_id' goes into ECX
          "a" (low),    // Input operand: 'low' goes into EAX
          "d" (high)   // Input operand: 'high' goes into EDX
        // Clobber list: "memory" is often used for instructions like wrmsr
        // that can have side effects beyond the specified registers. It tells
        // the compiler that memory might have changed in ways it doesn't know.
        : "memory"
    );
}