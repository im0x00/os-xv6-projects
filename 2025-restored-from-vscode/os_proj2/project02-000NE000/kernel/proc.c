#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock mem_lock;
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

static struct proc* allocproc(void);

static inline struct proc*
task_leader(struct proc *p)
{
  return p->tg_leader ? p->tg_leader : p;
}
// kernel/proc.c

// kernel/proc.c

// In kernel/proc.c, after your helper task_leader():

// Only tear down a kernel‐thread TCB, not the shared pagetable.
// kernel/proc.c

// freethread: only tear down a kernel‐thread TCB, not the shared pagetable.
static void
freethread(struct proc *t)
{
  // 1) unmap per‐thread trapframe VA from the shared pagetable
  if(t->trapframe_va){
    uvmunmap(t->pagetable, t->trapframe_va, 1, 0);
    t->trapframe_va = 0;
  }

  // 2) free the trapframe physical page
  if(t->trapframe){
    kfree((void*)t->trapframe);
    t->trapframe = 0;
  }

  // 3) close and drop duplicated file descriptors
  for(int fd = 0; fd < NOFILE; fd++){
    if(t->ofile[fd]){
      fileclose(t->ofile[fd]);
      t->ofile[fd] = 0;
    }
  }

  // 4) drop cwd reference
  if(t->cwd){
    iput(t->cwd);
    t->cwd = 0;
  }

  // 5) reset TCB fields
  t->isthread  = 0;
  t->ustack    = 0;
  t->tg_leader = 0;
  t->parent    = 0;
  t->name[0]   = 0;
  t->chan      = 0;
  t->killed    = 0;
  t->xstate    = 0;
  t->state     = UNUSED;
}

// kernel/proc.c
int
clone(uint64 fcn, uint64 arg1, uint64 arg2, uint64 stack)
{
  /* 0. 사용자 스택 체크 */
  if(stack == 0 || stack % PGSIZE)
    return -1;

  struct proc *p  = myproc();
  struct proc *np = allocproc();     // np->lock held
  if(np == 0)
    return -1;

  /* 1) 주소 공간 공유 */
  np->pagetable = p->pagetable;
  np->sz        = p->sz;

  /* 2) allocproc()이 만들어준 trapframe 물리페이지를
        부모 pagetable에도 매핑 (스레드마다 다른 VA) */
  np->trapframe_va = TRAPFRAME - (np->pid * PGSIZE);   // 예: 0x3ffff000, 0x3fffe000 ...
  if(mappages(p->pagetable,
              np->trapframe_va,
              PGSIZE,
              (uint64)np->trapframe,
              PTE_R | PTE_W) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  /* 3) 레지스터 세팅 */
  *(np->trapframe)   = *(p->trapframe);
  np->trapframe->epc = fcn;
  np->trapframe->sp  = stack + PGSIZE;
  np->trapframe->a0  = arg1;
  np->trapframe->a1  = arg2;

  /* 4) FD/CWD 복제 */
  for(int i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  /* 5) 메타데이터 */
  safestrcpy(np->name, p->name, sizeof(np->name));
  np->isthread  = 1;
  np->ustack    = (void*)stack;
  np->tg_leader = task_leader(p);

  /* 6) 실행 가능 상태 */
  np->state = RUNNABLE;
  release(&np->lock);

  /* 7) 부모-자식 연결, join 깨우기 */
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);
  wakeup(p);

  return np->pid;
}


int
join(uint64 ustack_addr)
{
  struct proc *p = myproc();
  struct proc *pp;
  int havekids, pid;

  acquire(&wait_lock);          // ① wait()와 동일하게, 루프 전체를 wait_lock 아래 둡니다.

  for(;;){
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p && pp->isthread){
        havekids = 1;

        acquire(&pp->lock);
        if(pp->state == ZOMBIE){
          pid = pp->pid;
          if(ustack_addr &&
             copyout(p->pagetable, ustack_addr,
                     (char *)&pp->ustack, sizeof(uint64)) < 0){
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freethread(pp);
          release(&pp->lock);
          release(&wait_lock);  // ② 성공 경로에서 락 해제
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 자식(쓰레드)이 없거나, killed 되었으면 실패
    if(!havekids || killed(p)){
      release(&wait_lock);      // ③ 오류/종료 경로에서도 꼭 해제
      return -1;
    }

    // sleep() 호출 시 wait_lock을 보유한 채로 들어가야 함
    sleep(p, &wait_lock);       // ④ wait_lock을 들고 있으므로 panic이 사라집니다.
  }
}

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  initlock(&mem_lock, "mem_lock");
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
// kernel/proc.c

static struct proc*
allocproc(void)
{
  struct proc *p;

  // 1) find a free slot
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == UNUSED){
      // 2) mark it and assign PID
      p->state = USED;
      p->pid   = allocpid();

      // 3) initialize new fields for both processes and threads
      p->isthread  = 0;
      p->tg_leader = p;
      p->ustack    = 0;

      // 4) allocate its trapframe
      if((p->trapframe = (struct trapframe*)kalloc()) == 0){
        freeproc(p);
        release(&p->lock);
        return 0;
      }

      // 5) create its page table
      if((p->pagetable = proc_pagetable(p)) == 0){
        freeproc(p);
        release(&p->lock);
        return 0;
      }

      // 6) set up its kernel context to come back via forkret
      memset(&p->context, 0, sizeof(p->context));
      p->context.ra = (uint64)forkret;
      p->context.sp = p->kstack + PGSIZE;

      return p;    // lock still held
    }
    release(&p->lock);
  }

  return 0;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  p->trapframe_va = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, p->trapframe_va = TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  acquire(&mem_lock);
  uint64 sz;
  struct proc *p = myproc();
  
  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  release(&mem_lock);
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  int leader_dying = !p->isthread;

  /* (파일·cwd 정리 부분은 그대로) */

  acquire(&wait_lock);

  acquire(&p->lock);          // ┐
  p->xstate = status;         // │ 1) 먼저 ZOMBIE 전환
  p->state  = ZOMBIE;         // ┘

  wakeup(p->parent);          // 2) 부모 깨우기 (이미 ZOMBIE)

  release(&wait_lock);        // 3) wait_lock 해제
                              //    p->lock은 그대로 쥔 채

  if(leader_dying){
    /* 같은 pagetable 공유하는 thread 모두 kill */
    for(struct proc *q = proc; q < &proc[NPROC]; q++){
      if(q != p && q->pagetable == p->pagetable){
        acquire(&q->lock);
        q->killed = 1;
        if(q->state == SLEEPING)
          q->state = RUNNABLE;
        release(&q->lock);
      }
    }
  }

  sched();                    // 4) p->lock 보유 상태로 안전하게 컨텍스트 스위치
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
            freethread(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
