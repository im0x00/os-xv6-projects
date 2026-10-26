//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a hard link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off = 2 * sizeof(de); off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// Create a new inode (file, directory, device, or symlink).
// If linktarget != 0, create a T_SYMLINK and store linktarget in its data.
// ----------------------------------------------------------
// allocate + initialise new inode (file / dir / device / symlink)
//   path        : full pathname we want to create
//   type        : T_FILE / T_DIR / T_DEVICE / T_SYMLINK
//   major/minor : for T_DEVICE
//   linktarget  : (T_SYMLINK 전용) 심볼릭 링크가 가리킬 경로, NULL 이면 무시
//
// 성공 시   : lock 을 잡은 상태의 inode* 반환
// 실패 시   : 0 반환 (부모 락은 풀림, 할당된 블록/inode 들도 정리됨)
// ----------------------------------------------------------
static struct inode*
create(char *path, short type, short major, short minor, char *linktarget)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  // 1. 부모 디렉터리 찾기
  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  // 2. 같은 이름이 이미 존재?
  if((ip = dirlookup(dp, name, 0)) != 0){
    // 이미 있으면, 파일·디바이스일 때만 재사용 허용
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;                 // ← lock 잡힌 채 반환
    iunlockput(ip);
    return 0;
  }

  // 3. 새 inode 할당
  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }
  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  // 4. 디렉터리라면 "." ".." 엔트리 작성
  if(type == T_DIR){
    if(dirlink(ip, ".",  ip->inum) < 0 ||
       dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  // 5. 심볼릭 링크라면 target 문자열 저장
  if(type == T_SYMLINK && linktarget){
    // (1) 경로 길이 검사
    int len = strlen(linktarget);
    if(len >= MAXPATH)           // MAXPATH-1 글자 + '\0' 보장
      goto fail;

    // (2) target + NULL 포함해 writei()
    if(writei(ip, 0, (uint64)linktarget, 0, len + 1) != len + 1)
      goto fail;
  }

  // 6. 부모 디렉터리에 링크 추가
  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    dp->nlink++;     // ".." 가 부모를 가리키므로, 부모 link-count +1
    iupdate(dp);
  }

  iunlockput(dp);     // 부모 디렉터리 락/참조 해제
  return ip;          // *** ip 는 아직 lock 상태! ***

fail:
  // 되돌리기
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int  fd, omode, n;
  struct file  *f;
  struct inode *ip;

  // 사용자 인자 얻기
  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();
  int depth = 0;                       // symlink 중첩 깊이

resolve_again:
  if(omode & O_CREATE){
    /* ---------- 새 파일 생성 ---------- */
    ip = create(path, T_FILE, 0, 0, 0);   // ip 는 lock 보유
    if(ip == 0){ end_op(); return -1; }
  } else {
    /* ---------- 기존 경로 열기 ---------- */
    if((ip = namei(path)) == 0){ end_op(); return -1; }
    ilock(ip);                           // 반드시 lock 확보

    while(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)){
      if(++depth > 10){                  // ELOOP 방지
        iunlock(ip); iput(ip); end_op();
        return -1;
      }

      /* ★★★  lock을 유지한 채 target 읽기  ★★★ */
      char buf[MAXPATH];
      int r = readi(ip, 0, (uint64)buf, 0, MAXPATH-1);
      if(r <= 0){
        iunlock(ip); iput(ip); end_op(); return -1;
      }
      buf[r] = '\0';
      /* ----------------------------------- */

      iunlock(ip);       // 읽었으니 해제
      iput(ip);

      safestrcpy(path, buf, MAXPATH);    // 새 경로로 반복
      goto resolve_again;
    }
    /* 여기서 ip 는 lock 보유 & symlink 아님 */
  }

  /* ---------- 디바이스 노드 검사 ---------- */
  if(ip->type == T_DEVICE &&
     (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip); end_op(); return -1;
  }

  /* ---------- file 구조체 / FD 할당 ---------- */
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f) fileclose(f);
    iunlockput(ip); end_op(); return -1;
  }

  if(ip->type == T_DEVICE){
    f->type  = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type  = FD_INODE;
    f->off   = 0;
  }
  f->ip       = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE)
    itrunc(ip);

  iunlock(ip);          // 참조는 fileclose()때 해제
  end_op();
  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 ||
     (ip = create(path, T_DIR, 0, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if(argstr(0, path, MAXPATH) < 0 ||
     (ip = create(path, T_DEVICE, major, minor, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i = 0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv + sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return ret;

bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();
  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray + sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;
  if(argstr(0, target, MAXPATH) < 0 ||
    argstr(1, path,   MAXPATH) < 0)
    return -1;
  begin_op();
  ip = create(path, T_SYMLINK, 0, 0, target);
  if(ip == 0){
    end_op();
    return -1;
  }
  // create() 가 잡아둔 lock / ref 해제
  iunlockput(ip);
  end_op();
  return 0;
}