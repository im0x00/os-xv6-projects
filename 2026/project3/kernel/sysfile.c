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
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
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
  if((fd=fdalloc(f)) < 0)
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

// Create the path new as a link to the same inode as old.
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

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
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

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
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
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
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
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
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
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
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

  int ret = kexec(path, argv);

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
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

static int
vmaconflict(struct proc *p, uint64 addr, uint64 len)
{
  for(int i = 0; i < NVMA; i++){
    struct vma *v = &p->vmas[i];
    if(!v->used)
      continue;
    if(addr < v->addr + v->length && v->addr < addr + len)
      return 1;
  }
  return 0;
}

uint64
sys_mmap(void)
{
  int len, prot, flags, fd, offset;
  struct file *f;
  struct proc *p = myproc();

  argint(0, &len);
  argint(1, &prot);
  argint(2, &flags);
  argint(4, &offset);
  if (argfd(3, &fd, &f) < 0)
    return -1;

  // 1. validate args 
  if (len <= 0 || len % PGSIZE != 0)
    return -1;
  if (prot == 0)
    return -1;
  if (prot & ~(PROT_READ | PROT_WRITE))
    return -1;
  if (flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;
  if ((prot & PROT_READ) && !f->readable)
    return -1;
  if ((prot & PROT_WRITE) && flags == MAP_SHARED && !f->writable)
    return -1;
  if (offset != 0)
    return -1;

  // 2. find a free VMA slot  
  struct vma *vv;
  for(vv = p->vmas; vv < &p->vmas[NVMA]; vv++){
    if(!vv->used)
      break;
  }
  if(vv == &p->vmas[NVMA])
    return -1;

  // 3. pick an unused VA below TRAPFRAME, above p->sz 
  uint64 va_mmap = 0;
  uint64 heap_top = PGROUNDUP(p->sz);
  for(uint64 a = TRAPFRAME - len; ; a -= PGSIZE){
    if(a < heap_top)
      break;

    if(!vmaconflict(p, a, len)){
      va_mmap = a;
      break;
    }

    if(a < PGSIZE)
      break;
  }

  if(va_mmap == 0)
    return -1;

  // 4. fill in the VMA and return its addr
  vv->addr = va_mmap;
  vv->f = filedup(f);
  vv->flags = flags;
  vv->length = len;
  vv->offset = offset;
  vv->prot = prot;
  vv->used = 1;

  return va_mmap;
}



int
do_munmap(uint64 addr, int len)
{
  if(len <= 0 || addr % PGSIZE || len % PGSIZE)
    return -1;
  uint64 end = addr + len;
  if(end < addr)
    return -1;

  struct proc *p = myproc();
  struct vma *v = 0;

  // 1. Write back the data to the file if VMA is MAP_SHARED and free the physical page and clear the PTE.
  for (struct vma *vv = p->vmas; vv < &p->vmas[NVMA]; vv++) {
    if (!vv->used)
      continue;
    if(vv->addr <= addr && end <= vv->addr + vv->length) {
      v = vv;
      break;
    }
  }

  if(v == 0)
    return -1;

  uint64 vend = v->addr + v->length;

  if(addr != v->addr && end != vend) // 가운데 뚫는거 금지
    return -1;
  if (v->flags & MAP_SHARED && v->prot & PROT_WRITE) {
    for(uint64 a = addr; a < end; a += PGSIZE){
      if(ismapped(p->pagetable, a)){ // 여기 write-back
        filewrite(v->f, a, PGSIZE);
      }
    }
  }

  uvmunmap(p->pagetable, addr, len / PGSIZE, 1);

  // 2. Adjust the VMA’s addr, offset, length.
  if(addr == v->addr && len == v->length){ 
    fileclose(v->f); // Close the file and free the VMA slot if whole VMA gone
    v->used = 0;
    v->addr = 0;
    v->length = 0;
    v->offset = 0;
    v->prot = 0;
    v->flags = 0;
    v->f = 0;
  } else if(addr == v->addr){
    // 앞부분 unmap
    v->addr += len;
    v->offset += len;
    v->length -= len;
  } else {
    // 뒷부분 unmap
    v->length -= len;
  }

  return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int len;

  argaddr(0, &addr);
  argint(1, &len);

  return do_munmap(addr, len);
}

int
mmapfault(uint64 va)
{
  struct proc *p = myproc();
  struct vma *v = 0;
  uint64 va0 = PGROUNDDOWN(va);

  // find the vma which cover the va
  for(struct vma *vv = p->vmas; vv < &p->vmas[NVMA]; vv++){
    if (vv->used && vv->addr <= va0 && va0 < vv->addr + vv->length) {
      v = vv;
      break;
    } 
  }
  if (v == 0)
    return -1;
  if (ismapped(p->pagetable, va0))
    return -1;

  // Allocate a physical page and initialize it with zeros.
  void *pa = kalloc();
  if (pa == 0)
    return -1;
  memset(pa, 0, PGSIZE);

  // Read the file data into the allocated page.
  ilock(v->f->ip);
  int r = readi(v->f->ip, 0, (uint64)pa, v->offset + (va0 - v->addr), PGSIZE);
  iunlock(v->f->ip);
  if(r < 0){
    kfree(pa);
    return -1;
  }

  // Translate prot to PTE permission bits, and map the physical page to the va.
  int perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;

  // Return the physical address mapped to va.
  if(mappages(p->pagetable, va0, PGSIZE, (uint64)pa, perm) < 0){
    kfree(pa);
    return -1;
  }

  return 0;
}
