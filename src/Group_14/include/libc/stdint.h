#pragma once


typedef long unsigned int size_t;
typedef long unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef long int int32_t;
typedef short int16_t;
typedef signed char int8_t;

// Added definition for uint64_t
typedef unsigned long long uint64_t;

#define UINT32_MAX (0xFFFFFFFFU)
#define INT32_MAX  (2147483647L)
#define INT32_MIN  (-2147483647L - 1L) // Also adding MIN for completeness

// Added definition for UINT64_MAX
#define UINT64_MAX (0xFFFFFFFFFFFFFFFFULL)


#define SIZE_MAX   UINT32_MAX // Note: SIZE_MAX might need to be UINT64_MAX on a 64-bit target, but is likely correct as UINT32_MAX for i386

typedef uint32_t uintptr_t;
#define UINTPTR_MAX UINT32_MAX // Note: uintptr_t is often 64-bit on 64-bit targets