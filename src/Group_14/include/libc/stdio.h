#pragma once

// Use quotes and relative path for local libc headers
#include "libc/stddef.h" // For size_t
#include "libc/stdbool.h" // Include if needed by print

int putchar(int ic);
bool print(const char* data, size_t length);
int printf(const char* __restrict__ format, ...);
int snprintf(char *str, size_t size, const char *format, ...);