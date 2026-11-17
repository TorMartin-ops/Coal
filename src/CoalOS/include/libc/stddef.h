#pragma once

typedef unsigned int size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif
