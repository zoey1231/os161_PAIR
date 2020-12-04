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
#include <kern/limits.h>
#include <mips/tlb.h>
#include <elf.h>
#include <spl.h>

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

    lock_acquire(curproc->fda_lock);
    if (array_num(curproc->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->fda_lock);
        return EMFILE;
    }

    // update fd to the number of files have opened. fd should be associated to
    // the new file after the loop
    while (fd_get(curproc->fdArray, fd, NULL) != NULL)
    {
        fd += 1;
    }
    // Open file and grab the vnode associated with it
    err = vfs_open(path, flags, 0, &vn);

    kfree(path);
    kfree(path_len);

    if (err)
    {
        lock_release(curproc->fda_lock);
        return err;
    }
    file = kmalloc(sizeof(struct file));
    if (file == NULL)
    {
        lock_release(curproc->fda_lock);
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
            lock_release(curproc->fda_lock);

            return ENOMEM;
        }
        VOP_STAT(vn, stat);
        file->offset = stat->st_size;
    }
    else
    {
        file->offset = 0;
    }

    //build file entry
    fe = kmalloc(sizeof(struct fd_entry));
    if (fe == NULL)
    {
        lock_release(curproc->fda_lock);

        return EMFILE;
    }

    fe->fd = fd;
    fe->file = file;
    *retVal = fd;
    lock_acquire(filetable.ft_lock);

    //add the file's kernel representation to the glocal filetable
    err = filetable_add(&filetable, file);
    if (err)
    {
        lock_release(curproc->fda_lock);
        lock_release(filetable.ft_lock);
        return err;
    }
    //add the file entry to the process's filedescriptor table fd_Array
    err = array_add(curproc->fdArray, fe, NULL);
    {
        lock_release(curproc->fda_lock);
        lock_release(filetable.ft_lock);
        return err;
    }

    lock_release(curproc->fda_lock);
    lock_release(filetable.ft_lock);
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

    lock_acquire(curproc->fda_lock);
    int index = -1;
    struct fd_entry *fe = fd_get(curproc->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);

    //decrement the reference count of the file, if refcount is 0 after the decrement,
    //remove the file completely since there is no one has opened the file
    //otherwise, simply remove the file with fd from the process's filedescriptor table
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
        // remove file entry from the process's filedescriptor table
        array_remove(curproc->fdArray, (unsigned)index);
        kfree(fe);
    }
    else
    {
        fe->file = NULL;
        // remove file entry from the process's filedescriptor table
        array_remove(curproc->fdArray, (unsigned)index);
        kfree(fe);
        lock_release(file->file_lock);
    }

    lock_release(curproc->fda_lock);
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

    int index = -1;
    lock_acquire(curproc->fda_lock);
    struct fd_entry *fe = fd_get(curproc->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if fd was opened for reading
    if (file->status & O_WRONLY)
    {
        lock_release(file->file_lock);
        lock_release(curproc->fda_lock);
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
        lock_release(curproc->fda_lock);
        return err;
    }

    file->offset += (off_t)(buflen - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->fda_lock);
    *retVal = (int)buflen - uio.uio_resid;

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
    int err;
    int index = -1;

    lock_acquire(curproc->fda_lock);
    struct fd_entry *fe = fd_get(curproc->fdArray, fd, &index);
    if (index == -1)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }

    file = fe->file;
    lock_acquire(file->file_lock);
    //check if the file is opened for writing
    if (!(file->status & (O_WRONLY | O_RDWR)))
    {
        lock_release(file->file_lock);
        lock_release(curproc->fda_lock);
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
        lock_release(curproc->fda_lock);
        return err;
    }

    file->offset += (off_t)(nbytes - uio.uio_resid);
    lock_release(file->file_lock);
    lock_release(curproc->fda_lock);
    *retVal = (int)nbytes - uio.uio_resid;

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
    lock_acquire(curproc->fda_lock);
    struct fd_entry *fe = fd_get(curproc->fdArray, fd, &index);
    // return EBAD if can't find the file entry with given fd
    if (index == -1)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }
    lock_release(curproc->fda_lock);

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

    lock_acquire(curproc->fda_lock);

    struct fd_entry *fe = fd_get(curproc->fdArray, oldfd, NULL);

    /* check if the file that oldfd points to exists and valid */
    if (fe == NULL || fe->file == NULL)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }
    if (!fe->file->valid)
    {
        lock_release(curproc->fda_lock);
        return EBADF;
    }

    // return the existing file if oldfd is the same as the newfd
    if (oldfd == newfd)
    {
        *retVal = newfd;
        lock_release(curproc->fda_lock);
        return 0;
    }

    struct fd_entry *newfd_fe = fd_get(curproc->fdArray, newfd, NULL);
    // check if file that newfd points to exists, kill it if it does
    if (newfd_fe != NULL)
    {
        lock_release(curproc->fda_lock);
        sys_close(newfd);
        lock_acquire(curproc->fda_lock);
    }

    if (array_num(curproc->fdArray) >= OPEN_MAX)
    {
        lock_release(curproc->fda_lock);
        return EMFILE;
    }

    struct fd_entry *new_fe = kmalloc(sizeof(struct fd_entry));
    if (new_fe == NULL)
    {
        lock_release(curproc->fda_lock);
        return EMFILE;
    }

    new_fe->fd = newfd;
    new_fe->file = fe->file;

    lock_acquire(fe->file->file_lock);
    fe->file->refcount++;
    lock_release(fe->file->file_lock);

    array_add(curproc->fdArray, new_fe, NULL);
    *retVal = newfd;
    lock_release(curproc->fda_lock);
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
 * Fork the current process to to create a child process so that the two copies are almost identical except the PID.
 * On success, sys_fork returns twice, once in the parent process and once in the child process.
 * In the child process, 0 is returned.
 * In the parent process, the new child process's pid is returned.
 * On error, no new process is created. sys_fork only returns once with corresponding error code.
 */
int sys_fork(struct trapframe *tf, int *retval)
{
    //kprintf("enter fork\n");
    int spl = splhigh();
    struct addrspace *parent_as;
    struct addrspace *child_as;

    struct proc *new_proc;
    const char *new_proc_name = curproc->p_name;
    const char *new_thread_name = curthread->t_name;

    //create a new process
    new_proc = create_fork_proc(new_proc_name);
    if (new_proc == NULL)
    {
        return ENOMEM;
    }

    // add the new process to the pidtable and save its PID into new_proc->pid
    int ret = pidtable_add(new_proc, &(new_proc->pid));
    if (ret)
    {
        proc_destroy(new_proc);
        return ret;
    }

    // copy the parent process's address space into the new process
    parent_as = proc_getas();
    ret = as_copy(parent_as, &child_as);
    new_proc->p_addrspace = child_as;
    if (ret)
    { // if addrspace's copy is not successful, free the new process
        if (new_proc->pid >= PID_MIN && new_proc->pid <= PID_MAX)
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

    // copy the trapframe from the parent
    struct trapframe *tf_dup = (struct trapframe *)kmalloc(sizeof(struct trapframe));
    if (tf_dup == NULL)
    {
        return ENOMEM;
    }
    memcpy((void *)tf_dup, (const void *)tf, sizeof(struct trapframe));

    //return child process's PID as the parent process's retVal
    *retval = new_proc->pid;

    //set up the arguments for new process's entry function:enter_forked_process()
    void **argv;
    argv = kmalloc(2 * sizeof(void *));
    argv[0] = tf_dup;
    argv[1] = child_as;

    //create child thread
    ret = thread_fork(new_thread_name, new_proc, enter_forked_process, argv, 2);
    if (ret)
    {
        proc_destroy(new_proc);
        if (new_proc->pid >= PID_MIN && new_proc->pid <= PID_MAX)
            pidtable_remove(new_proc->pid);
        kfree(tf_dup);
        return ret;
    }
    splx(spl);
    return 0;
}

/**
 * Copy a string with intended length copy_len from user's addrspace into a kernel address
 * Return the actual copied length into *actural_len
 */
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
 * Helper function for sys_execv().
 * Return the number of arguments in argc.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
static int get_argc(char **args, int *argc)
{
    int ret;
    int i = 0;
    char *next_arg;

    //count the number of arguments one by one until we hit the null terminator,
    // also check if there are too many arguments
    do
    {
        i++;
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
 * Helper function for copy_in_args()
 * Get the length of a string
 */
static int
get_strLen(const char *string, int max_len, size_t *actual_length)
{

    int ret;
    int i = -1;
    char next_char;
    do
    {
        i++;
        ret = copyin((const_userptr_t)&string[i], (void *)&next_char, (size_t)sizeof(char));
        if (ret)
        {
            return ret;
        }

    } while (next_char != 0 && i < max_len);

    if (next_char != 0)
    {
        return E2BIG;
    }

    *actual_length = i;
    return 0;
}

/**
 * Copy the arguments and each argument string's length into args_copy and size_arr.
 * Return the total argument string's length in strSize_total.
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
static int
copy_in_args(int argc, char **args, char **args_copy, int *size_arr, int *strSize_total)
{
    int err;
    size_t str_len, aligned_strlen, actual_len;
    *strSize_total = 0;

    //check if arguments' size exceeds ARG_MAX
    int arg_size_left = ARG_MAX;

    //copy one argument string at a time
    for (int i = 0; i < argc; i++)
    {
        //get the argument string stored in args[i]'s length in str_len
        err = get_strLen((const char *)args[i], arg_size_left - 1, &str_len);
        if (err)
        {
            for (int j = 0; j < i; j++)
            {
                kfree(args_copy[j]);
            }
            return err;
        }

        //copy the argument string pointed by args[i] into address args_copy[i]
        //record the actual copied length in actual_len
        err = string_in((const char *)args[i], &args_copy[i], str_len, &actual_len);

        //align the string pointer to 4 if not already
        if (actual_len % 4 != 0)
        {
            aligned_strlen = (size_t)4 * (actual_len / 4) + 4;
        }
        else
        {
            aligned_strlen = actual_len;
        }

        arg_size_left -= aligned_strlen;
        size_arr[i] = aligned_strlen;
        *strSize_total += aligned_strlen;
    }
    return 0;
}

/**
 * Helper function for sys_execv()
 * Clear args_copy and size used for copy arguments from kernel memory
 */
static void free_copied_args(int argc, int *size, char **args_copy)
{
    for (int i = 0; i < argc; i++)
    {
        kfree(args_copy[i]);
    }
    kfree(size);
    kfree(args_copy);
}

/**
 * Copy out the argument strings and the null-terminated argv array from the kernel address args_copy 
 * to the stack 
 * Return the updated stack pointer's position to stackptr and the argv array's address to argv
 * On success,return 0.
 * On error, the corresponding error code is returned.
 */
static int copy_out_args(int argc, char **args_copy, int *size, int strSize_total, vaddr_t *stackptr, userptr_t *argv)
{
    size_t actual_len;
    int err;
    userptr_t argv_base_addr = (userptr_t)(*stackptr - (argc + 1) * sizeof(userptr_t *) - strSize_total * sizeof(char));
    userptr_t str_base_addr = (userptr_t)(*stackptr - strSize_total * sizeof(char));

    //update the stack pointer's position to the base address of argv array, and record the address to argv as well
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
        //copy out argument string's pointer to the new stack
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
 * Execute a process with pathname program with arguments in args array.
 * On success, execv does not return; instead, the new program begins executing.
 * On failure, the corresponding error code is returned.
 */
int sys_execv(const char *program, char **args)
{
    int ret;
    int strSize_total;
    char *progname;
    // check for invalid pointers
    if (program == NULL || args == NULL)
        return EFAULT;

    //copy the process's pathname into kernel
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

    //allocate a kernel buffer for copy arguments array args into kernel
    char **args_copy = kmalloc(argc * sizeof(char *));
    if (args_copy == NULL)
    {
        kfree(progname);
        return ENOMEM;
    }

    //allocate an array to store each argument string's length in each array entry
    int *size_arr = kmalloc(argc * sizeof(int));
    if (size_arr == NULL)
    {
        kfree(progname);
        return ENOMEM;
    }

    //copy the arguments and each argument string's length into pre-allocated kernel buffer args_copy and size_arr
    //record the total argument string's length in strSize_total
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
        return ENOMEM;
    }

    //open the file with pathname progname
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

    //Load the executable user program into the new address space
    //record the program's initial Program counter in entrypoint
    ret = load_elf(vn, &entrypoint);
    if (ret)
    {
        //if there's an error, switch back to old address space
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
    //record the stack pointer in stackptr
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

    //copy the arguments to the new address space and properly arranging them on the stack
    //update the stack pointer in stackptr and record the argv array's base address in argv
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

    //free allocated kernel memory for copy arguments
    kfree(progname);
    free_copied_args(argc, size_arr, args_copy);

    //clean up the old address space
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
    // avoid race condition
    lock_acquire(curproc->pid_lock);
    *retval = curproc->pid;
    lock_release(curproc->pid_lock);
    return 0;
}
/**
 * Cause the current process to exit. 
 * The exit code exit_code is reported back to other process(es) via the waitpid() call.
 * sys__exit does not return.
 */
void sys__exit(int exit_code)
{
    lock_acquire(curproc->pid_lock);

    // check the state of each child program of proc
    // if the child has already exited, clear its pid information
    // if not, update its state to ORPHAN to indicate its parent has exited
    updateChildState(curproc);

    pid_t pid = curproc->pid;
    unsigned int state = curproc->proc_state;
    //if parent has already exited, clear the current process
    if (state == ORPHAN)
    {
        lock_release(curproc->pid_lock);
        proc_destroy(curproc);

        lock_acquire(pidtable.pt_lock);
        reset_pidtable_entry(pid);
        lock_release(pidtable.pt_lock);
    }
    // if parent is still running, update proc's state to ZOMBIE and record its exit code
    else if (state == RUNNING)
    {
        curproc->proc_state = ZOMBIE;
        curproc->exit_code = _MKWAIT_EXIT(exit_code);

        //signal processes waiting for curproc to exit
        cv_broadcast(curproc->pid_cv, curproc->pid_lock);

        lock_release(curproc->pid_lock);
    }
    //should not branch here
    else
    {
        lock_release(curproc->pid_lock);
        panic("Tried to exit a bad process.\n");
    }

    // exit the thread
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

    //if parent has already exited, clear the current process
    if (curproc->proc_state == ORPHAN)
    {
        lock_acquire(pidtable.pt_lock);
        reset_pidtable_entry(pid);
        lock_release(pidtable.pt_lock);

        lock_release(curproc->pid_lock);
        proc_destroy(curproc);
    }
    // if parent is still running, update proc's state to ZOMBIE and record its exit code
    else if (curproc->proc_state == RUNNING)
    {
        curproc->proc_state = ZOMBIE;
        curproc->exit_code = _MKWAIT_SIG(exit_code);

        //signal processes waiting for curproc to exit
        cv_broadcast(curproc->pid_cv, curproc->pid_lock);
        lock_release(curproc->pid_lock);
    }
    //should not branch here
    else
    {
        panic("Tried to exit a bad process.\n");
    }

    // exit the thread
    thread_exit();
    panic("program should not reach here");
}

/**
 * Wait for the process specified by pid to exit.
 * Returns the process id in retval and report the exit code of the procss specified by pid in status or not if status is NULL 
 * On success, return 0.
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
    lock_acquire(pidtable.pt_lock);
    // if (pid < PID_MIN || pid > PID_MAX || pidtable.occupied[pid] == FREE)
    if (pid < PID_MIN || pid > PID_MAX || !bitmap_isset(pidtable.occupied, pid))
    {
        lock_release(pidtable.pt_lock);
        return ESRCH;
    }
    lock_release(pidtable.pt_lock);

    // allocate a kernel space to temporarily store the child process's exitcode
    kbuf = kmalloc(sizeof(*kbuf));
    if (kbuf == NULL)
    {
        return ENOMEM; // out of memory error is returned if can't allocate space to kbuf
    }

    // check if the process specified by pid is a child process of curproc

    // bool var flag indicates if pid is a child of current process
    int flag = 0;
    lock_acquire(pidtable.pt_lock);
    // struct proc *proc_of_pid = pidtable.pid_procs[pid];
    struct proc *proc_of_pid = array_get(pidtable.pid_procs, pid);
    lock_release(pidtable.pt_lock);

    int num_of_children = array_num(curproc->children);
    // iterate over curproc's children array and see if process with pid passed in matches with
    //any one of curproc's child process
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

    // wait until the child process to exit and store the child process's exitcode in kbuf
    while (proc_of_pid->proc_state != ZOMBIE)
    {
        cv_wait(proc_of_pid->pid_cv, proc_of_pid->pid_lock);
    }
    *kbuf = proc_of_pid->exit_code;

    // a NULL value of status indicates no status value is produced and everything else proceeds normally
    if (status != NULL)
    {
        //check if status pointer is properly aligned to 4, accounting for misalignment test in badcall
        if (!((int)status % 4 == 0))
        {
            lock_release(proc_of_pid->pid_lock);
            kfree(kbuf);
            return EFAULT; // The status argument was an invalid pointer.
        }

        // collect exit code of the child process, copy out from kbuf which points to
        //proc_of_pid->exit_code to user pointer's status
        ret = copyout(kbuf, (userptr_t)status, sizeof(int));
        if (ret)
        {
            lock_release(proc_of_pid->pid_lock);
            kfree(kbuf);
            return EFAULT; // The status argument was an invalid pointer.
        }
    }

    lock_release(proc_of_pid->pid_lock);

    kfree(kbuf);

    //returns the pid whose exit status is reported in status
    *retval = pid;

    return 0;
}

int sys_sbrk(intptr_t amount, vaddr_t *retval)
{
    //kprintf("sbrk called with %d\n", (int)amount);
    struct addrspace *addr = curproc->p_addrspace;

    //check if heap is initialized
    if (addr->heapregion == NULL)
    {
        as_define_heap(addr, (vaddr_t *)retval);
        *retval = (vaddr_t)addr->heaptop;
        //kprintf("heapbase is now %x\n", (int)retval);
        KASSERT(retval);
    }
    else
    {
        *retval = (vaddr_t)addr->heaptop;
        //kprintf("heapbase is now %x\n", (int)retval);
        KASSERT(retval);
    }

    //check if don't need to alloc
    if (amount == 0)
    {
        return 0;
    }

    //check possible heap overflow
    if ((addr->heaptop + amount < addr->stackbase) &&
        (addr->heaptop + amount >= addr->vtop2) &&
        (addr->heaptop + amount >= addr->vtop1))
    {

        lock_acquire(addr->as_lock);

        if (amount < 0)
        {
            int free_page_num = -amount / PAGE_SIZE;
            int total_page_index = (addr->heaptop - addr->heapbase) / PAGE_SIZE - 1;
            int lower_bound = total_page_index - free_page_num;

            for (int i = total_page_index; i > lower_bound; i--)
            {
                //clear pte
                struct pte *pte = addr->heapregion + i;
                if (pte->allocated == 0)
                {
                    continue;
                }

                if (pte->present == 1)
                {
                    spinlock_acquire(coremap_lock);
                    KASSERT(pte->ppagenum != 0);
                    struct frame *f = coremap + COREMAP_INDEX(pte->ppagenum);
                    KASSERT(f->used == 1);
                    KASSERT(f->as == addr);
                    KASSERT(f->kernel == 0);
                    KASSERT(f->vaddr == pte->vpagenum);
                    KASSERT(f->size == 1);

                    bzero((void *)PADDR_TO_KVADDR(pte->ppagenum), PAGE_SIZE);
                    f->as = NULL;
                    f->used = 0;
                    f->kernel = 0;
                    f->vaddr = 0;
                    f->size = 0;
                    spinlock_release(coremap_lock);
                }
                else if (pte->present == 0)
                {
                    lock_acquire(swap_disk_lock);
                    bitmap_unmark(swap_disk_bitmap, pte->swap_disk_offset);
                    lock_release(swap_disk_lock);
                }

                //update entry in page table
                pte->ppagenum = 0;
                pte->allocated = 0;
                pte->present = 0;
                pte->swap_disk_offset = 0;
            }

            //clear tlb
            int spl = splhigh();
            for (int i = 0; i < NUM_TLB; ++i)
            {
                tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
            }
            splx(spl);
        }

        //kprintf("heaptop = %x\n", (int)addr->heaptop);
        //move heap pointer
        addr->heaptop = addr->heaptop + amount;
        //kprintf("heaptop = %x\n", (int)addr->heaptop);

        lock_release(addr->as_lock);
    }
    else
    {
        kprintf("amount is %x, but heap base %x, heaptop %x\n", (int)amount, (int)(addr->heapbase), (int)(addr->heaptop));
        return EINVAL;
    }

    return 0;
}