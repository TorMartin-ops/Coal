/**
 * @file dma.h
 * @brief Memory a device reads and writes by physical address, and device registers (ioremap() is in paging.h).
 *
 * Everything the kernel allocates (kmalloc, frames) lives in the direct map, so a kernel
 * address converts to a bus address with dma_phys(). Drivers go through these helpers only,
 * which keeps the address-translation rules in one place.
 */
#ifndef KERNEL_MEMORY_DMA_H
#define KERNEL_MEMORY_DMA_H

#include <kernel/core/types.h>
#include <kernel/memory/paging.h>

/** Zeroed, physically contiguous, page-aligned memory for rings and descriptors. NULL on failure. */
void *dma_alloc_coherent(size_t size, uintptr_t *phys);
void  dma_free_coherent(void *virt);

/** Bus address of a direct-mapped kernel address (kmalloc memory, frames, dma_alloc_coherent). */
static inline uintptr_t dma_phys(const void *virt) { return virt_to_phys(virt); }

/** True when [p, p+len) is direct mapped, so dma_phys() is valid and contiguous across it. */
bool dma_reachable(const void *p, size_t len);

static inline uint8_t  mmio_read8(const volatile void *a)  { return *(const volatile uint8_t *)a; }
static inline uint16_t mmio_read16(const volatile void *a) { return *(const volatile uint16_t *)a; }
static inline uint32_t mmio_read32(const volatile void *a) { return *(const volatile uint32_t *)a; }
static inline void mmio_write8(volatile void *a, uint8_t v)   { *(volatile uint8_t *)a = v; }
static inline void mmio_write16(volatile void *a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void mmio_write32(volatile void *a, uint32_t v) { *(volatile uint32_t *)a = v; }

/** A 64-bit device register is written as two 32-bit halves, low first. */
static inline void mmio_write64(volatile void *a, uint64_t v) {
    mmio_write32(a, (uint32_t)v);
    mmio_write32((volatile uint8_t *)a + 4, (uint32_t)(v >> 32));
}

#endif
