/**
 * @file heapdebug.h
 * @brief Heap hardening of the debug kernel: red zones, poisoning, double-free detection, quarantine.
 *
 * Every kmalloc block carries a trailer: a 16-byte red zone behind the object, the requested size, the allocation
 * state, the callers of the allocation and of the last free, and a second red zone that guards the front of the
 * next object. The unused tail of the object is filled with a pattern. A freed object is filled with 0x6b and parked in a
 * quarantine; its fill is checked when it leaves the quarantine and again when it is handed out. A fresh
 * object is filled with 0xa5 so that reads of uninitialised memory stand out.
 */
#ifndef KERNEL_MEMORY_HEAPDEBUG_H
#define KERNEL_MEMORY_HEAPDEBUG_H

#include <kernel/core/types.h>

#ifdef COAL_DEBUG_KERNEL

#define HD_RZ_LEN       16
#define HD_RZ_BYTE      0xcc
#define HD_SLACK_BYTE   0xbc
#define HD_POISON_FREE  0x6b
#define HD_POISON_ALLOC 0xa5
#define HD_BT           3

enum { HD_FRESH = 0x46524553, HD_ALLOC = 0xa110c8ed, HD_FREE = 0xf4eef4ee };

struct hd_trailer {
    uint8_t rz[HD_RZ_LEN];
    void *next;                         /* slab free-list link, kept out of the object */
    uint32_t req, state;
    uintptr_t alloc_bt[HD_BT], free_bt[HD_BT];
    uint8_t rz_next[HD_RZ_LEN];         /* the red zone in front of the next object */
};

/** Mark obj (cap bytes, req requested) allocated: fill, record callers. */
void hd_on_alloc(void *obj, size_t cap, size_t req, struct hd_trailer *t, const uintptr_t *bt, unsigned n);

/** Validate a free (state, red zones before and after, tail pattern), then poison. false: do not free. */
bool hd_on_free(void *obj, size_t cap, struct hd_trailer *t, const uint8_t *front_rz, const uintptr_t *bt, unsigned n);

/** Report if a freed object lost its poison. */
void hd_check_poison(void *obj, size_t cap, struct hd_trailer *t, const char *where);

/** Report a free of a pointer that is no allocation. */
void hd_bad_free(void *p, const char *why);

/** Park a freed slab object; returns the oldest one when the quarantine is full, else NULL. */
void *hd_quarantine(void *obj);

#endif
#endif
