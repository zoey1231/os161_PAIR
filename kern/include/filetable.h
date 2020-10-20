#ifndef _FILETABLE_H_
#define _FILETABLE_H_

#include <types.h>
#include <vnode.h>
#include <limits.h>
#include <synch.h>
#include <array.h>
// entry of a file table
struct file
{
    struct vnode *vn;
    unsigned status;
    unsigned valid;
    off_t offset;
    unsigned refcount;

    struct lock *file_lock;
};

/*filetable array*/
struct filetable
{
    struct array *entrys;
    struct lock *ft_lock;
};
extern struct filetable filetable;

struct fd_entry
{
    unsigned int fd;
    struct file *file;
};

/* array to keep track of fd of each file in the file table*/
struct fileDescriptorArray
{
    struct array *fdArray;
    struct lock *fda_lock;
};
struct fileDescriptorArray *fdArray_create(void);
struct filetable *filetable_create(void);
void filetable_destory(struct filetable *ft);
int filetable_add(struct filetable *ft, struct file *f);
struct fd_entry *fd_get(struct array *arr, unsigned fd, int *index);
void filetable_remove(struct filetable *ft, unsigned fd);
#endif
