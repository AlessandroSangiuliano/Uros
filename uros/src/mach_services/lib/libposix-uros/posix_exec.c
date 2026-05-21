/*
 * libposix-uros — POSIX execve over exec_server (#254 / Phase 5).
 *
 * On Uros, POSIX execve("path", argv, envp) becomes:
 *   1. Serialise argv + envp into the NUL-separated blob format
 *      exec_server expects (exec_blob_t in exec_types.h).
 *   2. RPC `exec_load` to exec_server: it creates a brand-new task
 *      with the requested ELF already running.
 *   3. proc_register the new task with proc_server so SIGCHLD wires
 *      up correctly later.
 *   4. task_terminate(self) — the calling task vanishes, the new task
 *      keeps running, matching POSIX "replace current image" within
 *      one tolerable nuance (the task port name changes, where Linux
 *      keeps it stable).
 *
 * The Phase 5b fork+exec pattern will use this same primitive via
 * the `__uros_spawn` helper below, which is also what `posix_spawn`
 * eventually plugs into.  Until fork lands, calling execve from a
 * useful program is rare (you become the new image and die), so
 * the runtime smoke for Phase 5 lives in posix_wait.c — execve is
 * available as a syscall but not yet exercised end-to-end.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach_init.h>
#include <servers/netname.h>

#include "exec.h"
#include "exec_types.h"
#include "proc.h"
#include "proc_types.h"
#include "uros_fd_handoff.h"

/* posix_exec_fds.c: serialise the surviving fd set into 'blob'. */
extern int __uros_serialize_fds(struct uros_fd_handoff *blob);

extern mach_port_t  __uros_proc_port;
extern unsigned int __uros_my_pid;

/* Cached exec_server send right — looked up once.  MACH_PORT_NULL
 * means "not yet resolved" (lookup is lazy on first execve call). */
static mach_port_t __uros_exec_port = MACH_PORT_NULL;

/* Phase 5b (#255) hooks for posix_fork.c.  Keeps __uros_exec_port
 * file-static here while letting fork inherit the cached send right
 * into the child's IPC space. */
mach_port_t __uros_get_exec_port(void) { return __uros_exec_port; }
void        __uros_set_exec_port(mach_port_t p) { __uros_exec_port = p; }

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static kern_return_t
lookup_exec_server(mach_port_t *out)
{
    if (name_server_port == MACH_PORT_NULL)
        return KERN_INVALID_ARGUMENT;
    char host[80] = "";
    char serv[80] = "exec_server";
    return netname_look_up(name_server_port, host, serv, out);
}

/*
 * Serialise a NULL-terminated array of C strings into the
 * NUL-separated + double-NUL-terminated wire format.  Returns the
 * total byte count (including the trailing NUL).  buf may be NULL in
 * which case we just measure — caller passes a fitting buffer on the
 * second pass.  Stops early (returns -E2BIG) if the result exceeds
 * EXEC_BLOB_MAX, matching exec_server's limit.
 */
static long
serialise_blob(char *const *vec, char *buf, size_t buf_size)
{
    size_t total = 0;

    if (vec) {
        for (size_t i = 0; vec[i]; i++) {
            size_t n = strlen(vec[i]) + 1;
            if (total + n + 1 > EXEC_BLOB_MAX)
                return -E2BIG;
            if (buf) {
                if (total + n > buf_size)
                    return -E2BIG;
                memcpy(buf + total, vec[i], n);
            }
            total += n;
        }
    }
    /* Trailing empty string ("" + its NUL = single byte). */
    if (buf) {
        if (total >= buf_size)
            return -E2BIG;
        buf[total] = '\0';
    }
    total += 1;
    return (long)total;
}

/* ------------------------------------------------------------------ */
/* Public: spawn (used by execve below + by Phase 5 smoke directly)    */
/* ------------------------------------------------------------------ */

/*
 * __uros_spawn — implementation primitive shared between execve()
 * and the Phase 5 smoke driver.  Loads `path` via exec_server and
 * registers the new task with proc_server, returning the assigned
 * pid + the task/thread send rights.  Returns 0 on success, negative
 * errno on failure.
 *
 * Out arguments may be NULL when the caller doesn't need them — for
 * an execve-style "replace and die" the caller only cares that the
 * spawn succeeded so it can task_terminate itself.
 */
int
__uros_spawn(const char *path,
             char *const argv[],
             char *const envp[],
             mach_port_t *out_task,
             mach_port_t *out_thread,
             proc_pid_t *out_pid)
{
    kern_return_t kr;
    int rc;

    /* Resolve exec_server on first use. */
    if (__uros_exec_port == MACH_PORT_NULL) {
        kr = lookup_exec_server(&__uros_exec_port);
        if (kr != NETNAME_SUCCESS)
            return -ENOSYS;
    }

    /* Two-pass blob serialisation: measure, allocate on the stack,
     * then fill.  EXEC_BLOB_MAX = 4 KiB ceiling matches exec_server. */
    long argv_n = serialise_blob(argv, NULL, 0);
    long envp_n = serialise_blob(envp, NULL, 0);
    if (argv_n < 0) return (int)argv_n;
    if (envp_n < 0) return (int)envp_n;

    char argv_blob[EXEC_BLOB_MAX];
    char envp_blob[EXEC_BLOB_MAX];
    (void)serialise_blob(argv, argv_blob, sizeof argv_blob);
    (void)serialise_blob(envp, envp_blob, sizeof envp_blob);

    mach_port_t new_task = MACH_PORT_NULL;
    mach_port_t new_thread = MACH_PORT_NULL;

    /* MIG name_t / exec_path_t is char[1024]; size the local buffer
     * properly so -Wstringop-overflow is satisfied. */
    char path_buf[EXEC_PATH_MAX];
    size_t path_len = strlen(path);
    if (path_len >= sizeof path_buf)
        return -ENAMETOOLONG;
    memcpy(path_buf, path, path_len + 1);

    rc = EXEC_ERR_PARSE;
    kr = exec_load(__uros_exec_port,
                   mach_task_self(),
                   path_buf,
                   (vm_offset_t)argv_blob, (mach_msg_type_number_t)argv_n,
                   (vm_offset_t)envp_blob, (mach_msg_type_number_t)envp_n,
                   &new_task, &new_thread, &rc);
    if (kr != KERN_SUCCESS)
        return -EIO;
    switch (rc) {
    case EXEC_OK:                              break;
    case EXEC_ERR_NOT_FOUND:                   return -ENOENT;
    case EXEC_ERR_READ:                        return -EIO;
    case EXEC_ERR_PARSE:                       return -ENOEXEC;
    case EXEC_ERR_NOT_STATIC:                  return -ENOEXEC;
    case EXEC_ERR_TASK_CREATE:                 return -EAGAIN;
    case EXEC_ERR_VM_SETUP:                    return -ENOMEM;
    case EXEC_ERR_THREAD_CREATE:               return -EAGAIN;
    case EXEC_ERR_BAD_BLOB:                    return -EINVAL;
    case EXEC_ERR_TOO_BIG:                     return -E2BIG;
    case EXEC_ERR_PERMISSION:                  return -EACCES;
    default:                                   return -EIO;
    }

    /*
     * #262 step 3: hand the surviving (non-O_CLOEXEC) fds to the new
     * image, which exec_server returned SUSPENDED.  We plant a handoff
     * page at the fixed VA, insert each fs_server send right into the
     * new task's IPC space (under the same name the blob records), then
     * vm_write the blob.  The new task's __uros_absorb_inherited_fds
     * reads it before main.  Skip entirely when nothing is inheritable.
     */
    {
        static struct uros_fd_handoff blob;   /* ~2 KiB — keep off the stack */
        int nfd = __uros_serialize_fds(&blob);
        if (nfd > 0) {
            vm_address_t hv = (vm_address_t)EXEC_FD_HANDOFF_VA;
            if (vm_allocate(new_task, &hv, EXEC_FD_HANDOFF_SIZE, FALSE)
                == KERN_SUCCESS) {
                int i, j;
                for (i = 0; i < nfd; i++) {
                    mach_port_t name = (mach_port_t)blob.recs[i].fs_port;
                    mach_port_t right = MACH_PORT_NULL;
                    mach_msg_type_name_t acq = 0;
                    int dup = 0;
                    for (j = 0; j < i; j++)
                        if (blob.recs[j].fs_port == blob.recs[i].fs_port) {
                            dup = 1; break;
                        }
                    if (dup)
                        continue;
                    if (mach_port_extract_right(mach_task_self(), name,
                            MACH_MSG_TYPE_COPY_SEND, &right, &acq)
                        == KERN_SUCCESS)
                        (void)mach_port_insert_right(new_task, name,
                                                     right, acq);
                }
                (void)vm_write(new_task, (vm_address_t)EXEC_FD_HANDOFF_VA,
                               (vm_offset_t)&blob, sizeof blob);
            }
        }
    }

    /* The initial thread came back suspended (#262 step 3) — start it
     * now that the fd handoff is in place. */
    (void)thread_resume(new_thread);

    /* Register the new task with proc_server so SIGCHLD / waitpid
     * work.  Failure here is non-fatal — the task is already running. */
    proc_pid_t new_pid = 0;
    int prc = PROC_ERR_INVAL;
    if (__uros_proc_port != MACH_PORT_NULL) {
        char cmd[128];
        size_t cl = 0;
        for (size_t i = path_len; i > 0; i--) {
            if (path[i - 1] == '/') { cl = path_len - i; goto have_base; }
        }
        cl = path_len;
have_base:
        if (cl >= sizeof cmd) cl = sizeof cmd - 1;
        memcpy(cmd, path + path_len - cl, cl);
        cmd[cl] = '\0';
        (void)proc_register(__uros_proc_port, __uros_my_pid,
                            new_task, cmd, &new_pid, &prc);
    }

    if (out_task)   *out_task = new_task;
    else            (void)mach_port_deallocate(mach_task_self(), new_task);
    if (out_thread) *out_thread = new_thread;
    else            (void)mach_port_deallocate(mach_task_self(), new_thread);
    if (out_pid)    *out_pid = new_pid;

    return 0;
}

/* ------------------------------------------------------------------ */
/* POSIX execve                                                       */
/* ------------------------------------------------------------------ */

/*
 * Replace the current task image with `path`.  On Uros this is a
 * "spawn + suicide" — see top-of-file rationale.  Successful return
 * never happens; failure returns a negative errno.
 */
int
__uros_execve(const char *path, char *const argv[], char *const envp[])
{
    int r = __uros_spawn(path, argv, envp, NULL, NULL, NULL);
    if (r != 0)
        return r;
    /* New task is alive — politely vanish.  task_terminate doesn't
     * return; the for(;;) is just to satisfy non-void return type. */
    (void)task_terminate(mach_task_self());
    for (;;) { /* unreachable */ }
    return -EIO;
}
