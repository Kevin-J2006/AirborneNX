#ifndef __REIMPL_IO_H__
#define __REIMPL_IO_H__

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

const char *translate_path(const char *path, char *buffer, size_t buf_size);

FILE *wrap_fopen(const char *path, const char *mode);
int wrap_open(const char *path, int flags, ...);
int wrap_close(int fd);
ssize_t wrap_read(int fd, void *buf, size_t count);
ssize_t wrap_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t wrap_write(int fd, const void *buf, size_t count);
off_t wrap_lseek(int fd, off_t offset, int whence);
FILE *wrap_fdopen(int fd, const char *mode);
const char *io_inflight(unsigned *seconds);
void io_read_stats(unsigned long *calls, unsigned long *bytes, unsigned long *ipc);
int wrap_stat(const char *path, void *buf);
int wrap_fstat(int fd, void *buf);
int wrap_access(const char *path, int mode);
int wrap_mkdir(const char *path, mode_t mode);
void *wrap_opendir(const char *name);
void *wrap_readdir(void *dirp);
int wrap_closedir(void *dirp);
int wrap_chdir(const char *path);
char *wrap_getcwd(char *buf, size_t size);
int wrap_unlink(const char *pathname);
int wrap_remove(const char *pathname);
int wrap_rename(const char *oldpath, const char *newpath);
int wrap_statfs(const char *path, void *buf);
void *wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int wrap_munmap(void *addr, size_t length);

#endif // __REIMPL_IO_H__
