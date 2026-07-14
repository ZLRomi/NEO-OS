#ifndef PROCFS_H
#define PROCFS_H

int procfs_init(void);
int procfs_read(const char* path, char* buf, int max_len);
int procfs_readdir(const char* path, char* buf, int max_len);

#endif
