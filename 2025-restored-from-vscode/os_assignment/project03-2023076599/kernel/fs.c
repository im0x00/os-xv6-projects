// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// ‑‑‑‑‑‑‑‑‑‑ Block‑device helper functions ‑‑‑‑‑‑‑‑‑‑

static void readsb(int dev, struct superblock *sb)
{
  struct buf *bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

void fsinit(int dev)
{
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

static void bzero(int dev, int bno)
{
  struct buf *bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// ‑‑‑‑‑‑‑‑‑‑ Block allocator ‑‑‑‑‑‑‑‑‑‑

static uint balloc(uint dev)
{
  for(int b = 0; b < sb.size; b += BPB){
    struct buf *bp = bread(dev, BBLOCK(b, sb));
    for(int bi = 0; bi < BPB && b + bi < sb.size; bi++){
      int m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){
        bp->data[bi/8] |= m;
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

static void bfree(int dev, uint b)
{
  struct buf *bp = bread(dev, BBLOCK(b, sb));
  int bi = b % BPB;
  int m  = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// ‑‑‑‑‑‑‑‑‑‑ In‑memory inode table ‑‑‑‑‑‑‑‑‑‑

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void iinit(void)
{
  initlock(&itable.lock, "itable");
  for(int i = 0; i < NINODE; i++)
    initsleeplock(&itable.inode[i].lock, "inode");
}

static struct inode *iget(uint dev, uint inum);

// ‑‑‑‑‑‑‑‑‑‑ Inode allocation & update ‑‑‑‑‑‑‑‑‑‑

struct inode *ialloc(uint dev, short type)
{
  for(int inum = 1; inum < sb.ninodes; inum++){
    struct buf *bp = bread(dev, IBLOCK(inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

void iupdate(struct inode *ip)
{
  struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  struct dinode *dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type  = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size  = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// ‑‑‑‑‑‑‑‑‑‑ iget / idup / ilock / … ‑‑‑‑‑‑‑‑‑‑

static struct inode *iget(uint dev, uint inum)
{
  struct inode *ip, *empty = 0;

  acquire(&itable.lock);
  for(ip = itable.inode; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)
      empty = ip;
  }
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev   = dev;
  ip->inum  = inum;
  ip->ref   = 1;
  ip->valid = 0;
  release(&itable.lock);
  return ip;
}

struct inode *idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

void ilock(struct inode *ip)
{
  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);
  if(ip->valid == 0){
    struct buf *bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    struct dinode *dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type  = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size  = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

void iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");
  releasesleep(&ip->lock);
}

void iput(struct inode *ip)
{
  acquire(&itable.lock);
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    acquiresleep(&ip->lock);
    release(&itable.lock);

    itrunc(ip);
    ip->type  = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);
    acquire(&itable.lock);
  }
  ip->ref--;
  release(&itable.lock);
}

void iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// ‑‑‑‑‑‑‑‑‑‑ bmap (direct / indirect / double‑indirect) ‑‑‑‑‑‑‑‑‑‑

static uint bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  // direct blocks
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      if((addr = balloc(ip->dev)) == 0) return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  // singly‑indirect
  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0){
      if((addr = balloc(ip->dev)) == 0) return 0;
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);
    a  = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      if((addr = balloc(ip->dev)))
        a[bn] = addr, log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  // doubly‑indirect
  bn       -= NINDIRECT;
  uint i1   = bn / NINDIRECT;
  uint i0   = bn % NINDIRECT;

  if((addr = ip->addrs[NDIRECT+1]) == 0){
    if((addr = balloc(ip->dev)) == 0) return 0;
    ip->addrs[NDIRECT+1] = addr;
  }
  bp = bread(ip->dev, addr);
  a  = (uint*)bp->data;
  if((addr = a[i1]) == 0){
    if((addr = balloc(ip->dev)))
      a[i1] = addr, log_write(bp);
  }
  brelse(bp);
  if(addr == 0) return 0;

  bp = bread(ip->dev, addr);
  a  = (uint*)bp->data;
  if((addr = a[i0]) == 0){
    if((addr = balloc(ip->dev)))
      a[i0] = addr, log_write(bp);
  }
  brelse(bp);
  return addr;
}

// ‑‑‑‑‑‑‑‑‑‑ Content helpers (itrunc / readi / writei) ‑‑‑‑‑‑‑‑‑‑

void itrunc(struct inode *ip)
{
  int i, j, k;
  struct buf *bp, *bp1;
  uint *a1, *a0;

  // direct
  for(i = 0; i < NDIRECT; i++)
    if(ip->addrs[i])
      bfree(ip->dev, ip->addrs[i]), ip->addrs[i] = 0;

  // singly‑indirect
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a1 = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++)
      if(a1[j]) bfree(ip->dev, a1[j]);
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  // doubly‑indirect
  if(ip->addrs[NDIRECT+1]){
    bp  = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a1  = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++)
      if(a1[i]){
        bp1 = bread(ip->dev, a1[i]);
        a0  = (uint*)bp1->data;
        for(k = 0; k < NINDIRECT; k++)
          if(a0[k]) bfree(ip->dev, a0[k]);
        brelse(bp1);
        bfree(ip->dev, a1[i]);
      }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }
  ip->size = 0;
  iupdate(ip);
}

void stati(struct inode *ip, struct stat *st)
{
  st->dev   = ip->dev;
  st->ino   = ip->inum;
  st->type  = ip->type;
  st->nlink = ip->nlink;
  st->size  = ip->size;
}

int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  if(ip->type != T_FILE && ip->type != T_DIR && ip->type != T_SYMLINK)
    panic("readi");

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  uint tot = 0, m;
  while(tot < n){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0) break;
    struct buf *bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1){
      brelse(bp);
      return -1;
    }
    brelse(bp);
    tot += m; off += m; dst += m;
  }
  return tot;
}

int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  if(ip->type != T_FILE && ip->type != T_DIR && ip->type != T_SYMLINK)
    panic("writei");

  if(off > ip->size || off + n < off)   return -1;
  if(off + n > MAXFILE*BSIZE)           return -1;

  uint tot = 0, m;
  while(tot < n){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0) break;
    struct buf *bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1){
      brelse(bp);
      return -1;
    }
    log_write(bp);
    brelse(bp);
    tot += m; off += m; src += m;
  }

  if(off > ip->size) ip->size = off;
  iupdate(ip);
  return tot;
}

// ‑‑‑‑‑‑‑‑‑‑ Directory helpers ‑‑‑‑‑‑‑‑‑‑

int namecmp(const char *s, const char *t){ return strncmp(s, t, DIRSIZ); }

struct inode *dirlookup(struct inode *dp, char *name, uint *poff)
{
  if(dp->type != T_DIR) panic("dirlookup not DIR");
  struct dirent de;
  for(uint off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum && namecmp(name, de.name) == 0){
      if(poff) *poff = off;
      return iget(dp->dev, de.inum);
    }
  }
  return 0;
}

int dirlink(struct inode *dp, char *name, uint inum)
{
  if(dirlookup(dp, name, 0) != 0) return -1;  // already exists

  struct dirent de;
  uint off;
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0) break;
  }
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;
  return 0;
}

// ‑‑‑‑‑‑‑‑‑‑ Pathname‑walker (namex / namei / …) ‑‑‑‑‑‑‑‑‑‑

static char *skipelem(char *path, char *name)
{
  while(*path == '/') path++;
  if(*path == 0) return 0;
  char *s = path;
  while(*path && *path != '/') path++;
  int len = path - s;
  if(len >= DIRSIZ) memmove(name, s, DIRSIZ);
  else               { memmove(name, s, len); name[len] = 0; }
  while(*path == '/') path++;
  return path;
}

static struct inode *namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  int depth = 0;

  ip = (*path == '/') ? iget(ROOTDEV, ROOTINO) : idup(myproc()->cwd);
  while((path = skipelem(path, name)) != 0){
  restart:
    ilock(ip);
    // follow up to 10 symlinks
    while(ip->type == T_SYMLINK && depth < 10){
      char buf[MAXPATH];
      int m = readi(ip, 0, (uint64)buf, 0, sizeof(buf)-1);
      if(m <= 0){ iunlockput(ip); return 0; }
      buf[m] = 0;            // ensure NUL‑terminated string
      iunlockput(ip);
      depth++;
      ip = (buf[0] == '/') ? iget(ROOTDEV, ROOTINO) : idup(myproc()->cwd);
      path = buf;
      goto restart;
    }
    if(ip->type != T_DIR){ iunlockput(ip); return 0; }
    if(nameiparent && *path == '\0'){ iunlock(ip); return ip; }
    if((next = dirlookup(ip, name, 0)) == 0){ iunlockput(ip); return 0; }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){ iput(ip); return 0; }
  return ip;
}

struct inode *namei(char *path){ char name[DIRSIZ]; return namex(path, 0, name); }
struct inode *nameiparent(char *path, char *name){ return namex(path, 1, name); }
