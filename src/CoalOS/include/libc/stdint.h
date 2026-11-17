#pragma once

/* LP64 (x86_64): int is 32 bits, long and pointers are 64 bits. */
typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef short              int16_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long      uint64_t;
typedef long               int64_t;

typedef unsigned long      uintptr_t;
typedef long               intptr_t;

#define UINT32_MAX  (0xFFFFFFFFU)
#define INT32_MAX   (2147483647)
#define INT32_MIN   (-2147483647 - 1)
#define UINT64_MAX  (0xFFFFFFFFFFFFFFFFUL)
#define INT64_MAX   (0x7FFFFFFFFFFFFFFFL)
#define SIZE_MAX    UINT64_MAX
#define UINTPTR_MAX UINT64_MAX
