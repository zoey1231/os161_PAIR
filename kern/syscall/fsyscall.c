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
#include <machine/trapframe.h>
#include <addrspace.h>
#include <kern/wait.h>

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

    // update fd to the number of files have opened. fd should be associated to
    // the new file after the loop
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
    lock_acquire(filetable->ft_lock);
    err = filetable_add(filetable, file);
    if (err)
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(filetable->ft_lock);
        return err;
    }
    // build file entry and save it to the fd_Array
    err = array_add(curproc->p_fdArray->fdArray, fe, NULL);
    {
        lock_release(curproc->p_fdArray->fda_lock);
        lock_release(filetable->ft_lock);
        return err;
    }

    lock_release(curproc->p_fdArray->fda_lock);
    lock_release(filetable->ft_lock);
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
        // remove file entry from the kernel representation
        array_remove(curproc->p_fdArray->fdArray, (unsigned)index);
        kfree(fe);
    }
    else
    {
        fe->file = NULL;
        // remove file entry from the kernel representation
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
    // checking to see if buf provided is in user space
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
    // checking to see if buf provided is in user space
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
    // bring in whence from userspace to kernel space
    err = copyin((const_userptr_t)whence, &k_whence, sizeof(int32_t));
    if (err)
    {
        return EINVAL;
    }

    // whence has to be either SEEK_SET or SEEK_CUR or SEEK_END. return EINVAL if anything else
    if (!(k_whence == SEEK_SET || k_whence == SEEK_CUR || k_whence == SEEK_END))
    {
        return EINVAL;
    }

    int index = -1;
    lock_acquire(curproc->p_fdArray->fda_lock);
    struct fd_entry *fe = fd_get(curproc->p_fdArray->fdArray, fd, &index);
    // return EBAD if can't find the file entry with given fd
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

/**
 * Copys the currently running process.
 * The two copies are identical except the child process has a new, unique process id.
 * The two processes do not share memory or open file tables. However, the file handle objects the file 
 * tables point to are shared.
 * On success, fork returns twice, once in the parent process and once in the child process.
 * In the child process, 0 is returned. In the parent process, the new child process's pid is returned.
 * On error, no new process is created. fork only returns once with corresponding error code.
 */
int sys_fork(struct trapframe *tf, int *retval)
{
    struct addrspace *parent_as;
    struct addrspace *child_as;

    struct proc *new_proc;
    const char *new_proc_name = curproc->p_name;
    const char *new_thread_name = curthread->t_name;

    //create a child process
    new_proc = create_fork_proc(new_proc_name);
    if (new_proc == NULL)
    {
        return ENOMEM;
    }

    // add the new process to the pid table and save the child process's pid in new_proc->pid
    int ret = pidtable_add(new_proc, &(new_proc->pid));
    if (ret)
    {
        proc_destroy(new_proc);
        return ret;
    }

    // copy address space from the parent process
    parent_as = proc_getas();
    ret = as_copy(parent_as, &child_as);
    new_proc->p_addrspace = child_as;
    if (ret)
    { // if copy is not successful, free new process
        pidtable_remove(new_proc->pid);
        proc_destroy(new_proc);
        return ret;
    }

    //make sure that the parent and the child processes have the same cwd
    spinlock_acquire(&curproc->p_lock);
    if (curproc->p_cwd != NULL)
    {
        VOP_INCREF(curproc->p_cwd);
        new_proc->p_cwd = curproc->p_cwd;
    }
    spinlock_release(&curproc->p_lock);

    // copy filetable from the parent process
    ft_copy(curproc, new_proc);

    // copy trapframe from the parent
    struct trapframe *tf_dup = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if (tf_dup == NULL)
    {
        return ENOMEM;
    }
    memcpy((void *)tf_dup, (const void *)tf, sizeof(struct trapframe));

    //return child process's pid to parent
    *retval = new_proc->pid;

    void **argv;
    argv = kmalloc(2 * sizeof(void *));
    argv[0] = tf_dup;
    argv[1] = child_as;

    //create child thread
    ret = thread_fork(new_thread_name, new_proc, enter_forked_process, argv, 2);
    if (ret)
    {
        proc_destroy(new_proc);
        pidtable_remove(new_proc->pid);
        kfree(tf_dup);
        return ret;
    }
    return 0;
}

static int
string_in(const char *user_src, char **kern_dest, size_t copy_len, size_t *actural_len)
{
    int ret;

    copy_len++;
    *kern_dest = kmalloc(copy_len * sizeof(char));

    ret = copyinstr((const_userptr_t)user_src, *kern_dest, copy_len, actural_len);
    if (ret)
    {
        kfree(*kern_dest);
        return ret;
    }

    return 0;
}

/**
 * Helper function for sys_execv()
 * Return the number of arguments in argc
 */
static int get_argc(char **args, int *argc)
{
    int ret;
    int i = 0;
    char *next_arg;

    //count the argument number one by one until we meet the null terminator, also check if there are too many arguments
    do
    {
        i++; // i starts from 1
        ret = copyin((const_userptr_t)&args[i], (void *)&next_arg, (size_t)sizeof(char *));
        if (ret)
        {
            return ret;
        }
    } while (next_arg != NULL && (i + 1) * (sizeof(char *)) <= ARG_MAX);

    if (next_arg != NULL)
    {
        return E2BIG;
    }

    *argc = i;
    return 0;
}

/**
*Copies the user strings into the kernel, populating size[] with their respective lengths.
*Returns an error if an argument's length exceeds ARG_MAX.
*/
static int
copy_in_args(int argc, char **args, char **args_copy, int *size_arr, int *strSize_total) //TODO:this implementation might not pass bigexecv test
{
    int err;
    size_t str_len, aligned_strlen;
    *strSize_total = 0;
    for (int i = 0; i < argc; i++)
    {
        err = string_in((const char *)args[i], &args_copy[i], ARG_MAX, &str_len);
        if (err)
        {
            for (int j = 0; j < i; j++)
            {
                kfree(args_copy[j]);
            }
            return err;
        }
        //align the string pointer to 4 if not already
        if (str_len % 4 != 0)
        {
            aligned_strlen = (size_t)4 * (str_len / 4) + 4;
        }
        else
        {
            aligned_strlen = str_len;
        }
        size_arr[i] = aligned_strlen;
        *strSize_total += aligned_strlen;
    }
    return 0;
}

static void free_copied_args(int argc, int *size, char **args_copy)
{
    for (int i = 0; i < argc; i++)
    {
        kfree(args_copy[i]);
    }
    kfree(size);
    kfree(args_copy);
}
static int copy_out_args(int argc, char **args_copy, int *size, int strSize_total, vaddr_t *stackptr, userptr_t *argv)
{
    size_t actual_len;
    int err;
    userptr_t argv_base_addr = (userptr_t)(*stackptr - (argc + 1) * sizeof(userptr_t *) - strSize_total * sizeof(char));
    userptr_t str_base_addr = (userptr_t)(*stackptr - strSize_total * sizeof(char));

    *stackptr = (vaddr_t)argv_base_addr;
    *argv = argv_base_addr;

    userptr_t argv_temp_ptr = argv_base_addr;
    userptr_t str_temp_ptr = str_base_addr;
    for (int i = 0; i < argc; i++)
    {

        //copy out argument string to the new stack
        err = copyoutstr(args_copy[i], str_temp_ptr, size[i], &actual_len);
        if (err)
        {
            return err;
        }
        //copy out argument's pointer to the new stack
        err = copyout(&str_temp_ptr, argv_temp_ptr, sizeof(char *));
        if (err)
        {
            return err;
        }
        //adjust pointers to point to the next argument's position on stack
        argv_temp_ptr += sizeof(userptr_t *);
        str_temp_ptr += size[i];
    }
    return 0;
}

/**
 * Replaces the currently executing program with a newly loaded program image.
 * The pathname of the program to run is passed as program. The args argument is an array of 0-terminated strings.
 * The array itself should be terminated by a NULL pointer.The argument strings should be copied into the new 
 * process as the new process's argv[] array. In the new process, argv[argc] must be NULL. 
 * By convention, argv[0] in new processes contains the name that was used to invoke the program. 
 * This is not necessarily the same as program, and furthermore is only a convention and should not be enforced by the kernel. 
 * The process file table and current working directory are not modified by execv.
 * On success, execv does not return; instead, the new program begins executing.
 * On failure, the corresponding error code is returned.
 */
int sys_execv(const char *program, char **args)
{
    int ret;
    int strSize_total;
    char *progname;
    if (program == NULL || args == NULL)
        return EFAULT; // indicates that one of the argument is an invalid pointer

    //check if program is a valid user space pointer
    char ** check_ptr=kmalloc(sizeof(void*));
    int ptr_err=copyin((const_userptr_t)program,check_ptr,sizeof(void*));
    kfree(check_ptr);
    if(ptr_err){ 
        return ptr_err;
    }

    //copy the arguments from the old address space
    size_t *path_len = kmalloc(sizeof(size_t));
    ret = string_in(program, &progname, PATH_MAX, path_len);
    if (ret)
    {
        kfree(path_len);
        return ret;
    }
    kfree(path_len);

    //get number of arguments
    int argc;
    ret = get_argc(args, &argc);
    if (ret)
    {
        kfree(progname);
        return ret;
    }

    char **args_copy = kmalloc(argc * sizeof(char *));
    if (args_copy == NULL)
    {
        kfree(progname);
        return ENOMEM;
    }
    int *size_arr = kmalloc(argc * sizeof(int));
    if (size_arr == NULL)
    {
        kfree(progname);
        return ENOMEM;
    }
    ret = copy_in_args(argc, args, args_copy, size_arr, &strSize_total);
    if (ret)
    {
        kfree(args_copy);
        kfree(size_arr);
        kfree(progname);
        return ret;
    }

    //get a new address space
    struct addrspace *as_old = proc_getas();
    struct addrspace *as_new = as_create();
    if (as_new == NULL)
    {
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ENOMEM; // insufficient virtual memory is available
    }

    struct vnode *vn;
    vaddr_t entrypoint, stackptr;
    ret = vfs_open(progname, O_RDONLY, 0, &vn);
    if (ret)
    {
        kfree(progname);
        as_destroy(as_new);
        free_copied_args(argc, size_arr, args_copy);
        return ENOMEM;
    }

    //switch to the new address space
    proc_setas(as_new);
    as_activate();

    //load a new executable
    ret = load_elf(vn, &entrypoint);
    if (ret)
    {
        //if err, switch back to old address space
        proc_setas(as_old);
        as_activate();
        as_destroy(as_new);

        vfs_close(vn);
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ret;
    }
    vfs_close(vn);
    //define a new stack region
    ret = as_define_stack(as_new, &stackptr);
    if (ret)
    {
        proc_setas(NULL);
        as_deactivate();

        proc_setas(as_old);
        as_activate();

        as_destroy(as_new);
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ret;
    }

    //copy the arguments to the new address space, properly arranging them
    userptr_t argv;
    ret = copy_out_args(argc, args_copy, size_arr, strSize_total, &stackptr, &argv);
    if (ret)
    {
        proc_setas(NULL);
        as_deactivate();

        proc_setas(as_old);
        as_activate();

        as_destroy(as_new);
        kfree(progname);
        free_copied_args(argc, size_arr, args_copy);
        return ret;
    }

    kfree(progname);
    free_copied_args(argc, size_arr, args_copy);

    //clean up the old address space and
    as_destroy(as_old);
    //wrap to user mode
    enter_new_process(argc, argv, NULL, stackptr, entrypoint);

    // enter_new_process should not return
    panic("enter_new_process returned in sys_execv\n");
    return EINVAL;
}
/**
 * Returns the process id of the current process.
 * sys_getpid does not fail.
 */
int sys_getpid(int *retval)
{
    lock_acquire(curproc->pid_lock);
    *retval = curproc->pid;
    lock_release(curproc->pid_lock);
    return 0;
}
/**
 * Cause the current process to exit. 
 * The exit code exit_code is reported back to other process(es) via the waitpid() call.
 * The process id of the exiting process should not be reused until the process's parent has 
 * collect its exit status.
 * sys__exit does not return.
 */

void sys__exit(int exit_code)
{
    lock_acquire(curproc->pid_lock);

    //check the state of each child program of proc
    //if the child has already exited,clear its pid information
    //if not, update its state to ORPHAN to indicate its parent has exited
    updateChildState(curproc);

    pid_t pid = curproc->pid;

    //if parent has already exited, clear the current process's pid information
    if (curproc->proc_state == ORPHAN)
    {
        proc_destroy(curproc);
        reset_pidtable_entry(pid);
    }
    // if parent is still running, update proc's state to ZOMBIE and record its exit code
    else if (curproc->proc_state == RUNNING)
    {
        curproc->proc_state = ZOMBIE;
        curproc->exit_code = _MKWAIT_EXIT(exit_code);
    }
    //should not branch here
    else
    {
        panic("Tried to exit a bad process.\n");
    }

    //signal processes waiting for proc to exit
    cv_broadcast(curproc->pid_cv, curproc->pid_lock);

    lock_release(curproc->pid_lock);
    thread_exit();
    panic("program should not reach here");
}

/**
 * Do the same as sys__exit except that here using the _MKWAIT_SIG macro in kern/wait.h.
 * called by kill_curthread()
 */
void kExit(int exit_code)
{
    lock_acquire(curproc->pid_lock);

    //check the state of each child program of proc
    //if the child has already exited,clear its pid information
    //if not, update its state to ORPHAN to indicate its parent has exited
    updateChildState(curproc);

    pid_t pid = curproc->pid;

    //if parent has already exited, clear the current process's pid information
    if (curproc->proc_state == ORPHAN)
    {
        proc_destroy(curproc);
        reset_pidtable_entry(pid);
        curproc->proc_state = READY;
        curproc->exit_code = (int) NULL;
    }
    // if parent is still running, update proc's state to ZOMBIE and record its exit code
    else if (curproc->proc_state == RUNNING)
    {
        curproc->proc_state = ZOMBIE;
        // curproc->exit_code = exit_code;
        curproc->exit_code = _MKWAIT_SIG(exit_code);
    }
    //should not branch here
    else
    {
        panic("Tried to exit a bad process.\n");
    }

    //signal processes waiting for proc to exit
    cv_broadcast(curproc->pid_cv, curproc->pid_lock);

    lock_release(curproc->pid_lock);
    thread_exit();
}

/**
 * Wait for the process specified by pid to exit, and return an encoded exit status in the integer pointed 
 * to by status. If that process has exited already, waitpid returns immediately. 
 * If that process does not exist, waitpid fails.
 * It is explicitly allowed for status to be NULL, in which case waitpid operates normally but the status value
 * is not produced.
 * A process moves from "has exited already" to "does not exist" when every process that is expected to collect
 * its exit status with waitpid has done so.
 * If a parent process exits before one or more of its children, it can no longer be expected collect their exit status. 
 * not required to implement options in OS161.However, the system should 
 * check to make sure that requests for options the system do not support are rejected.
 * On success, returns the process id whose exit status is reported in status. 
 * On failure, the corresponding error code is returned.
 */

int sys_waitpid(pid_t pid, int *status, int options, int *retval)
{
    int ret;
    int *kbuf;

    // reject requests for options since options are not required to be implemented in OS161
    if (options != 0)
    {
        return EINVAL;
    }
    // No such a process if it's out of bound or its pid is not a valid entry in the pidtable
    if (pid < PID_MIN || pid > PID_MAX || pidtable->occupied[pid] == FREE)
    {
        return ESRCH;
    }

    // allocate a kernel space to temporarily store the child process's exitcode
    kbuf = kmalloc(sizeof(*kbuf));
    if (kbuf == NULL)
    {
        return ENOMEM;
    }

    // check if the process specified by pid is a child process of curproc

    // bool var indicates if pid is a child of current process
    int flag = 0;
    struct proc *proc_of_pid = pidtable->pid_procs[pid];
    int num_of_children = array_num(curproc->children);
    for (int i = 0; i < num_of_children; ++i)
    {
        struct proc *child = array_get(curproc->children, i);
        if (child == proc_of_pid)
        {
            flag = 1;
        }
    }
    //return if the process specified by pid is NOT a child process of curproc
    if (flag == 0)
    {
        kfree(kbuf);
        return ECHILD;
    }

    lock_acquire(proc_of_pid->pid_lock);

    // wait until the child process to exit and store the child process's exitcode in status
    while (proc_of_pid->proc_state != ZOMBIE)
    {
        cv_wait(proc_of_pid->pid_cv, proc_of_pid->pid_lock);
    }
    *kbuf = proc_of_pid->exit_code;

    if (status != NULL)
    {
        //check if status pointer is properly aligned to 4
        if (!((int)status % 4 == 0))
        {
            kfree(kbuf);
            return EFAULT;
        }
        ret = copyout(kbuf, (userptr_t)status, sizeof(int));
        if (ret)
        {
            kfree(kbuf);
            return EFAULT;
        }
    }

    //destroy the child's process structure and free its slot in pidtable
    //since we, as the parent process, is the only one waiting for it
    proc_destroy(proc_of_pid);
    reset_pidtable_entry(pid);

    kfree(kbuf);

    //returns the pid whose exit status is reported in status
    *retval = pid;
    lock_release(proc_of_pid->pid_lock);

    return 0;
}
