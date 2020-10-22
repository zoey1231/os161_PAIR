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
#include <kern/seek.h>
#include <kern/stattypes.h>

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
    struct fd_entry *fe;
    unsigned int fd = 0;

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

    lock_acquire(curproc->p_fdArray->fda_lock);
    if (array_num(curproc->p_fdArray->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    while (fd_get(curproc->p_fdArray->fdArray, fd, NULL) != NULL)
    {
        fd += 1;
    }
    // Open file and grab the vnode associated with it
    err = vfs_open(path, flags, 0, &vn);

    kfree(path);
    kfree(path_len);

    if (err)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }
    file = kmalloc(sizeof(struct file));
    if (file == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
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
            lock_release(curproc->p_fdArray->fda_lock);

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
    fe = kmalloc(sizeof(struct fd_entry));
    if (fe == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);

        return EMFILE;
    }
    fe->fd = fd;
    fe->file = file;
    *retVal = fd;
    lock_acquire(curproc->p_filetable->ft_lock);
    err = filetable_add(curproc->p_filetable, file);
    if (err)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }
    err = array_add(curproc->p_fdArray->fdArray, fe, NULL);
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(curproc->p_filetable->ft_lock);
        return err;
    }

    lock_release(curproc->p_fdArray->fda_lock);
    lock_release(curproc->p_filetable->ft_lock);
    return 0;
}

/**
 * Close the file handle fd. The same file handle may then be returned again from open, dup2, pipe, 
 * or similar calls. 
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_close(int fd)
{
    struct file *file;

    lock_acquire(curproc->p_fdArray->fda_lock);
    int index = -1;
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);

    //decrement the reference count of the file, if refcount is 0 after the decrement,
    //remove the file completely since there is no one has opened the file
    //otherwise, simply remove the file with fd from the filetable
    KASSERT(file->refcount >= 1);
    file->refcount -= 1;
    if (file->refcount == 0)
    {
        vfs_close(file->vn);
        file->vn = NULL;
        file->offset = 0;
        file->valid = 0;
        lock_release(file->file_lock);
        lock_destroy(file->file_lock);
        fe->file = NULL;
        array_remove(curproc->p_fdArray->fdArray, (unsigned)index);
        kfree(fe);
    }
    else
    {
        fe->file = NULL;
        array_remove(curproc->p_fdArray->fdArray, (unsigned)index);
        kfree(fe);
        lock_release(file->file_lock);
    }

    lock_release(curproc->p_fdArray->fda_lock);
    return 0;
}
/**
 * reads up to buflen bytes from the file specified by fd, at the location in the file specified by 
 * the current seek position of the file, and stores them in the space pointed to by buf. 
 * The file must be open for reading.
 * The count of bytes read is returned in retVal.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_read(int fd, userptr_t buf, size_t buflen, int *retVal)
{
    struct iovec iovec;
    struct uio uio;
    struct file *file;
    int err;

    char *kbuf = kmalloc(sizeof(char *));
    err = copyin((const_userptr_t)buf, (void *)kbuf, sizeof(char *));
    if (err)
    {
        kfree(kbuf);
        return EFAULT;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if fd was opened for reading
    if (file->status & O_WRONLY)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    //set up uio structure for read
    iovec.iov_ubase = buf;
    iovec.iov_len = buflen;
    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 1;
    uio.uio_resid = buflen;
    uio.uio_offset = file->offset;
    uio.uio_segflg = UIO_USERSPACE;
    uio.uio_rw = UIO_READ;
    uio.uio_space = curproc->p_addrspace;

    err = VOP_READ(file->vn, &uio);
    if (err)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }

    file->offset += (off_t)(buflen - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->p_fdArray->fda_lock);
    *retVal = (int)buflen - uio.uio_resid;
    kfree(kbuf);

    return 0;
}
/**
 * writes up to buflen bytes to the file specified by fd, at the location in the file specified by the current 
 * seek position of the file, taking the data from the space pointed to by buf. 
 * The file must be open for writing.
 * The count of bytes written is returned in retVal.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_write(int fd, const_userptr_t buf, size_t nbytes, int *retVal)
{
    struct file *file;
    char *kbuf = kmalloc(sizeof(char *));
    int err;
    err = copyin(buf, (void *)kbuf, sizeof(char *));

    if (err)
    {
        kfree(kbuf);
        return EFAULT;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if fd was opened for writing
    if (!(file->status & (O_WRONLY | O_RDWR)))
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    //set up uio structure for write
    struct uio uio;
    struct iovec iovec;
    iovec.iov_ubase = (userptr_t)buf;
    iovec.iov_len = nbytes;
    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 1;
    uio.uio_resid = nbytes;
    uio.uio_offset = file->offset;
    uio.uio_segflg = UIO_USERSPACE;
    uio.uio_rw = UIO_WRITE;
    uio.uio_space = curproc->p_addrspace;

    err = VOP_WRITE(file->vn, &uio);
    if (err)
    {
        lock_release(file->file_lock);
        lock_release(curproc->p_fdArray->fda_lock);
        return err;
    }

    file->offset += (off_t)(nbytes - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->p_fdArray->fda_lock);
    *retVal = (int)nbytes - uio.uio_resid;
    kfree(kbuf);

    return 0;
}
/**
 * alter the current seek position of the file with file handle fd, seeking to a new position based on 
 * pos and whence.
 * On success,return the new position in retVal.
 * On error, the corresponding error code is returned.
 */
int sys_lseek(int fd, off_t pos, userptr_t whence, int64_t *retVal)
{
    int err;
    struct stat *stat;
    off_t new_pos;
    int k_whence = 0;
    err = copyin((const_userptr_t)whence, &k_whence, sizeof(int32_t));
    if (err)
    {
        return EINVAL;
    }

    if (!(k_whence == SEEK_SET || k_whence == SEEK_CUR || k_whence == SEEK_END))
    {
        return EINVAL;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }
    lock_release(curproc->p_fdArray->fda_lock);

    struct file *file = fe->file;
    lock_acquire(file->file_lock);
    if (!file->valid)
    {
        lock_release(file->file_lock);
        return EBADF;
    }

    //check if object is seekable. i.e., is not console device etc.
    if (!VOP_ISSEEKABLE(file->vn))
    {
        lock_release(file->file_lock);
        return ESPIPE;
    }
    //seeking to a new position based on pos and whence
    switch (k_whence)
    {
    case SEEK_SET:
        new_pos = pos;
        break;
    case SEEK_CUR:
        new_pos = file->offset + pos;
        break;
    case SEEK_END:
        //get the file's original size
        stat = kmalloc(sizeof(struct stat));
        KASSERT(stat != NULL);
        VOP_STAT(file->vn, stat);
        new_pos = stat->st_size + pos;
        kfree(stat);
        break;
    }

    //Seek positions < zero are invalid. Seek positions beyond EOF are legal
    if (new_pos < 0)
    {
        lock_release(file->file_lock);
        return EINVAL;
    }

    file->offset = new_pos;
    lock_release(file->file_lock);
    *retVal = new_pos;

    return 0;
}
/**
 * dup2 clones the file handle oldfd onto the file handle newfd. 
 * The two handles refer to the same object and share the same seek pointer.
 * On success,return newfd in retVal.
 * On error, the corresponding error code is returned.
 */
int sys_dup2(int oldfd, int newfd, int *retVal)
{
    if (newfd < 0 || oldfd < 0 || newfd >= OPEN_MAX || oldfd >= OPEN_MAX)
        return EBADF;

    lock_acquire(curproc->p_fdArray->fda_lock);

    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, oldfd, NULL);

    /* check if the file that oldfd points to exists and valid */
    if (fe == NULL || fe->file == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }
    if (!fe->file->valid)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EBADF;
    }

    // return the existing file if oldfd is the same as the newfd
    if (oldfd == newfd)
    {
        *retVal = newfd;
        lock_release(curproc->p_fdArray->fda_lock);
        return 0;
    }

    struct fd_entry *newfd_fe = fd_get(curproc->p_fdArray->fdArray, newfd, NULL);
    // check if file that newfd points to exists, kill it if it does
    if (newfd_fe != NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        sys_close(newfd);
        lock_acquire(curproc->p_fdArray->fda_lock);
    }

    if (array_num(curproc->p_fdArray->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    struct fd_entry *new_fe = kmalloc(sizeof(struct fd_entry));
    if (new_fe == NULL)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        return EMFILE;
    }

    new_fe->fd = newfd;
    new_fe->file = fe->file;

    lock_acquire(fe->file->file_lock);
    fe->file->refcount++;
    lock_release(fe->file->file_lock);

    array_add(curproc->p_fdArray->fdArray, new_fe, NULL);
    *retVal = newfd;
    lock_release(curproc->p_fdArray->fda_lock);
    return 0;
}
/**
 * The current directory of the current process is set to the directory named by pathname.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
int sys_chdir(const char *pathname)
{
    if (!pathname)
    {
        return EFAULT;
    }

    char *path;
    path = kmalloc(PATH_MAX);
    size_t *path_len = kmalloc(sizeof(int));

    // Copy the filename into kernel space's address path
    int err = copyinstr((const_userptr_t)pathname, path, PATH_MAX, path_len);
    if (err)
    {
        kfree(path);
        kfree(path_len);
        return err;
    }

    kfree(path_len);

    err = vfs_chdir(path);
    if (err)
    {
        kfree(path);
        return err;
    }
    else
    {
        kfree(path);
        return 0;
    }
}
/**
 * Stores the current working directory name in buf, an area of size buflen.
 * On success, returns the actural length of the data returned in retVal.
 * On error, the corresponding error code is returned.
 */
int sys___getcwd(char *buf, size_t buflen, int *retVal)
{
    int err;
    struct uio uio;
    struct iovec iovec;

    if (!buf)
        return EFAULT;
    char *temp_buf = kmalloc(sizeof(char *));

    //initialize an uio structure to get cwd
    uio_kinit(&iovec, &uio, temp_buf, buflen, 0, UIO_READ);

    err = vfs_getcwd(&uio);
    if (err)
    {
        return err;
    }

    //copy the the path from kernel to user space
    err = copyout((const void *)temp_buf, (userptr_t)buf, (size_t)(sizeof(char *)));
    kfree(temp_buf);

    if (err)
        return err;

    *retVal = buflen - uio.uio_resid;
    return 0;
}
