#include "queue.h"
#include "defs.h"      // for panic()

// 실제 global queue 정의
struct queue fcfs_queue;
struct queue mlfq[NMLFQ_LEVELS];

void
enqueue(struct queue *q, struct proc *p) 
{
  p->next = NULL;
  if (q->tail) {
    q->tail->next = p;
    q->tail = p;
  } else {
    q->head = q->tail = p;
  }
}

struct proc *
dequeue(struct queue *q) 
{
  struct proc *p = q->head;
  if (!p)
    return NULL;
  q->head = p->next;
  if (!q->head)
    q->tail = NULL;
  return p;
}

void
queues_init(void) 
{
  queue_init(&fcfs_queue);
  for (int i = 0; i < NMLFQ_LEVELS; i++)
    queue_init(&mlfq[i]);
}

void
remove_proc(struct queue *q, struct proc *p)
{
    struct proc *cur = q->head;
    struct proc *prev = NULL;

    while (cur) {
        if (cur == p) {
            // unlink cur
            if (prev)
                prev->next = cur->next;
            else
                q->head = cur->next;
            // update tail if needed
            if (q->tail == cur)
                q->tail = prev;
            // clear p->next to avoid dangling pointer
            p->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    // p not found: no-op
}