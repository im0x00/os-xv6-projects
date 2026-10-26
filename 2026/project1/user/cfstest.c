#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NICE_MIN -3
#define NICE_MAX 2
#define NICE_COUNT 6
#define WORKERS_PER_CLASS 6
#define SHARE_TICKS 45
#define WAKE_TICKS 20
#define PRIORITY_WORKERS_PER_CLASS 3
#define PRIORITY_SETUP_TICKS 120
#define PRIORITY_WARMUP_TICKS 80
#define PRIORITY_RUN_TICKS 240

#define TAG_LOW 1
#define TAG_MID 2
#define TAG_HIGH 3
#define TAG_WAKE 4
#define TAG_HOG 5
#define TAG_CASE2 6
#define TAG_CASE3 7

int pass_count = 0;
int fail_count = 0;
volatile unsigned long sink;

struct report {
  int tag;
  int nice;
  int count;
  int ok;
};

void
check(int cond, char *name)
{
  if(cond){
    printf("[PASS] %s\n", name);
    pass_count++;
  } else {
    printf("[FAIL] %s\n", name);
    fail_count++;
  }
}

void
work_chunk(void)
{
  unsigned long x = sink + getpid();

  for(int i = 0; i < 20000; i++)
    x = x * 1103515245 + 12345 + i;

  sink = x;
}

int
count_until(int stop_tick)
{
  int count = 0;

  while(uptime() < stop_tick){
    work_chunk();
    count++;
  }

  return count;
}

void
wait_all(int n, int *ok)
{
  int status;

  for(int i = 0; i < n; i++){
    if(wait(&status) < 0 || status != 0)
      *ok = 0;
  }
}

void
start_children(int fd, int n, int stop_tick)
{
  for(int i = 0; i < n; i++)
    write(fd, &stop_tick, sizeof(stop_tick));
}

void
read_reports(int fd, struct report *reports, int n, int *ok)
{
  for(int i = 0; i < n; i++){
    int r = read(fd, &reports[i], sizeof(reports[i]));
    if(r != sizeof(reports[i]))
      *ok = 0;
  }
}

void
counter_child(int start_fd, int result_fd, int tag, int nice, int set_child_nice)
{
  int stop_tick;
  struct report r;

  r.tag = tag;
  r.nice = nice;
  r.count = 0;
  r.ok = 1;

  if(set_child_nice && set_nice(nice) != 0)
    r.ok = 0;

  if(read(start_fd, &stop_tick, sizeof(stop_tick)) != sizeof(stop_tick)){
    r.ok = 0;
  } else {
    r.count = count_until(stop_tick);
  }

  write(result_fd, &r, sizeof(r));
  exit(r.ok ? 0 : 1);
}

void
spawn_counter(int start_fd, int result_fd, int tag, int nice, int set_child_nice)
{
  int pid = fork();

  if(pid == 0){
    counter_child(start_fd, result_fd, tag, nice, set_child_nice);
  }
}

void
priority_child(int result_fd, int nice, int start_tick, int measure_tick, int stop_tick)
{
  struct report r;

  r.tag = TAG_MID;
  r.nice = nice;
  r.count = 0;
  r.ok = 1;

  if(set_nice(NICE_MAX) != 0)
    r.ok = 0;

  if(uptime() >= start_tick)
    r.ok = 0;

  while(uptime() < start_tick)
    work_chunk();

  if(set_nice(nice) != 0)
    r.ok = 0;

  while(uptime() < measure_tick)
    work_chunk();

  r.count = count_until(stop_tick);
  write(result_fd, &r, sizeof(r));
  exit(r.ok ? 0 : 1);
}

void
spawn_priority_counter(int result_fd, int nice, int start_tick, int measure_tick, int stop_tick)
{
  int pid = fork();

  if(pid == 0)
    priority_child(result_fd, nice, start_tick, measure_tick, stop_tick);
}

void
test_set_nice_boundary(void)
{
  printf("\n=== Test 1: set_nice range and return value ===\n");

  check(set_nice(-3) == 0, "set_nice(-3) returns 0");
  check(set_nice(-2) == 0, "set_nice(-2) returns 0");
  check(set_nice(-1) == 0, "set_nice(-1) returns 0");
  check(set_nice(0) == 0, "set_nice(0) returns 0");
  check(set_nice(1) == 0, "set_nice(1) returns 0");
  check(set_nice(2) == 0, "set_nice(2) returns 0");

  check(set_nice(-4) == 1, "set_nice(-4) returns 1");
  check(set_nice(3) == 1, "set_nice(3) returns 1");
  check(set_nice(-100) == 1, "set_nice(-100) returns 1");
  check(set_nice(100) == 1, "set_nice(100) returns 1");

  set_nice(0);
}

void
test_initial_nice_and_basic_fork(void)
{
  printf("\n=== Test 2: default nice and basic fork ===\n");

  set_nice(0);

  int pid = fork();
  if(pid == 0){
    int r1 = set_nice(0);
    int r2 = set_nice(NICE_MIN);
    int r3 = set_nice(NICE_MAX);
    exit(r1 == 0 && r2 == 0 && r3 == 0 ? 0 : 1);
  }

  int status;
  wait(&status);
  check(status == 0, "new process accepts valid nice range");
  set_nice(0);
}

void
test_nice_inheritance_by_share(void)
{
  printf("\n=== Test 3: nice inheritance through fork ===\n");

  int start_pipe[2];
  int result_pipe[2];
  int ok = 1;
  int total_low = 0;
  int total_high = 0;
  int n = WORKERS_PER_CLASS * 2;
  struct report reports[WORKERS_PER_CLASS * 2];

  pipe(start_pipe);
  pipe(result_pipe);

  for(int i = 0; i < WORKERS_PER_CLASS; i++){
    set_nice(NICE_MIN);
    spawn_counter(start_pipe[0], result_pipe[1], TAG_LOW, NICE_MIN, 0);
  }

  for(int i = 0; i < WORKERS_PER_CLASS; i++){
    set_nice(NICE_MAX);
    spawn_counter(start_pipe[0], result_pipe[1], TAG_HIGH, NICE_MAX, 0);
  }

  set_nice(0);
  start_children(start_pipe[1], n, uptime() + SHARE_TICKS);
  read_reports(result_pipe[0], reports, n, &ok);
  wait_all(n, &ok);

  for(int i = 0; i < n; i++){
    if(!reports[i].ok)
      ok = 0;
    if(reports[i].tag == TAG_LOW)
      total_low += reports[i].count;
    if(reports[i].tag == TAG_HIGH)
      total_high += reports[i].count;
  }

  printf("inherited nice -3 work: %d\n", total_low);
  printf("inherited nice  2 work: %d\n", total_high);
  check(ok && total_low > total_high + total_high / 2,
        "children inherit parent nice value");

  close(start_pipe[0]);
  close(start_pipe[1]);
  close(result_pipe[0]);
  close(result_pipe[1]);
  set_nice(0);
}

void
test_priority_weighting(void)
{
  printf("\n=== Test 4: CFS priority weighting ===\n");

  int result_pipe[2];
  int ok = 1;
  int n = NICE_COUNT * PRIORITY_WORKERS_PER_CLASS;
  int totals[NICE_COUNT];
  struct report reports[NICE_COUNT * PRIORITY_WORKERS_PER_CLASS];
  int start_tick = uptime() + PRIORITY_SETUP_TICKS;
  int measure_tick = start_tick + PRIORITY_WARMUP_TICKS;
  int stop_tick = measure_tick + PRIORITY_RUN_TICKS;

  for(int i = 0; i < NICE_COUNT; i++)
    totals[i] = 0;

  pipe(result_pipe);
  set_nice(NICE_MIN);

  for(int round = 0; round < PRIORITY_WORKERS_PER_CLASS; round++){
    if(round % 2 == 0){
      for(int nice = NICE_MIN; nice <= NICE_MAX; nice++)
        spawn_priority_counter(result_pipe[1], nice, start_tick, measure_tick, stop_tick);
    } else {
      for(int nice = NICE_MAX; nice >= NICE_MIN; nice--)
        spawn_priority_counter(result_pipe[1], nice, start_tick, measure_tick, stop_tick);
    }
  }

  set_nice(0);
  read_reports(result_pipe[0], reports, n, &ok);
  wait_all(n, &ok);

  for(int i = 0; i < n; i++){
    if(!reports[i].ok)
      ok = 0;
    if(reports[i].nice >= NICE_MIN && reports[i].nice <= NICE_MAX)
      totals[reports[i].nice - NICE_MIN] += reports[i].count;
  }

  for(int i = 0; i < NICE_COUNT; i++)
    printf("nice %d work: %d\n", i + NICE_MIN, totals[i]);

  int nonzero = 1;
  for(int i = 0; i < NICE_COUNT; i++){
    if(totals[i] == 0)
      nonzero = 0;
  }

  int ordered = 1;
  for(int i = 0; i < NICE_COUNT - 1; i++){
    if(totals[i] <= totals[i + 1])
      ordered = 0;
  }

  check(ok && nonzero, "all nice classes enter the measured interval");
  check(ok && nonzero && ordered, "lower nice receives more CPU work");
  check(ok && totals[0] > totals[3] && totals[3] > totals[5],
        "vruntime increment grows as nice increases");

  close(result_pipe[0]);
  close(result_pipe[1]);
  set_nice(0);
}

void
test_wakeup_vruntime(void)
{
  printf("\n=== Test 5: wakeup vruntime update ===\n");

  int start_pipe[2];
  int result_pipe[2];
  int ok = 1;
  int hog_total = 0;
  int wake_total = 0;
  int n = WORKERS_PER_CLASS + 1;
  struct report reports[WORKERS_PER_CLASS + 1];

  pipe(start_pipe);
  pipe(result_pipe);

  int pid = fork();
  if(pid == 0){
    int stop_tick;
    struct report r;

    r.tag = TAG_WAKE;
    r.nice = 0;
    r.count = 0;
    r.ok = 1;

    if(set_nice(0) != 0)
      r.ok = 0;

    count_until(uptime() + 8);
    pause(5);

    if(read(start_pipe[0], &stop_tick, sizeof(stop_tick)) != sizeof(stop_tick)){
      r.ok = 0;
    } else {
      r.count = count_until(stop_tick);
    }

    write(result_pipe[1], &r, sizeof(r));
    exit(r.ok ? 0 : 1);
  }

  for(int i = 0; i < WORKERS_PER_CLASS; i++)
    spawn_counter(start_pipe[0], result_pipe[1], TAG_HOG, 0, 1);

  pause(8);
  start_children(start_pipe[1], n, uptime() + WAKE_TICKS);
  read_reports(result_pipe[0], reports, n, &ok);
  wait_all(n, &ok);

  for(int i = 0; i < n; i++){
    if(!reports[i].ok)
      ok = 0;
    if(reports[i].tag == TAG_WAKE)
      wake_total += reports[i].count;
    if(reports[i].tag == TAG_HOG)
      hog_total += reports[i].count;
  }

  printf("woken process work: %d\n", wake_total);
  printf("runnable competitors total work: %d\n", hog_total);
  check(ok && wake_total > 0, "sleeping process wakes and runs");
  check(ok && wake_total * WORKERS_PER_CLASS >= hog_total / 2,
        "woken process is not left behind after wakeup");

  close(start_pipe[0]);
  close(start_pipe[1]);
  close(result_pipe[0]);
  close(result_pipe[1]);
  set_nice(0);
}

void
test_wakeup_vruntime_running_only(void)
{
  printf("\n=== Test 6: wakeup vruntime with only RUNNING competitor ===\n");

  int result_pipe[2];
  int ok = 1;
  int hog_total = 0;
  int wake_total = 0;
  int start_tick = uptime() + 5;
  int stop_tick = start_tick + WAKE_TICKS;
  struct report reports[2];

  pipe(result_pipe);

  int pid = fork();
  if(pid == 0){
    struct report r;

    r.tag = TAG_CASE2;
    r.nice = 0;
    r.count = 0;
    r.ok = 1;

    pause(5);
    r.count = count_until(stop_tick);

    write(result_pipe[1], &r, sizeof(r));
    exit(0);
  }

  pid = fork();
  if(pid == 0){
    struct report r;

    r.tag = TAG_HOG;
    r.nice = 0;
    r.count = count_until(stop_tick);
    r.ok = 1;

    write(result_pipe[1], &r, sizeof(r));
    exit(0);
  }

  read_reports(result_pipe[0], reports, 2, &ok);
  wait_all(2, &ok);

  for(int i = 0; i < 2; i++){
    if(!reports[i].ok)
      ok = 0;
    if(reports[i].tag == TAG_CASE2)
      wake_total += reports[i].count;
    if(reports[i].tag == TAG_HOG)
      hog_total += reports[i].count;
  }

  printf("running-only wake work: %d\n", wake_total);
  printf("single running competitor work: %d\n", hog_total);
  check(ok && wake_total > 0, "process wakes when only a RUNNING process exists");
  check(ok && wake_total < hog_total * 2,
        "woken process does not restart far before running minimum");

  close(result_pipe[0]);
  close(result_pipe[1]);
  set_nice(0);
}

void
test_wakeup_vruntime_empty_system(void)
{
  printf("\n=== Test 7: wakeup vruntime with no RUNNABLE or RUNNING peer ===\n");

  int result_pipe[2];
  int ok = 1;
  struct report r;

  pipe(result_pipe);

  int pid = fork();
  if(pid == 0){
    struct report child_report;

    child_report.tag = TAG_CASE3;
    child_report.nice = 0;
    child_report.count = 0;
    child_report.ok = 1;

    pause(3);
    child_report.count = count_until(uptime() + WAKE_TICKS);

    write(result_pipe[1], &child_report, sizeof(child_report));
    exit(0);
  }

  read_reports(result_pipe[0], &r, 1, &ok);
  wait_all(1, &ok);

  if(!r.ok)
    ok = 0;

  printf("empty-system wake work: %d\n", r.count);
  check(ok && r.tag == TAG_CASE3 && r.count > 0,
        "process wakes when no RUNNABLE or RUNNING peer exists");

  close(result_pipe[0]);
  close(result_pipe[1]);
  set_nice(0);
}

void
test_sleeping_kill_stability(void)
{
  printf("\n=== Test 8: kill sleeping process stability ===\n");

  int pid = fork();
  if(pid == 0){
    pause(1000);
    exit(0);
  }

  pause(2);
  kill(pid);

  int status;
  wait(&status);
  check(1, "kill of sleeping process does not crash scheduler");
}

void
test_many_runnable_stability(void)
{
  printf("\n=== Test 9: many runnable processes stability ===\n");

  int ok = 1;

  for(int nice = NICE_MIN; nice <= NICE_MAX; nice++){
    int pid = fork();
    if(pid == 0){
      if(set_nice(nice) != 0)
        exit(1);
      count_until(uptime() + 10);
      exit(0);
    }
  }

  wait_all(NICE_COUNT, &ok);
  check(ok, "nice -3 through 2 processes all complete");
  set_nice(0);
}

int
main(int argc, char *argv[])
{
  printf("=============================\n");
  printf("  CFS Scheduler Test Suite\n");
  printf("  works with CPUS=1 and SMP\n");
  printf("=============================\n");

  test_set_nice_boundary();
  test_initial_nice_and_basic_fork();
  test_nice_inheritance_by_share();
  test_priority_weighting();
  test_wakeup_vruntime();
  test_wakeup_vruntime_running_only();
  test_wakeup_vruntime_empty_system();
  test_sleeping_kill_stability();
  test_many_runnable_stability();

  printf("\n=============================\n");
  printf("PASS: %d / FAIL: %d\n", pass_count, fail_count);
  if(fail_count == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");
  printf("=============================\n");

  exit(fail_count == 0 ? 0 : 1);
}



