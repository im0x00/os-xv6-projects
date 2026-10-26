#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/riscv.h"
#include "user/user.h"
#include "user/uthread.h"

#define NJOIN 4
#define WAIT_STATUS 42
#define LEADER_EXIT_STATUS 61

static char *current_test = "startup";

static volatile int shared_value;
static volatile int private_value;
static volatile int joined_slots[NJOIN];
static volatile int join_from_thread_result;
static volatile int wrapper_value;
static volatile int sleeper_ready;
static volatile int limit_release;
static volatile int limit_started[NTHREAD];

struct slot_arg {
  int slot;
  int value;
};

struct sbrk_arg {
  char *oldbrk;
  char *allocated;
  int ok;
};

static void
fail_at(int line, char *msg)
{
  printf("kproctest: FAIL [%s] line %d: %s\n", current_test, line, msg);
  exit(1);
}

static void
fail_eq_at(int line, char *msg, int got, int want)
{
  printf("kproctest: FAIL [%s] line %d: %s got=%d want=%d\n",
         current_test, line, msg, got, want);
  exit(1);
}

static void
fail_ptr_eq_at(int line, char *msg, void *got, void *want)
{
  printf("kproctest: FAIL [%s] line %d: %s got=%p want=%p\n",
         current_test, line, msg, got, want);
  exit(1);
}

#define CHECK(cond, msg) do { \
  if(!(cond)) \
    fail_at(__LINE__, msg); \
} while(0)

#define CHECK_EQ(got, want, msg) do { \
  int _got = (got); \
  int _want = (want); \
  if(_got != _want) \
    fail_eq_at(__LINE__, msg, _got, _want); \
} while(0)

#define CHECK_PTR_EQ(got, want, msg) do { \
  void *_got = (void *)(got); \
  void *_want = (void *)(want); \
  if(_got != _want) \
    fail_ptr_eq_at(__LINE__, msg, _got, _want); \
} while(0)

static void
begin(char *name)
{
  current_test = name;
  printf("kproctest: RUN %s\n", current_test);
}

static void
pass(void)
{
  printf("kproctest: PASS %s\n", current_test);
}

static int
known_stack_index(void *stack, void **stacks, int *seen, int n)
{
  for(int i = 0; i < n; i++){
    if(stack == stacks[i]){
      if(seen[i])
        return -1;
      seen[i] = 1;
      return i;
    }
  }
  return -1;
}

static int
clone_one(void (*fn)(void *), void *arg, void **stack_out)
{
  void *stack = malloc(PGSIZE);
  int tid;

  CHECK(stack != 0, "malloc for thread stack failed");
  tid = clone(fn, arg, stack, 1, CLONE_VM);
  if(tid < 0){
    free(stack);
    return -1;
  }

  *stack_out = stack;
  return tid;
}

static void
join_one(int want_tid, void *want_stack)
{
  void *joined_stack = 0;
  int got = join(&joined_stack);

  CHECK_EQ(got, want_tid, "join returned the wrong tid");
  CHECK_PTR_EQ(joined_stack, want_stack, "join returned the wrong stack");
  free(joined_stack);
}

static void
shared_worker(void *arg)
{
  int *value = (int *)arg;
  shared_value = *value + 7;
  exit(0);
}

static void
slot_worker(void *arg)
{
  struct slot_arg *slot = (struct slot_arg *)arg;
  joined_slots[slot->slot] = slot->value;
  exit(0);
}

static void
join_from_thread_worker(void *arg)
{
  (void)arg;
  join_from_thread_result = join(0);
  exit(0);
}

static void
sbrk_worker(void *arg)
{
  struct sbrk_arg *a = (struct sbrk_arg *)arg;
  char *p = sbrk(PGSIZE);

  if(p == (char *)-1){
    a->ok = -1;
    exit(0);
  }

  p[0] = 'T';
  p[PGSIZE - 1] = 'Z';
  a->allocated = p;
  a->ok = 1;
  exit(0);
}

static void
sleeper_worker(void *arg)
{
  volatile int *ready = (volatile int *)arg;
  *ready = 1;
  for(;;)
    pause(1);
}

static void
limit_worker(void *arg)
{
  int slot = *(int *)arg;
  limit_started[slot] = 1;
  while(limit_release == 0)
    pause(1);
  exit(0);
}

static void
wrapper_worker(void *arg)
{
  int *value = (int *)arg;
  wrapper_value = *value;
  exit(0);
}

static void
test_clone_process_private_vm(void)
{
  int pid;
  int got;
  int status = -999;

  begin("clone_process_private_vm");

  private_value = 111;
  pid = fork();
  CHECK(pid >= 0, "fork wrapper over clone failed");
  if(pid == 0){
    private_value = 222;
    exit(WAIT_STATUS);
  }

  got = wait(&status);
  CHECK_EQ(got, pid, "wait returned the wrong child pid");
  CHECK_EQ(status, WAIT_STATUS, "wait copied out the wrong child status");
  CHECK_EQ(private_value, 111, "process clone unexpectedly shared memory");

  pass();
}

static void
test_clone_thread_shared_vm_and_join(void)
{
  void *stack = 0;
  int arg = 35;
  int tid;

  begin("clone_thread_shared_vm_and_join");

  shared_value = 0;
  tid = clone_one(shared_worker, &arg, &stack);
  CHECK(tid > 0, "clone(CLONE_VM) failed");
  join_one(tid, stack);
  CHECK_EQ(shared_value, 42, "thread did not share address space");
  CHECK_EQ(wait(0), -1, "wait should not reap a joined thread");

  pass();
}

static void
test_join_collects_all_threads_once(void)
{
  void *stacks[NJOIN];
  int tids[NJOIN];
  int seen[NJOIN];
  struct slot_arg args[NJOIN];

  begin("join_collects_all_threads_once");

  for(int i = 0; i < NJOIN; i++){
    joined_slots[i] = 0;
    seen[i] = 0;
    stacks[i] = malloc(PGSIZE);
    CHECK(stacks[i] != 0, "malloc for thread stack failed");
    args[i].slot = i;
    args[i].value = 100 + i;
    tids[i] = clone(slot_worker, &args[i], stacks[i], 1, CLONE_VM);
    CHECK(tids[i] > 0, "clone(CLONE_VM) failed");
  }

  for(int i = 0; i < NJOIN; i++){
    void *joined_stack = 0;
    int joined_tid = join(&joined_stack);
    CHECK(joined_tid > 0, "join failed before all threads were collected");
    CHECK(known_stack_index(joined_stack, stacks, seen, NJOIN) >= 0,
          "join returned an unknown or duplicate stack");
    free(joined_stack);
  }

  for(int i = 0; i < NJOIN; i++)
    CHECK_EQ(joined_slots[i], 100 + i, "thread did not write its slot");
  CHECK_EQ(join(0), -1, "join with no remaining threads should fail");

  pass();
}

static void
test_wait_reaps_process_exit_status(void)
{
  int pid;
  int got;
  int status = -999;

  begin("wait_reaps_process_exit_status");

  pid = fork();
  CHECK(pid >= 0, "fork wrapper over clone failed");
  if(pid == 0)
    exit(WAIT_STATUS);

  got = wait(&status);
  CHECK_EQ(got, pid, "wait returned the wrong child pid");
  CHECK_EQ(status, WAIT_STATUS, "wait returned the wrong exit status");
  CHECK_EQ(wait(0), -1, "wait with no children should fail");

  pass();
}

static void
test_wait_ignores_threads(void)
{
  void *stack = 0;
  int arg = 5;
  int tid;

  begin("wait_ignores_threads");

  shared_value = 0;
  tid = clone_one(shared_worker, &arg, &stack);
  CHECK(tid > 0, "clone(CLONE_VM) failed");
  while(shared_value == 0)
    pause(1);

  CHECK_EQ(wait(0), -1, "wait should not reap a sibling thread");
  join_one(tid, stack);

  pass();
}

static void
test_join_restricted_to_leader(void)
{
  void *stack = 0;
  int tid;

  begin("join_restricted_to_leader");

  join_from_thread_result = 99;
  tid = clone_one(join_from_thread_worker, 0, &stack);
  CHECK(tid > 0, "clone(CLONE_VM) failed");
  join_one(tid, stack);
  CHECK_EQ(join_from_thread_result, -1, "non-leader thread should not join");

  pass();
}

static void
test_thread_sbrk_shares_mm(void)
{
  void *stack = 0;
  int tid;
  struct sbrk_arg arg;

  begin("thread_sbrk_shares_mm");

  arg.oldbrk = sbrk(0);
  arg.allocated = 0;
  arg.ok = 0;

  tid = clone_one(sbrk_worker, &arg, &stack);
  CHECK(tid > 0, "clone(CLONE_VM) failed");
  join_one(tid, stack);

  CHECK_EQ(arg.ok, 1, "thread sbrk failed");
  CHECK_PTR_EQ(arg.allocated, arg.oldbrk, "thread sbrk used the wrong break");
  CHECK_EQ(arg.allocated[0], 'T', "parent could not read first byte");
  CHECK_EQ(arg.allocated[PGSIZE - 1], 'Z', "parent could not read last byte");
  CHECK_PTR_EQ(sbrk(0), arg.oldbrk + PGSIZE,
               "parent did not observe shared break growth");
  CHECK_PTR_EQ(sbrk(-PGSIZE), arg.oldbrk + PGSIZE,
               "sbrk shrink returned the wrong old break");
  CHECK_PTR_EQ(sbrk(0), arg.oldbrk, "sbrk shrink did not restore old break");

  pass();
}

static void
test_exit_leader_drains_threads(void)
{
  int pid;
  int got;
  int status = -999;

  begin("exit_leader_drains_threads");

  pid = fork();
  CHECK(pid >= 0, "fork wrapper over clone failed");
  if(pid == 0){
    void *stack = 0;
    int tid;

    sleeper_ready = 0;
    tid = clone_one(sleeper_worker, (void *)&sleeper_ready, &stack);
    CHECK(tid > 0, "child clone(CLONE_VM) failed");
    while(sleeper_ready == 0)
      pause(1);

    exit(LEADER_EXIT_STATUS);
  }

  got = wait(&status);
  CHECK_EQ(got, pid, "wait returned the wrong child pid");
  CHECK_EQ(status, LEADER_EXIT_STATUS, "leader exit status was not preserved");

  pass();
}

static void
test_kill_process_reports_minus_one(void)
{
  int fds[2];
  int pid;
  int got;
  int status = -999;
  char ready = 0;

  begin("kill_process_reports_minus_one");

  CHECK_EQ(pipe(fds), 0, "pipe failed");
  pid = fork();
  CHECK(pid >= 0, "fork wrapper over clone failed");
  if(pid == 0){
    close(fds[0]);
    ready = 'R';
    CHECK_EQ(write(fds[1], &ready, 1), 1, "child failed to report ready");
    close(fds[1]);
    for(;;)
      pause(10);
  }

  close(fds[1]);
  CHECK_EQ(read(fds[0], &ready, 1), 1, "parent did not receive ready byte");
  close(fds[0]);

  CHECK_EQ(kill(pid), 0, "kill returned failure for live child");
  got = wait(&status);
  CHECK_EQ(got, pid, "wait returned the wrong killed child pid");
  CHECK_EQ(status, -1, "killed child should report status -1");
  CHECK_EQ(kill(pid), -1, "kill should fail after child is reaped");

  pass();
}

static void
test_kill_thread_group_by_tid(void)
{
  int fds[2];
  int pid;
  int tid = -1;
  int got;
  int status = -999;

  begin("kill_thread_group_by_tid");

  CHECK_EQ(pipe(fds), 0, "pipe failed");
  pid = fork();
  CHECK(pid >= 0, "fork wrapper over clone failed");
  if(pid == 0){
    void *stack = 0;

    close(fds[0]);
    sleeper_ready = 0;
    tid = clone_one(sleeper_worker, (void *)&sleeper_ready, &stack);
    CHECK(tid > 0, "child clone(CLONE_VM) failed");
    while(sleeper_ready == 0)
      pause(1);
    CHECK_EQ(write(fds[1], &tid, sizeof(tid)), sizeof(tid),
             "child failed to report thread tid");
    close(fds[1]);
    for(;;)
      pause(10);
  }

  close(fds[1]);
  CHECK_EQ(read(fds[0], &tid, sizeof(tid)), sizeof(tid),
           "parent did not receive thread tid");
  close(fds[0]);
  CHECK(tid > 0, "child reported an invalid thread tid");

  CHECK_EQ(kill(tid), 0, "kill by thread tid failed");
  got = wait(&status);
  CHECK_EQ(got, pid, "wait should reap the killed group leader");
  CHECK_EQ(status, -1, "killed thread group should report status -1");

  pass();
}

static void
test_clone_thread_limit_no_panic(void)
{
  void *stacks[NTHREAD];
  int tids[NTHREAD];
  int slots[NTHREAD];
  int seen[NTHREAD];
  int created = 0;

  begin("clone_thread_limit_no_panic");

  limit_release = 0;
  for(int i = 0; i < NTHREAD; i++){
    limit_started[i] = 0;
    seen[i] = 0;
    stacks[i] = malloc(PGSIZE);
    CHECK(stacks[i] != 0, "malloc for limit stack failed");
    slots[i] = i;
    tids[i] = clone(limit_worker, &slots[i], stacks[i], 1, CLONE_VM);

    if(i < NTHREAD - 1){
      CHECK(tids[i] > 0, "clone failed before thread slots were exhausted");
      created++;
    } else {
      CHECK_EQ(tids[i], -1, "clone past thread limit should fail");
      free(stacks[i]);
    }
  }

  for(int i = 0; i < created; i++){
    while(limit_started[i] == 0)
      pause(1);
  }

  limit_release = 1;
  for(int i = 0; i < created; i++){
    void *joined_stack = 0;
    int joined_tid = join(&joined_stack);
    CHECK(joined_tid > 0, "join failed before all limit threads were collected");
    CHECK(known_stack_index(joined_stack, stacks, seen, created) >= 0,
          "join returned an unknown or duplicate limit stack");
    free(joined_stack);
  }

  pass();
}

static void
test_uthread_wrappers(void)
{
  int arg = 777;
  int tid;
  int joined_tid;

  begin("uthread_wrappers");

  wrapper_value = 0;
  tid = thread_create(wrapper_worker, &arg, 1);
  CHECK(tid > 0, "thread_create failed");
  joined_tid = thread_join();
  CHECK_EQ(joined_tid, tid, "thread_join returned the wrong tid");
  CHECK_EQ(wrapper_value, 777, "wrapper thread did not share memory");

  pass();
}

static void
run_all(void)
{
  test_clone_process_private_vm();
  test_clone_thread_shared_vm_and_join();
  test_join_collects_all_threads_once();
  test_wait_reaps_process_exit_status();
  test_wait_ignores_threads();
  test_join_restricted_to_leader();
  test_thread_sbrk_shares_mm();
  test_exit_leader_drains_threads();
  test_kill_process_reports_minus_one();
  test_kill_thread_group_by_tid();
  test_clone_thread_limit_no_panic();
  test_uthread_wrappers();
  printf("kproctest: PASS all\n");
}

static void
usage(void)
{
  printf("usage: kproctest [all|clone_process|clone_thread|join_many|wait_exit|wait_threads|join_leader|sbrk|exit_group|kill_process|kill_group|limit|uthread]\n");
}

int
main(int argc, char **argv)
{
  if(argc == 1 || strcmp(argv[1], "all") == 0){
    run_all();
  } else if(strcmp(argv[1], "clone_process") == 0){
    test_clone_process_private_vm();
  } else if(strcmp(argv[1], "clone_thread") == 0){
    test_clone_thread_shared_vm_and_join();
  } else if(strcmp(argv[1], "join_many") == 0){
    test_join_collects_all_threads_once();
  } else if(strcmp(argv[1], "wait_exit") == 0){
    test_wait_reaps_process_exit_status();
  } else if(strcmp(argv[1], "wait_threads") == 0){
    test_wait_ignores_threads();
  } else if(strcmp(argv[1], "join_leader") == 0){
    test_join_restricted_to_leader();
  } else if(strcmp(argv[1], "sbrk") == 0){
    test_thread_sbrk_shares_mm();
  } else if(strcmp(argv[1], "exit_group") == 0){
    test_exit_leader_drains_threads();
  } else if(strcmp(argv[1], "kill_process") == 0){
    test_kill_process_reports_minus_one();
  } else if(strcmp(argv[1], "kill_group") == 0){
    test_kill_thread_group_by_tid();
  } else if(strcmp(argv[1], "limit") == 0){
    test_clone_thread_limit_no_panic();
  } else if(strcmp(argv[1], "uthread") == 0){
    test_uthread_wrappers();
  } else {
    usage();
    exit(1);
  }

  exit(0);
}
