// user/thread.h
#pragma once

#include "kernel/types.h"
#include "user/user.h"

// syscall stubs (from usys.S)
int clone(void (*start)(void*,void*), void *a1, void *a2, void *stack);
int join(void **stack);

// thread‐library API
int thread_create(void (*start)(void*,void*), void *a1, void *a2);
int thread_join(void);   // no args, as the tests expect