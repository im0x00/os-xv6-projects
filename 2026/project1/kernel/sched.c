#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "rbtree.h"
#include "defs.h"

#define LOAD_DIFF_THRESHOLD 1

extern struct proc proc[NPROC];
extern struct cpu cpus[NCPU];

static const uint64 nice_to_vruntime_step[] = {
  1,   // nice = -3
  2,   // nice = -2
  4,   // nice = -1
  8,   // nice = 0
  16,  // nice = 1
  32   // nice = 2
};

static int
nice_to_index(int nice)
{
  if(nice < -3)
    nice = -3;
  if(nice > 2)
    nice = 2;
  return nice + 3;
}

uint64 // must need mutual exclusion
calculate_vruntime_delta(struct proc *p) 
{
  uint64 runtime = r_time() - p->last_scheduled_time;
  return nice_to_vruntime_step[nice_to_index(p->nice_value)] * runtime;
}

void 
shift_scale_vruntime(struct run_queue *src_rq, struct run_queue *dst_rq, struct proc *p)
{
  uint64 delta = 0;

  if(p->vruntime > src_rq->min_vruntime)
    delta = p->vruntime - src_rq->min_vruntime;

  p->vruntime = dst_rq->min_vruntime + delta;
}



struct cpu*
select_target_cpu(struct proc *p)
{   
  // 1순위: last cpu 
  // 2순위: idle cpu 
  // 3순위: least-loaded cpu
  struct cpu *c;
  struct cpu *last = 0;
  struct cpu *idle = 0;
  struct cpu *least = 0;

  int last_load = 0;
  int least_load = 0x7fffffff;

  for(c = cpus; c < &cpus[NCPU]; c++){
    if(!c->online)
      continue;

    if(c == p->home_cpu){
      last = c;
      last_load = c->rq->nr_running;
      continue;
    }

    acquire(&c->rq->lock);
    int load = c->rq->nr_running;
    int is_idle = (c->proc == 0 && load == 0);

    if(is_idle && idle == 0)
      idle = c;

    if(least == 0 || load < least_load){
      least = c;
      least_load = load;
    }

    release(&c->rq->lock);
  }

  if(last){
    if(least == 0)
      return last;

    if(last_load - least_load <= LOAD_DIFF_THRESHOLD)
      return last;
  }

  if(idle)
    return idle;

  if(least)
    return least;

  return mycpu();
}


int
find_minimum_running_vruntime(uint64 *out)
{
  struct cpu *c;
  uint64 min = 0;
  int found = 0;

  for(c = cpus; c < &cpus[NCPU]; c++) {
    if(!c->online)
      continue;
    
    
    if (c == mycpu()) {
      min = c->rq->min_vruntime;
      continue;
    }


    if(c->proc != 0) {
      struct proc *p = c->proc;
      uint64 cur = p->vruntime + calculate_vruntime_delta(p);

      if(!found || cur < min)
        min = cur;
      found = 1;
    }
  }

  if(found){
    *out = min;
    return 1;
  }
  return 0;
}

int
find_global_minimum_vruntime(struct run_queue *held_rq, uint64 *out)
{
  struct cpu *c;
  uint64 min = 0;
  int found = 0;

  for(c = cpus; c < &cpus[NCPU]; c++) {
    struct run_queue *rq = c->rq;
    uint64 tmp;

    if(!c->online)
      continue;

    if(rq != held_rq)
      acquire(&rq->lock);

    if(rq->nr_running > 0) {
      tmp = rq->min_vruntime;
      if(!found || tmp < min)
        min = tmp;
      found = 1;
    }

    if(rq != held_rq)
      release(&rq->lock);
  }

  if(found) {
    *out = min;
    return 1;
  }
  return 0;
}

uint64
compute_enqueue_vruntime(struct run_queue *nrq, struct proc *p, enum procstate from) 
{
  uint64 vruntime; 
  uint64 min;
  
  switch (from) {
    case USED: {
      struct proc *pp = p->parent;
      if (pp) {
        vruntime = pp->vruntime;
      } else {
        vruntime = 0; // init
      }
      break;
    }
    case SLEEPING: {
      if (find_global_minimum_vruntime(nrq, &min)) {// 1, RUNNABLE 프로세스가 존재하는 경우 
        vruntime = min;
      } else if (find_minimum_running_vruntime(&min)) { // 2. RUNNABLE 프로세스는 없지만, RUNNING 중인 프로세스가 하나 이상 존재하는 경우
        vruntime = min;
      } else // 3. RUNNABLE도 RUNNING도 존재하지 않는 경우 
          vruntime = 0;
      break;
    }
    default: {
      struct run_queue *rq = mycpu()->rq;
      if (rq != nrq) {
        acquire(&rq->lock);
        shift_scale_vruntime(mycpu()->rq, nrq, p);
        release(&rq->lock);
      }
      vruntime = p->vruntime + calculate_vruntime_delta(p);
      break;
    }
  }
  return vruntime;
}


void
enqueue_on_rq(struct run_queue *rq, struct proc *p, enum procstate from)
{
  if(p->queued)
    panic("double enqueue");

  p->vruntime = compute_enqueue_vruntime(rq, p, from);
  p->run_node.key = p->vruntime;
  p->run_node.data = p;

  rb_insert(rq->tree, &p->run_node);
  rq->leftmost = rb_first(rq->tree->root);
  if(rq->leftmost)
    rq->min_vruntime = rq->leftmost->key;
  else
    rq->min_vruntime = p->vruntime;
  rq->nr_running++;

  p->state = RUNNABLE;
  p->queued = 1;
  p->on_rq = rq;
}

void
dequeue_from_rq(struct run_queue *rq, struct proc *p)
{
  if(!p->queued || p->on_rq != rq)
    panic("bad dequeue");

  rb_delete(rq->tree, &p->run_node);
  rq->nr_running--;
  rq->leftmost = rb_first(rq->tree->root);
  if(rq->leftmost)
    rq->min_vruntime = rq->leftmost->key;
  else
    rq->min_vruntime = p->vruntime;

  p->queued = 0;
  p->on_rq = 0;
}
