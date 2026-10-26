#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

#include "queue.h"

extern struct queue fcfs_queue;
extern struct queue mlfq[NMLFQ_LEVELS];

static const char *sched_mode_name[] = { // 문자열화용(디버깅·메시지용)
  [SCHED_FCFS] = "FCFS",
  [SCHED_MLFQ] = "MLFQ",
};

uint64
sys_mlfqmode(void)
{
  struct cpu *c = mycpu();
  int old = c->sched_mode;
  c->sched_mode = SCHED_MLFQ;

  if (old == c->sched_mode) {
    printf("Warning: already in %s mode\n", sched_mode_name[c->sched_mode]);
    return -1;
  }


  // FCFS 큐에 있던 모든 RUNNABLE 프로세스를 level=0 으로 이동
  struct proc *p;
  while ((p = dequeue(&fcfs_queue)) != NULL) {
    acquire(&p->lock);
    if (p->state == RUNNABLE) {
      p->level     = 0;
      p->time_used = 0;
      p->priority  = DEFAULT_PRIORITY;
      enqueue(&mlfq[p->level], p);
    }
    release(&p->lock);
  }

  return 0;
}

uint64
sys_fcfsmode(void)
{
  struct cpu *c = mycpu();
  int old = c->sched_mode;
  c->sched_mode = SCHED_FCFS;

  if (old == c->sched_mode) {
    printf("Warning: already in %s mode\n", sched_mode_name[c->sched_mode]);
    return -1;
  }

  // MLFQ 모든 레벨에서 RUNNABLE 프로세스를 가져와 배열에 저장
  struct proc *arr[NPROC];
  int cnt = 0;
  for (int lvl = 0; lvl < 3; lvl++) {
    struct proc *p;
    while ((p = dequeue(&mlfq[lvl])) != NULL) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // 필드 초기화
        p->level     = -1;
        p->time_used = -1;
        p->priority  = -1;
        arr[cnt++] = p;
      }
      release(&p->lock);
    }
  }

  // creation_time 순으로 정렬 (selection sort)
  for (int i = 0; i < cnt - 1; i++) {
    int min = i;
    for (int j = i + 1; j < cnt; j++) {
      if (arr[j]->ctime < arr[min]->ctime)
        min = j;
    }
    if (min != i) {
      struct proc *tmp = arr[i];
      arr[i] = arr[min];
      arr[min] = tmp;
    }
  }

  // FCFS 큐로 재enqueue
  for (int i = 0; i < cnt; i++) {
    enqueue(&fcfs_queue, arr[i]);
  }

  return old;  // 호출자에 이전 모드 반환
}

// 1) yield: give up CPU
uint64
sys_yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  // 현재 스케줄 모드에 따라 적절한 큐에 다시 넣기
  struct cpu *c = mycpu();
  if(c->sched_mode == SCHED_FCFS){
    enqueue(&fcfs_queue, p);
  } else {
    enqueue(&mlfq[p->level], p);
  }
  release(&p->lock);
  // scheduler로 즉시 복귀
  yield();
  return 0;
}

// 2) getlev: return queue level (0..2) or 99 if FCFS
uint64
sys_getlev(void)
{
  struct cpu *c = mycpu();
  if(c->sched_mode == SCHED_FCFS)
    return 99;
  struct proc *p = myproc();
  return p->level;
}


// 3) setpriority: set priority for any pid
uint64
sys_setpriority(void)
{
    int pid, prio;

    // 1) 인자 파싱
    argint(0, &pid);
    argint(1, &prio);

    // 2) prio range check
    if (prio < 0 || prio > 3)
        return -2;              // invalid priority

    // 3) 해당 pid 프로세스 검색
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (p->pid == pid) {
            // 4) 우선순위 설정
            p->priority = prio;
            // 5) RUNNABLE 상태면 큐 재정렬 (optional)
            if (p->state == RUNNABLE) {
                remove_proc(&mlfq[p->level], p);
                enqueue(&mlfq[p->level], p);
            }
            release(&p->lock);
            return 0;            // success
        }
        release(&p->lock);
    }

    return -1;                  // PID not found
}


uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
