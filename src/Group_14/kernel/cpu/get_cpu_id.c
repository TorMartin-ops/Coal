#include <kernel/cpu/get_cpu_id.h>



/**
 * get_cpu_id - Retrieves the current CPU's identifier.
 *
 * This implementation uses the CPUID instruction with EAX=1 to extract the Local APIC ID.
 * The APIC ID is located in EBX bits 24–31. For a uniprocessor system or when CPUID is unavailable,
 * you could fallback to 0.
 *
 * @return The CPU identifier.
 */
int get_cpu_id(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Execute CPUID with EAX=1.
    __asm__ volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    
    // Extract Local APIC ID from EBX (bits 24-31).
    int cpu_id = (int)((ebx >> 24) & 0xff);
    return cpu_id;
}
