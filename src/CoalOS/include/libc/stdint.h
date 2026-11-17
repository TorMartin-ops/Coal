#pragma once

/* ILP32 (i386): int and long are both 32 bits, but gcc's builtin
 * prototypes and format checking expect the int-based spellings. */
typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef short              int16_t;
typedef unsigned int       uint32_t;
typedef int                int32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;

typedef unsigned int       uintptr_t;
typedef int                intptr_t;

#define UINT32_MAX (0xFFFFFFFFU)
#define INT32_MAX  (2147483647)
#define INT32_MIN  (-2147483647 - 1)
#define UINT64_MAX (0xFFFFFFFFFFFFFFFFULL)
#define SIZE_MAX   UINT32_MAX
#define UINTPTR_MAX UINT32_MAX
