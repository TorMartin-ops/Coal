#pragma once
#ifndef GET_CPU_ID_H
#define GET_CPU_ID_H

#include <kernel/core/types.h> 
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retrieves the current CPU's identifier using the CPUID instruction.
 *
 * This function uses CPUID with EAX=1 to obtain the Local APIC ID from EBX (bits 24–31).
 * In an SMP environment, this value may be remapped if necessary.
 *
 * @return The CPU's identifier (0 if CPUID is not supported or in uniprocessor systems).
 */
int get_cpu_id(void);

#ifdef __cplusplus
}
#endif

#endif // GET_CPU_ID_H
