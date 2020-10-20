#include <types.h>
#include <vfs.h>
#include <current.h>
#include <syscall.h>
#include <filetable.h>
#include <proc.h>
#include <uio.h>
#include <vnode.h>
#include <lib.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <kern/errno.h>
#include <vm.h>
#include <array.h>
#include <synch.h>
#include <copyinout.h>
#include <limits.h>

/*
 * opens the file, device, or other kernel object named by the pathname filename. 
 * The flags argument specifies how to open the file.
 * On success, open returns a nonnegative file descriptor at retVal. 
 * On error, the corresponding error code is returned.
*/
int sys_open(const userptr_t filename, int flags, unsigned int *retVal)
{
    struct vnode *vn;
    char *path;
    size_t *path_len;
    int err;
    struct file *file;
    unsigned int *fd;

    fd = kmalloc(sizeof(unsigned int));
    path = kmalloc(PATH_MAX);
    path_len = kmalloc(sizeof(int));

    // Copy the filename into kernel space's address path
    err = copyinstr((const_userptr_t)filename, path, PATH_MAX, path_len);
    if (err)
    {
        kfree(path);
        kfree(path_len);
        return err;
    }

    //check if too many files have opened already
    lock_acquire(curproc->p_filetable->ft_lock);
    if (array_num(curproc->p_filetable->entrys) >= OPEN_MAX)
    {
        lock_release(curproc->p_filetable->ft_lock);
        return EMFILE;
    }

    // Open file and grab the vnode associated with it
    err = vfs_open(path, flags, 0, &vn);

    kfree(path);
    kfree(path_len);

    if (err)
    {
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }
    file = kmalloc(sizeof(struct file));
    if (file == NULL)
    {
        lock_release(curproc->p_filetable->ft_lock);
        return ENOMEM;
    }
    file->refcount = 1;
    file->file_lock = lock_create("file_lock");
    file->status = flags;
    file->vn = vn;
    file->valid = 1;

    //since O_APPEND causes all writes to the file to occur at the end of file, set file's offset to file's size
    if (flags & O_APPEND)
    {
        struct stat *stat;
        stat = kmalloc(sizeof(struct stat));
        if (stat == NULL)
        {
            lock_release(curproc->p_filetable->ft_lock);
            return ENOMEM;
        }
        VOP_STAT(vn, stat);
        file->offset = stat->st_size;
    }
    else
    {
        file->offset = 0;
    }

    //add the file's kernel representation to the process's file table
    err = filetable_add(curproc->p_filetable, file, fd);
    if (err)
    {
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }
    *retVal = *fd;
    kfree(fd);
    lock_release(curproc->p_filetable->ft_lock);
    return 0;
}

/**
 * Close the file handle fd. The same file handle may then be returned again from open, dup2, pipe, 
 * or similar calls. Other file handles are not affected in any way, even if they are attached to the 
 * same file.
 * On success, open returns a nonnegative file descriptor at retVal. 
 * On error, the corresponding error code is returned.
 */
int sys_close(int fd)
{
    struct file *file;

    lock_acquire(curproc->p_filetable->ft_lock);

    file = filetable_get(curproc->p_filetable, fd);
    if (file == NULL)
    {
        lock_release(curproc->p_filetable->ft_lock);
        return EBADF;
    }
    lock_acquire(file->file_lock);

    //decrement the reference count of the file, if refcount is 0 after the decrement,
    //remove the file completely since there is no one has opened the file
    //otherwise, simply remove the file with fd from the filetable
    KASSERT(file->refcount >= 1);
    file->refcount -= 1;
    if (file->refcount == 0)
    {
        vfs_close(file->vn);
        file->valid = 0;
        file->vn = NULL;
        file->offset = 0;
        lock_release(file->file_lock);
        lock_destroy(file->file_lock);
        filetable_remove(curproc->p_filetable, fd);
        kfree(file);
    }
    else
    {
        filetable_remove(curproc->p_filetable, fd);
        lock_release(file->file_lock);
    }

    lock_release(curproc->p_filetable->ft_lock);
    return 0;
}
//TODO:
int sys_read(int fd, userptr_t buf, size_t buflen, int *retVal)
{
    (void)fd;
    (void)buf;
    (void)buflen;
    (void)retVal;
    return 0;
}
int sys_write(int fd, userptr_t buf, size_t nbytes, int *retVal)
{
    (void)fd;
    (void)buf;
    (void)nbytes;
    (void)retVal;
    return 0;
}
int sys_lseek(int fd, off_t pos, userptr_t whence, int64_t *retVal)
{
    (void)fd;
    (void)pos;
    (void)whence;
    (void)retVal;
    return 0;
}

int sys_dup2(int oldfd, int newfd, int *retVal)
{
    (void)oldfd;
    (void)newfd;
    (void)retVal;
    return 0;
}
int sys_chdir(const char *pathname, int *retVal)
{
    (void)pathname;
    (void)retVal;
    return 0;
}
int sys___getcwd(char *buf, size_t buflen, int *retVal)
{
    (void)buf;
    (void)buflen;
    (void)retVal;
    return 0;
}
// /*
// Writes the data from buf up to buflen bytes to the file at fd, at the
// current seek position. The file must be open for writing.
// */
// int sys_write(int fd, const void *buf, size_t nbytes, int32_t *retval0)
// {
//     struct ft *ft = curproc->proc_ft;
//     struct iovec iov;
//     struct uio u;
//     int result;
//     struct ft_entry *entry;

//     lock_acquire(ft->ft_lock);
//     if (!fd_valid_and_used(ft, fd))
//     {
//         lock_release(ft->ft_lock);
//         return EBADF;
//     }

//     entry = ft->entries[fd];
//     lock_acquire(entry->entry_lock);

//     lock_release(ft->ft_lock);

//     if (!(entry->rwflags & (O_WRONLY | O_RDWR)))
//     {
//         lock_release(entry->entry_lock);
//         return EBADF;
//     }

//     iov.iov_ubase = (userptr_t)buf;
//     iov.iov_len = nbytes;
//     u.uio_iov = &iov;
//     u.uio_iovcnt = 1;
//     u.uio_resid = nbytes;
//     u.uio_offset = entry->offset;
//     u.uio_segflg = UIO_USERSPACE;
//     u.uio_rw = UIO_WRITE;
//     u.uio_space = curproc->p_addrspace;

//     result = VOP_WRITE(entry->file, &u);
//     if (result)
//     {
//         lock_release(entry->entry_lock);
//         return result;
//     }

//     ssize_t len = nbytes - u.uio_resid;
//     entry->offset += (off_t)len;

//     lock_release(entry->entry_lock);
//     *retval0 = len;

//     return 0;
// }

// /*
// Reads to buf up to buflen bytes from the file at fd,
// at current seek position. The file must be open for reading.
// */
// int sys_read(int fd, void *buf, size_t buflen, ssize_t *retval0)
// {
//     struct ft *ft = curproc->proc_ft;
//     struct iovec iov;
//     struct uio u;
//     int result;
//     struct ft_entry *entry;

//     lock_acquire(ft->ft_lock);
//     if (!fd_valid_and_used(ft, fd))
//     {
//         lock_release(ft->ft_lock);
//         return EBADF;
//     }

//     entry = ft->entries[fd];
//     lock_acquire(entry->entry_lock);

//     lock_release(ft->ft_lock);

//     if (entry->rwflags & O_WRONLY)
//     {
//         lock_release(entry->entry_lock);
//         return EBADF;
//     }

//     iov.iov_ubase = (userptr_t)buf;
//     iov.iov_len = buflen;
//     u.uio_iov = &iov;
//     u.uio_iovcnt = 1;
//     u.uio_resid = buflen;
//     u.uio_offset = entry->offset;
//     u.uio_segflg = UIO_USERSPACE;
//     u.uio_rw = UIO_READ;
//     u.uio_space = curproc->p_addrspace;

//     result = VOP_READ(entry->file, &u);
//     if (result)
//     {
//         lock_release(entry->entry_lock);
//         return result;
//     }

//     ssize_t len = buflen - u.uio_resid;
//     entry->offset += (off_t)len;

//     lock_release(entry->entry_lock);
//     *retval0 = len;

//     return 0;
// }

// /*
// Sets the file's seek position according to pos and whence.
// */
// int sys_lseek(int fd, off_t pos, int whence, int32_t *retval0, int32_t *retval1)
// {

//     if (whence < 0 || whence > 2)
//     {
//         return EINVAL;
//     }

//     struct ft *ft = curproc->proc_ft;
//     struct ft_entry *entry;
//     struct stat *stat;
//     off_t eof;
//     off_t seek;

//     lock_acquire(ft->ft_lock);
//     if (!fd_valid_and_used(ft, fd))
//     {
//         lock_release(ft->ft_lock);
//         return EBADF;
//     }

//     entry = ft->entries[fd];
//     lock_acquire(entry->entry_lock);

//     lock_release(ft->ft_lock);

//     if (!VOP_ISSEEKABLE(entry->file))
//     {
//         lock_release(entry->entry_lock);
//         return ESPIPE;
//     }

//     stat = kmalloc(sizeof(struct stat));
//     VOP_STAT(entry->file, stat);
//     eof = stat->st_size;

//     seek = entry->offset;

//     switch (whence)
//     {
//     case SEEK_SET:
//         seek = pos;
//         break;
//     case SEEK_CUR:
//         seek += pos;
//         break;
//     case SEEK_END:
//         seek = eof + pos;
//         break;
//     }

//     if (seek < 0)
//     {
//         lock_release(entry->entry_lock);
//         return EINVAL;
//     }

//     entry->offset = seek;

//     lock_release(entry->entry_lock);
//     *retval0 = seek >> 32;
//     *retval1 = seek & 0xFFFFFFFF;

//     return 0;
// }

// /*
// Clones the instance of the open file at oldfd to newfd.
// If there is an open file at newfd, it is closed.
// */
// int sys_dup2(int oldfd, int newfd, int *output)
// {
//     if (newfd < 0 || oldfd < 0 || newfd >= OPEN_MAX || oldfd >= OPEN_MAX)
//     {
//         return EBADF;
//     }

//     struct ft *ft = curproc->proc_ft;
//     struct ft_entry *entry;

//     lock_acquire(ft->ft_lock);

//     if (!fd_valid_and_used(ft, oldfd))
//     {
//         lock_release(ft->ft_lock);
//         return EBADF;
//     }

//     entry = ft->entries[oldfd];

//     if (fd_valid_and_used(ft, newfd))
//     {
//         free_fd(ft, newfd);
//     }

//     assign_fd(ft, entry, newfd);
//     lock_release(ft->ft_lock);
//     *output = newfd;

//     return 0;
// }

// /*
// Changes the current working directory.
// */
// int sys_chdir(const char *pathname)
// {
//     char *path;
//     size_t *path_len;
//     int err;

//     path = kmalloc(PATH_MAX);
//     path_len = kmalloc(sizeof(int));

//     /* Copy the string from userspace to kernel space and check for valid address */
//     err = copyinstr((const_userptr_t)pathname, path, PATH_MAX, path_len);

//     if (err)
//     {
//         kfree(path);
//         kfree(path_len);
//         return err;
//     }

//     int result = vfs_chdir((char *)pathname);

//     kfree(path);
//     kfree(path_len);

//     if (result)
//     {
//         return result;
//     }

//     return 0;
// }

// /*
// Stores the current working directory at buf.
// */
// int sys___getcwd(char *buf, size_t buflen, int32_t *output)
// {
//     struct iovec iov;
//     struct uio u;
//     int result;

//     iov.iov_ubase = (userptr_t)buf;
//     iov.iov_len = buflen;
//     u.uio_iov = &iov;
//     u.uio_iovcnt = 1;
//     u.uio_resid = buflen;
//     u.uio_offset = 0;
//     u.uio_segflg = UIO_USERSPACE;
//     u.uio_rw = UIO_READ;
//     u.uio_space = curproc->p_addrspace;

//     result = vfs_getcwd(&u);
//     if (result)
//     {
//         return result;
//     }

//     *output = buflen - u.uio_resid;
//     return 0;
// }
