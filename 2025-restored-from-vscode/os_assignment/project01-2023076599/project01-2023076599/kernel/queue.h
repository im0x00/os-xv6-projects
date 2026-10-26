#ifndef XV6_QUEUE_H
#define XV6_QUEUE_H

#include "param.h"
#include "proc.h"

// -- 원형 큐 타입만 --
struct PQueue {
  struct proc *buf[NPROC];
  int hd, tl;
};

/*  ↓↓↓  전역 배열 두 줄 **완전히 삭제**  ↓↓↓
static struct PQueue mlqueues[3];
static struct PQueue fcfs_q;
*/

// inline helpers
static inline void qinit(struct PQueue *q){
  q->hd = q->tl = 0;
}
static inline int  qempty(struct PQueue *q){ return q->hd == q->tl; }
static inline void pushq(struct PQueue *q, struct proc *p){

  q->buf[q->tl] = p; q->tl = (q->tl+1) % NPROC;

}
static inline struct proc *popq(struct PQueue *q){
  struct proc *p;

  if(qempty(q)) return 0;
  p = q->buf[q->hd];
  q->hd = (q->hd+1) % NPROC;

  return p;
}

#endif