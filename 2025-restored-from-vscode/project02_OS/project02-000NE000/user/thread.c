#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/thread.h"

#ifndef PGSIZE
#define PGSIZE 4096

static void *
alloc_stack(void){
  void *raw = malloc(2*PGSIZE);
  if(!raw) return 0;
  uint64 aligned = ((uint64)raw + PGSIZE-1) & ~(PGSIZE-1);
  return (void*)aligned;
}

int
thread_create(void (*start)(void*,void*), void *a1, void *a2)
{
  void *stack = alloc_stack();
  if(!stack) return -1;
  // cast each argument to uint64
  int tid = clone((uint64)start, (uint64)a1, (uint64)a2, (uint64)stack);
  if(tid < 0)
    free((void*)((uint64)stack & ~(PGSIZE-1)));
  return tid;
}

int
thread_join(void)
{
  void *stack;
  // join takes a void**, so pass &stack
  int tid = join(&stack);
  if(tid >= 0)
    free(stack);
  return tid;
}
#endif
