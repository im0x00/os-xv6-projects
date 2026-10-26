// On-disk and in-memory file metadata (stat) definitions.

#ifndef XV6_STAT_H
#define XV6_STAT_H

#define T_UNALLOC  0   // Unallocated inode
#define T_DIR      1   // Directory
#define T_FILE     2   // Regular file
#define T_DEVICE   3   // Device file
#define T_SYMLINK  4   // Symbolic link

struct stat {
  int    dev;     // File system's disk device
  uint   ino;     // Inode number
  short  type;    // File type (T_DIR, T_FILE, T_DEVICE, T_SYMLINK)
  short  nlink;   // Number of links to this inode
  uint64 size;    // Size of file in bytes
};

#endif // XV6_STAT_H