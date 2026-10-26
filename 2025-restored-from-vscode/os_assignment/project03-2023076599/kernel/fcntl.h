// File control options for sys_open, open(), etc.

#ifndef XV6_FCNTL_H
#define XV6_FCNTL_H

#define O_RDONLY    0x000  // Open for read only
#define O_WRONLY    0x001  // Open for write only
#define O_RDWR      0x002  // Open for read & write
#define O_CREATE    0x200  // Create file if it does not exist
#define O_TRUNC     0x400  // Truncate file upon open
#define O_NOFOLLOW  0x100  // Do not follow symbolic links when opening

#endif // XV6_FCNTL_H