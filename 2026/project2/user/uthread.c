// TODO
// User-space helpers for kernel threads.
//
// thread_create() and thread_join() are thin wrappers over the clone()
// and join() system calls.  They mirror the spirit of pthread_create()
// and pthread_join():

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "user/uthread.h"

int
thread_create(void (*fn)(void *), void *arg, int n_pages)
{
  void *st = malloc(n_pages * PGSIZE);
  int tid = clone(fn, arg, st, n_pages, CLONE_VM);
  if (tid < 0) {
    free(st);
    return -1;
  }

  return tid;
}

int
thread_join(void)
{
  void *stack;
  int tid = join(&stack);

  if (tid < 0) return -1;

  free(stack);
  return tid;
}
