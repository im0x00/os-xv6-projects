# Memory-mapped files 설계 및 구현 정리

이 문서는 현재 코드베이스에서 `mmap`/`munmap` 기반 memory-mapped files가 어떤 설계로 들어갔고, 실제로 어떤 코드 경로에서 동작하는지 정리한다. 핵심 설계는 **VMA(Virtual Memory Area) 메타데이터만 먼저 등록하고, 실제 물리 페이지와 파일 내용은 page fault 시점에 lazy하게 채우는 방식**이다.

## 1. 사용자 API와 syscall 연결

사용자 프로그램은 `mmap()`과 `munmap()`을 일반 시스템콜처럼 호출한다. 보호 플래그와 mapping 플래그는 `kernel/fcntl.h`에 추가되어 있다.

```c
// kernel/fcntl.h:7-16
// mmap prot bits (PROT_NONE/PROT_EXEC are defined for reference but
// not exercised by mmaptest).
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

// mmap flags (mutually exclusive)
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
```

시스템콜 번호는 `SYS_mmap`, `SYS_munmap`으로 배정되어 있고, syscall dispatch table에 등록된다.

```c
// kernel/syscall.h:23-24
#define SYS_mmap   22
#define SYS_munmap 23
```

```c
// kernel/syscall.c:104-105
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
```

```c
// kernel/syscall.c:131-132
[SYS_mmap]    sys_mmap,
[SYS_munmap]  sys_munmap,
```

사용자 영역에는 prototype과 syscall stub 생성 항목이 추가되어 있다.

```c
// user/user.h:27-28
char* mmap(int len, int prot, int flags, int fd, int offset);
int   munmap(void *addr, int len);
```

```perl
# user/usys.pl:45-46
entry("mmap");
entry("munmap");
```

## 2. VMA 기반 상태 관리

프로세스마다 최대 16개의 VMA 슬롯을 가진다. 각 VMA는 mapping의 시작 주소, 길이, protection, shared/private 여부, 파일 offset, refcount된 파일 포인터를 저장한다.

```c
// kernel/proc.h:84-95
// mmap virtual memory area. One slot per active mapping.
#define NVMA 16

struct vma {
  int          used;     // 1 if this slot is in use
  uint64       addr;     // start virtual address (page-aligned)
  uint64       length;   // length in bytes (PGSIZE multiple)
  int          prot;     // PROT_READ / PROT_WRITE
  int          flags;    // MAP_SHARED / MAP_PRIVATE
  int          offset;   // file offset (always 0 in this lab)
  struct file *f;        // mapped file (refcounted via filedup)
};
```

`struct proc`에는 이 VMA 배열이 들어간다.

```c
// kernel/proc.h:111-120
// these are private to the process, so p->lock need not be held.
uint64 kstack;               // Virtual address of kernel stack
uint64 sz;                   // Size of process memory (bytes)
pagetable_t pagetable;       // User page table
struct trapframe *trapframe; // data page for trampoline.S
struct context context;      // swtch() here to run process
struct file *ofile[NOFILE];  // Open files
struct inode *cwd;           // Current directory
char name[16];               // Process name (debugging)
struct vma vmas[NVMA];       // mmap regions
```

새 프로세스를 만들 때는 VMA 슬롯을 비운다.

```c
// kernel/proc.c:150-154
// No mmap regions yet.
for(int i = 0; i < NVMA; i++){
  p->vmas[i].used = 0;
  p->vmas[i].f = 0;
}
```




## 6. fork/exit에서의 수명 관리



## 7. 구현이 통과하려는 테스트 범위

`user/mmaptest.c`는 다음 성질을 확인한다.

- 기본 file-backed read mapping이 파일 내용을 읽고, 파일 끝 이후 page 부분이 0으로 보이는지
- `MAP_PRIVATE` 수정이 파일에 반영되지 않는지
- `MAP_SHARED | PROT_WRITE` 수정이 `munmap()` 후 파일에 반영되는지
- mapping을 만들고 일부만 접근했을 때 lazy loading이 동작하는지
- 서로 다른 파일 mapping이 충돌하지 않는지
- `fork()` 후 부모/자식이 VMA를 유지하는지
- `munmap()` 후 접근하면 fault로 종료되는지
- read-only mapping에 쓰면 fault로 종료되는지

lazy loading 테스트는 실제로 3 page를 mapping하지만 첫 byte만 접근한다.

```c
// user/mmaptest.c:203-207
// mmap a large region (3 pages), only touch first byte to verify laziness
char *p = mmap(PGSIZE*3, PROT_READ, MAP_PRIVATE, fd, 0);
if(p == MAP_FAILED) err("mmap");
if(p[0] != 'A') err("read");
if(munmap(p, PGSIZE*3) == -1) err("munmap");
```

fork 테스트는 child와 parent가 같은 VMA 정보를 바탕으로 각각 내용을 읽을 수 있음을 확인한다.

```c
// user/mmaptest.c:265-281
char *p = mmap(PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);
if(p == MAP_FAILED) err("mmap");

int pid = fork();
if(pid < 0) err("fork");
if(pid == 0){
  // child checks its mapping
  verify_content(p);
  if(munmap(p, PGSIZE*2) == -1) err("munmap child");
  exit(0);
}
int status;
wait(&status);
if(status != 0) err("child exit");
// parent still has mapping
verify_content(p);
if(munmap(p, PGSIZE*2) == -1) err("munmap parent");
```



## 9. 전체 흐름 요약

1. 사용자 `mmap(len, prot, flags, fd, 0)` 호출
2. kernel이 인자를 검증하고, `TRAPFRAME` 아래 빈 주소 구간을 찾음
3. VMA 슬롯에 주소/길이/권한/flags/file reference를 저장하고 주소 반환
4. 사용자가 mapping 주소에 접근하면 page fault 발생
5. `trap.c`가 `mmapfault()` 호출
6. `mmapfault()`가 파일 내용을 읽어 물리 page를 채우고 PTE 등록
7. `munmap()` 또는 `exit()` 때 `MAP_SHARED | PROT_WRITE` page를 파일에 write-back하고 PTE/VMA/file reference 정리

정리하면, 이 구현은 **VMA metadata + lazy page fault + munmap write-back** 조합으로 memory-mapped files를 구현했다. 과제 테스트 범위에서는 충분히 잘 맞지만, 파일 offset 기반 write-back과 syscall buffer fault 처리 부재는 실제 mmap처럼 확장하려면 먼저 보완해야 할 지점이다.
