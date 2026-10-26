// user/thread.c
#include "kernel/types.h"
#include "user/user.h"
#include "thread.h"

#define STACK_SIZE 4096
#define PTR_SIZE   sizeof(void*)

/* 내부용: 원본 malloc 포인터를 스택 바로 앞에 숨겨 둔다 */
static inline void *
alloc_aligned_stack(void)
{
  void *raw = malloc(STACK_SIZE + STACK_SIZE - 1 + PTR_SIZE);
  if(raw == 0)
    return 0;

  uint64 aligned = ((uint64)raw + PTR_SIZE + STACK_SIZE - 1) & ~(STACK_SIZE - 1);
  /* aligned – PTR_SIZE 위치에 raw 저장 */
  *((void **)(aligned - PTR_SIZE)) = raw;
  return (void *)aligned;
}

static inline void
free_aligned_stack(void *aligned)
{
  void *raw = *((void **)((uint64)aligned - PTR_SIZE));
  free(raw);
}

/* ---------- 기존 API ---------- */

int
thread_create(void (*start)(void*,void*), void *arg1, void *arg2)
{
  void *stack = alloc_aligned_stack();
  if(stack == 0)
    return -1;

  int tid = clone(start, arg1, arg2, stack);
  if(tid < 0)
    free_aligned_stack(stack);

  return tid;
}

int
thread_join(void)
{
  void *stack = 0;
  int tid = join(&stack);
  if(tid > 0 && stack)
    free_aligned_stack(stack);
  return tid;
}