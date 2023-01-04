#ifndef	_bsd_1_user_
#define	_bsd_1_user_

/* Module bsd_1 */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

#include <mach/std_types.h>
#include <mach/mach_types.h>
#include <serv/bsd_types.h>
#include <mach_debug/mach_debug_types.h>

/* Routine bsd_execve */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_execve
#if	defined(LINTLIBRARY)
    (proc_port, arg_addr, arg_size, arg_count, env_count, fname, fnameCnt)
	mach_port_t proc_port;
	vm_address_t arg_addr;
	vm_size_t arg_size;
	int arg_count;
	int env_count;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_execve(proc_port, arg_addr, arg_size, arg_count, env_count, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	vm_address_t arg_addr,
	vm_size_t arg_size,
	int arg_count,
	int env_count,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_after_exec */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_after_exec
#if	defined(LINTLIBRARY)
    (proc_port, arg_addr, arg_size, arg_count, env_count, image_port, emul_name, emul_nameCnt, fname, fnameCnt, cfname, cfnameCnt, cfarg, cfargCnt)
	mach_port_t proc_port;
	vm_address_t *arg_addr;
	vm_size_t *arg_size;
	int *arg_count;
	int *env_count;
	mach_port_t *image_port;
	path_name_t emul_name;
	mach_msg_type_number_t *emul_nameCnt;
	path_name_t fname;
	mach_msg_type_number_t *fnameCnt;
	path_name_t cfname;
	mach_msg_type_number_t *cfnameCnt;
	path_name_t cfarg;
	mach_msg_type_number_t *cfargCnt;
{ return bsd_after_exec(proc_port, arg_addr, arg_size, arg_count, env_count, image_port, emul_name, emul_nameCnt, fname, fnameCnt, cfname, cfnameCnt, cfarg, cfargCnt); }
#else
(
	mach_port_t proc_port,
	vm_address_t *arg_addr,
	vm_size_t *arg_size,
	int *arg_count,
	int *env_count,
	mach_port_t *image_port,
	path_name_t emul_name,
	mach_msg_type_number_t *emul_nameCnt,
	path_name_t fname,
	mach_msg_type_number_t *fnameCnt,
	path_name_t cfname,
	mach_msg_type_number_t *cfnameCnt,
	path_name_t cfarg,
	mach_msg_type_number_t *cfargCnt
);
#endif

/* Routine bsd_vm_map */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_vm_map
#if	defined(LINTLIBRARY)
    (proc_port, address, size, mask, anywhere, memory_object_representative, offset, copy, cur_protection, max_protection, inheritance)
	mach_port_t proc_port;
	vm_address_t *address;
	vm_size_t size;
	vm_address_t mask;
	boolean_t anywhere;
	mach_port_t memory_object_representative;
	vm_offset_t offset;
	boolean_t copy;
	vm_prot_t cur_protection;
	vm_prot_t max_protection;
	vm_inherit_t inheritance;
{ return bsd_vm_map(proc_port, address, size, mask, anywhere, memory_object_representative, offset, copy, cur_protection, max_protection, inheritance); }
#else
(
	mach_port_t proc_port,
	vm_address_t *address,
	vm_size_t size,
	vm_address_t mask,
	boolean_t anywhere,
	mach_port_t memory_object_representative,
	vm_offset_t offset,
	boolean_t copy,
	vm_prot_t cur_protection,
	vm_prot_t max_protection,
	vm_inherit_t inheritance
);
#endif

/* Routine bsd_fd_to_file_port */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_fd_to_file_port
#if	defined(LINTLIBRARY)
    (proc_port, fileno, port)
	mach_port_t proc_port;
	int fileno;
	mach_port_t *port;
{ return bsd_fd_to_file_port(proc_port, fileno, port); }
#else
(
	mach_port_t proc_port,
	int fileno,
	mach_port_t *port
);
#endif

/* Routine bsd_file_port_open */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_file_port_open
#if	defined(LINTLIBRARY)
    (proc_port, mode, crtmode, fname, fnameCnt, port)
	mach_port_t proc_port;
	int mode;
	int crtmode;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
	mach_port_t *port;
{ return bsd_file_port_open(proc_port, mode, crtmode, fname, fnameCnt, port); }
#else
(
	mach_port_t proc_port,
	int mode,
	int crtmode,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt,
	mach_port_t *port
);
#endif

/* Routine bsd_zone_info */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_zone_info
#if	defined(LINTLIBRARY)
    (proc_port, names, namesCnt, info, infoCnt)
	mach_port_t proc_port;
	zone_name_array_t *names;
	mach_msg_type_number_t *namesCnt;
	zone_info_array_t *info;
	mach_msg_type_number_t *infoCnt;
{ return bsd_zone_info(proc_port, names, namesCnt, info, infoCnt); }
#else
(
	mach_port_t proc_port,
	zone_name_array_t *names,
	mach_msg_type_number_t *namesCnt,
	zone_info_array_t *info,
	mach_msg_type_number_t *infoCnt
);
#endif

/* Routine bsd_signal_port_register */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_signal_port_register
#if	defined(LINTLIBRARY)
    (proc_port, sigport)
	mach_port_t proc_port;
	mach_port_t sigport;
{ return bsd_signal_port_register(proc_port, sigport); }
#else
(
	mach_port_t proc_port,
	mach_port_t sigport
);
#endif

/* Routine bsd_fork */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_fork
#if	defined(LINTLIBRARY)
    (proc_port, new_state, new_stateCnt, child_pid)
	mach_port_t proc_port;
	thread_state_t new_state;
	mach_msg_type_number_t new_stateCnt;
	int *child_pid;
{ return bsd_fork(proc_port, new_state, new_stateCnt, child_pid); }
#else
(
	mach_port_t proc_port,
	thread_state_t new_state,
	mach_msg_type_number_t new_stateCnt,
	int *child_pid
);
#endif

/* Routine bsd_vfork */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_vfork
#if	defined(LINTLIBRARY)
    (proc_port, new_state, new_stateCnt, child_pid)
	mach_port_t proc_port;
	thread_state_t new_state;
	mach_msg_type_number_t new_stateCnt;
	int *child_pid;
{ return bsd_vfork(proc_port, new_state, new_stateCnt, child_pid); }
#else
(
	mach_port_t proc_port,
	thread_state_t new_state,
	mach_msg_type_number_t new_stateCnt,
	int *child_pid
);
#endif

/* Routine bsd_take_signal */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_take_signal
#if	defined(LINTLIBRARY)
    (proc_port, old_mask, old_onstack, sig, code, handler, new_sp)
	mach_port_t proc_port;
	sigset_t *old_mask;
	int *old_onstack;
	int *sig;
	integer_t *code;
	vm_offset_t *handler;
	vm_offset_t *new_sp;
{ return bsd_take_signal(proc_port, old_mask, old_onstack, sig, code, handler, new_sp); }
#else
(
	mach_port_t proc_port,
	sigset_t *old_mask,
	int *old_onstack,
	int *sig,
	integer_t *code,
	vm_offset_t *handler,
	vm_offset_t *new_sp
);
#endif

/* Routine bsd_sigreturn */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_sigreturn
#if	defined(LINTLIBRARY)
    (proc_port, old_on_stack, old_sigmask)
	mach_port_t proc_port;
	int old_on_stack;
	sigset_t old_sigmask;
{ return bsd_sigreturn(proc_port, old_on_stack, old_sigmask); }
#else
(
	mach_port_t proc_port,
	int old_on_stack,
	sigset_t old_sigmask
);
#endif

/* Routine bsd_getrusage */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_getrusage
#if	defined(LINTLIBRARY)
    (proc_port, which, rusage)
	mach_port_t proc_port;
	int which;
	rusage_t *rusage;
{ return bsd_getrusage(proc_port, which, rusage); }
#else
(
	mach_port_t proc_port,
	int which,
	rusage_t *rusage
);
#endif

/* Routine bsd_chdir */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_chdir
#if	defined(LINTLIBRARY)
    (proc_port, fname, fnameCnt)
	mach_port_t proc_port;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_chdir(proc_port, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_chroot */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_chroot
#if	defined(LINTLIBRARY)
    (proc_port, fname, fnameCnt)
	mach_port_t proc_port;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_chroot(proc_port, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_open */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_open
#if	defined(LINTLIBRARY)
    (proc_port, mode, crtmode, fname, fnameCnt, fileno)
	mach_port_t proc_port;
	int mode;
	int crtmode;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
	int *fileno;
{ return bsd_open(proc_port, mode, crtmode, fname, fnameCnt, fileno); }
#else
(
	mach_port_t proc_port,
	int mode,
	int crtmode,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt,
	int *fileno
);
#endif

/* Routine bsd_mknod */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_mknod
#if	defined(LINTLIBRARY)
    (proc_port, fmode, dev, fname, fnameCnt)
	mach_port_t proc_port;
	int fmode;
	int dev;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_mknod(proc_port, fmode, dev, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	int fmode,
	int dev,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_link */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_link
#if	defined(LINTLIBRARY)
    (proc_port, target, targetCnt, linkname, linknameCnt)
	mach_port_t proc_port;
	path_name_t target;
	mach_msg_type_number_t targetCnt;
	path_name_t linkname;
	mach_msg_type_number_t linknameCnt;
{ return bsd_link(proc_port, target, targetCnt, linkname, linknameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t target,
	mach_msg_type_number_t targetCnt,
	path_name_t linkname,
	mach_msg_type_number_t linknameCnt
);
#endif

/* Routine bsd_symlink */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_symlink
#if	defined(LINTLIBRARY)
    (proc_port, target, targetCnt, linkname, linknameCnt)
	mach_port_t proc_port;
	path_name_t target;
	mach_msg_type_number_t targetCnt;
	path_name_t linkname;
	mach_msg_type_number_t linknameCnt;
{ return bsd_symlink(proc_port, target, targetCnt, linkname, linknameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t target,
	mach_msg_type_number_t targetCnt,
	path_name_t linkname,
	mach_msg_type_number_t linknameCnt
);
#endif

/* Routine bsd_unlink */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_unlink
#if	defined(LINTLIBRARY)
    (proc_port, fname, fnameCnt)
	mach_port_t proc_port;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_unlink(proc_port, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_access */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_access
#if	defined(LINTLIBRARY)
    (proc_port, fmode, fname, fnameCnt)
	mach_port_t proc_port;
	int fmode;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_access(proc_port, fmode, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	int fmode,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_readlink */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_readlink
#if	defined(LINTLIBRARY)
    (proc_port, count, name, nameCnt, buf, bufCnt)
	mach_port_t proc_port;
	int count;
	path_name_t name;
	mach_msg_type_number_t nameCnt;
	small_char_array buf;
	mach_msg_type_number_t *bufCnt;
{ return bsd_readlink(proc_port, count, name, nameCnt, buf, bufCnt); }
#else
(
	mach_port_t proc_port,
	int count,
	path_name_t name,
	mach_msg_type_number_t nameCnt,
	small_char_array buf,
	mach_msg_type_number_t *bufCnt
);
#endif

/* Routine bsd_utimes */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_utimes
#if	defined(LINTLIBRARY)
    (proc_port, times, fname, fnameCnt)
	mach_port_t proc_port;
	timeval_2_t times;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_utimes(proc_port, times, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	timeval_2_t times,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_rename */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_rename
#if	defined(LINTLIBRARY)
    (proc_port, from_name, from_nameCnt, to_name, to_nameCnt)
	mach_port_t proc_port;
	path_name_t from_name;
	mach_msg_type_number_t from_nameCnt;
	path_name_t to_name;
	mach_msg_type_number_t to_nameCnt;
{ return bsd_rename(proc_port, from_name, from_nameCnt, to_name, to_nameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t from_name,
	mach_msg_type_number_t from_nameCnt,
	path_name_t to_name,
	mach_msg_type_number_t to_nameCnt
);
#endif

/* Routine bsd_mkdir */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_mkdir
#if	defined(LINTLIBRARY)
    (proc_port, dmode, name, nameCnt)
	mach_port_t proc_port;
	int dmode;
	path_name_t name;
	mach_msg_type_number_t nameCnt;
{ return bsd_mkdir(proc_port, dmode, name, nameCnt); }
#else
(
	mach_port_t proc_port,
	int dmode,
	path_name_t name,
	mach_msg_type_number_t nameCnt
);
#endif

/* Routine bsd_rmdir */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_rmdir
#if	defined(LINTLIBRARY)
    (proc_port, name, nameCnt)
	mach_port_t proc_port;
	path_name_t name;
	mach_msg_type_number_t nameCnt;
{ return bsd_rmdir(proc_port, name, nameCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t name,
	mach_msg_type_number_t nameCnt
);
#endif

/* Routine bsd_acct */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_acct
#if	defined(LINTLIBRARY)
    (proc_port, acct_on, fname, fnameCnt)
	mach_port_t proc_port;
	boolean_t acct_on;
	path_name_t fname;
	mach_msg_type_number_t fnameCnt;
{ return bsd_acct(proc_port, acct_on, fname, fnameCnt); }
#else
(
	mach_port_t proc_port,
	boolean_t acct_on,
	path_name_t fname,
	mach_msg_type_number_t fnameCnt
);
#endif

/* Routine bsd_write_short */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_write_short
#if	defined(LINTLIBRARY)
    (proc_port, fileno, data, dataCnt, amount_written)
	mach_port_t proc_port;
	int fileno;
	small_char_array data;
	mach_msg_type_number_t dataCnt;
	size_t *amount_written;
{ return bsd_write_short(proc_port, fileno, data, dataCnt, amount_written); }
#else
(
	mach_port_t proc_port,
	int fileno,
	small_char_array data,
	mach_msg_type_number_t dataCnt,
	size_t *amount_written
);
#endif

/* Routine bsd_write_long */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_write_long
#if	defined(LINTLIBRARY)
    (proc_port, fileno, data, dataCnt, amount_written)
	mach_port_t proc_port;
	int fileno;
	char_array data;
	mach_msg_type_number_t dataCnt;
	size_t *amount_written;
{ return bsd_write_long(proc_port, fileno, data, dataCnt, amount_written); }
#else
(
	mach_port_t proc_port,
	int fileno,
	char_array data,
	mach_msg_type_number_t dataCnt,
	size_t *amount_written
);
#endif

/* Routine bsd_read_short */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_read_short
#if	defined(LINTLIBRARY)
    (proc_port, fileno, datalen, nread, data, dataCnt)
	mach_port_t proc_port;
	int fileno;
	int datalen;
	size_t *nread;
	small_char_array data;
	mach_msg_type_number_t *dataCnt;
{ return bsd_read_short(proc_port, fileno, datalen, nread, data, dataCnt); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int datalen,
	size_t *nread,
	small_char_array data,
	mach_msg_type_number_t *dataCnt
);
#endif

/* Routine bsd_read_long */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_read_long
#if	defined(LINTLIBRARY)
    (proc_port, fileno, datalen, nread, data, dataCnt)
	mach_port_t proc_port;
	int fileno;
	int datalen;
	size_t *nread;
	char_array *data;
	mach_msg_type_number_t *dataCnt;
{ return bsd_read_long(proc_port, fileno, datalen, nread, data, dataCnt); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int datalen,
	size_t *nread,
	char_array *data,
	mach_msg_type_number_t *dataCnt
);
#endif

/* Routine bsd_select */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_select
#if	defined(LINTLIBRARY)
    (proc_port, nd, in_set, ou_set, ex_set, in_valid, ou_valid, ex_valid, do_timeout, tv, rval)
	mach_port_t proc_port;
	int nd;
	fd_set *in_set;
	fd_set *ou_set;
	fd_set *ex_set;
	boolean_t in_valid;
	boolean_t ou_valid;
	boolean_t ex_valid;
	boolean_t do_timeout;
	timeval_t tv;
	integer_t *rval;
{ return bsd_select(proc_port, nd, in_set, ou_set, ex_set, in_valid, ou_valid, ex_valid, do_timeout, tv, rval); }
#else
(
	mach_port_t proc_port,
	int nd,
	fd_set *in_set,
	fd_set *ou_set,
	fd_set *ex_set,
	boolean_t in_valid,
	boolean_t ou_valid,
	boolean_t ex_valid,
	boolean_t do_timeout,
	timeval_t tv,
	integer_t *rval
);
#endif

/* Routine bsd_task_by_pid */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_task_by_pid
#if	defined(LINTLIBRARY)
    (proc_port, pid, task)
	mach_port_t proc_port;
	int pid;
	task_t *task;
{ return bsd_task_by_pid(proc_port, pid, task); }
#else
(
	mach_port_t proc_port,
	int pid,
	task_t *task
);
#endif

/* Routine bsd_setgroups */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_setgroups
#if	defined(LINTLIBRARY)
    (proc_port, gidsetsize, gidset)
	mach_port_t proc_port;
	int gidsetsize;
	gidset_t gidset;
{ return bsd_setgroups(proc_port, gidsetsize, gidset); }
#else
(
	mach_port_t proc_port,
	int gidsetsize,
	gidset_t gidset
);
#endif

/* Routine bsd_setrlimit */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_setrlimit
#if	defined(LINTLIBRARY)
    (proc_port, which, lim)
	mach_port_t proc_port;
	int which;
	rlimit_t lim;
{ return bsd_setrlimit(proc_port, which, lim); }
#else
(
	mach_port_t proc_port,
	int which,
	rlimit_t lim
);
#endif

/* Routine bsd_sigstack */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_sigstack
#if	defined(LINTLIBRARY)
    (proc_port, have_nss, nss, oss)
	mach_port_t proc_port;
	boolean_t have_nss;
	sigstack_t nss;
	sigstack_t *oss;
{ return bsd_sigstack(proc_port, have_nss, nss, oss); }
#else
(
	mach_port_t proc_port,
	boolean_t have_nss,
	sigstack_t nss,
	sigstack_t *oss
);
#endif

/* Routine bsd_settimeofday */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_settimeofday
#if	defined(LINTLIBRARY)
    (proc_port, have_tv, tv, have_tz, tz)
	mach_port_t proc_port;
	boolean_t have_tv;
	timeval_t tv;
	boolean_t have_tz;
	timezone_t tz;
{ return bsd_settimeofday(proc_port, have_tv, tv, have_tz, tz); }
#else
(
	mach_port_t proc_port,
	boolean_t have_tv,
	timeval_t tv,
	boolean_t have_tz,
	timezone_t tz
);
#endif

/* Routine bsd_adjtime */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_adjtime
#if	defined(LINTLIBRARY)
    (proc_port, delta, olddelta)
	mach_port_t proc_port;
	timeval_t delta;
	timeval_t *olddelta;
{ return bsd_adjtime(proc_port, delta, olddelta); }
#else
(
	mach_port_t proc_port,
	timeval_t delta,
	timeval_t *olddelta
);
#endif

/* Routine bsd_setitimer */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_setitimer
#if	defined(LINTLIBRARY)
    (proc_port, which, have_itv, oitv, itv)
	mach_port_t proc_port;
	int which;
	boolean_t have_itv;
	itimerval_t *oitv;
	itimerval_t itv;
{ return bsd_setitimer(proc_port, which, have_itv, oitv, itv); }
#else
(
	mach_port_t proc_port,
	int which,
	boolean_t have_itv,
	itimerval_t *oitv,
	itimerval_t itv
);
#endif

/* Routine bsd_bind */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_bind
#if	defined(LINTLIBRARY)
    (proc_port, s, name, nameCnt)
	mach_port_t proc_port;
	int s;
	sockarg_t name;
	mach_msg_type_number_t nameCnt;
{ return bsd_bind(proc_port, s, name, nameCnt); }
#else
(
	mach_port_t proc_port,
	int s,
	sockarg_t name,
	mach_msg_type_number_t nameCnt
);
#endif

/* Routine bsd_connect */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_connect
#if	defined(LINTLIBRARY)
    (proc_port, s, name, nameCnt)
	mach_port_t proc_port;
	int s;
	sockarg_t name;
	mach_msg_type_number_t nameCnt;
{ return bsd_connect(proc_port, s, name, nameCnt); }
#else
(
	mach_port_t proc_port,
	int s,
	sockarg_t name,
	mach_msg_type_number_t nameCnt
);
#endif

/* Routine bsd_setsockopt */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_setsockopt
#if	defined(LINTLIBRARY)
    (proc_port, s, level, name, val, valCnt)
	mach_port_t proc_port;
	int s;
	int level;
	int name;
	sockarg_t val;
	mach_msg_type_number_t valCnt;
{ return bsd_setsockopt(proc_port, s, level, name, val, valCnt); }
#else
(
	mach_port_t proc_port,
	int s,
	int level,
	int name,
	sockarg_t val,
	mach_msg_type_number_t valCnt
);
#endif

/* Routine bsd_getsockopt */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_getsockopt
#if	defined(LINTLIBRARY)
    (proc_port, s, level, name, val, valCnt)
	mach_port_t proc_port;
	int s;
	int level;
	int name;
	sockarg_t val;
	mach_msg_type_number_t *valCnt;
{ return bsd_getsockopt(proc_port, s, level, name, val, valCnt); }
#else
(
	mach_port_t proc_port,
	int s,
	int level,
	int name,
	sockarg_t val,
	mach_msg_type_number_t *valCnt
);
#endif

/* Routine bsd_init_process */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_init_process
#if	defined(LINTLIBRARY)
    (proc_port)
	mach_port_t proc_port;
{ return bsd_init_process(proc_port); }
#else
(
	mach_port_t proc_port
);
#endif

/* Routine bsd_table_set */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_table_set
#if	defined(LINTLIBRARY)
    (proc_port, id, index, lel, nel, addr, addrCnt, nel_done)
	mach_port_t proc_port;
	int id;
	int index;
	int lel;
	int nel;
	small_char_array addr;
	mach_msg_type_number_t addrCnt;
	int *nel_done;
{ return bsd_table_set(proc_port, id, index, lel, nel, addr, addrCnt, nel_done); }
#else
(
	mach_port_t proc_port,
	int id,
	int index,
	int lel,
	int nel,
	small_char_array addr,
	mach_msg_type_number_t addrCnt,
	int *nel_done
);
#endif

/* Routine bsd_table_get */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_table_get
#if	defined(LINTLIBRARY)
    (proc_port, id, index, lel, nel, addr, addrCnt, nel_done)
	mach_port_t proc_port;
	int id;
	int index;
	int lel;
	int nel;
	char_array *addr;
	mach_msg_type_number_t *addrCnt;
	int *nel_done;
{ return bsd_table_get(proc_port, id, index, lel, nel, addr, addrCnt, nel_done); }
#else
(
	mach_port_t proc_port,
	int id,
	int index,
	int lel,
	int nel,
	char_array *addr,
	mach_msg_type_number_t *addrCnt,
	int *nel_done
);
#endif

/* Routine bsd_emulator_error */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_emulator_error
#if	defined(LINTLIBRARY)
    (proc_port, err_message, err_messageCnt)
	mach_port_t proc_port;
	small_char_array err_message;
	mach_msg_type_number_t err_messageCnt;
{ return bsd_emulator_error(proc_port, err_message, err_messageCnt); }
#else
(
	mach_port_t proc_port,
	small_char_array err_message,
	mach_msg_type_number_t err_messageCnt
);
#endif

/* Routine bsd_share_wakeup */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_share_wakeup
#if	defined(LINTLIBRARY)
    (proc_port, lock_offset)
	mach_port_t proc_port;
	int lock_offset;
{ return bsd_share_wakeup(proc_port, lock_offset); }
#else
(
	mach_port_t proc_port,
	int lock_offset
);
#endif

/* Routine bsd_maprw_request_it */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_maprw_request_it
#if	defined(LINTLIBRARY)
    (proc_port, fileno)
	mach_port_t proc_port;
	int fileno;
{ return bsd_maprw_request_it(proc_port, fileno); }
#else
(
	mach_port_t proc_port,
	int fileno
);
#endif

/* Routine bsd_maprw_release_it */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_maprw_release_it
#if	defined(LINTLIBRARY)
    (proc_port, fileno)
	mach_port_t proc_port;
	int fileno;
{ return bsd_maprw_release_it(proc_port, fileno); }
#else
(
	mach_port_t proc_port,
	int fileno
);
#endif

/* Routine bsd_maprw_remap */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_maprw_remap
#if	defined(LINTLIBRARY)
    (proc_port, fileno, offset, size)
	mach_port_t proc_port;
	int fileno;
	int offset;
	int size;
{ return bsd_maprw_remap(proc_port, fileno, offset, size); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int offset,
	int size
);
#endif

/* Routine bsd_pid_by_task */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_pid_by_task
#if	defined(LINTLIBRARY)
    (proc_port, task, pid, command, commandCnt)
	mach_port_t proc_port;
	mach_port_t task;
	int *pid;
	path_name_t command;
	mach_msg_type_number_t *commandCnt;
{ return bsd_pid_by_task(proc_port, task, pid, command, commandCnt); }
#else
(
	mach_port_t proc_port,
	mach_port_t task,
	int *pid,
	path_name_t command,
	mach_msg_type_number_t *commandCnt
);
#endif

/* Routine bsd_mon_switch */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_mon_switch
#if	defined(LINTLIBRARY)
    (proc_port, sample_flavor)
	mach_port_t proc_port;
	int *sample_flavor;
{ return bsd_mon_switch(proc_port, sample_flavor); }
#else
(
	mach_port_t proc_port,
	int *sample_flavor
);
#endif

/* Routine bsd_mon_dump */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_mon_dump
#if	defined(LINTLIBRARY)
    (proc_port, mon_data, mon_dataCnt)
	mach_port_t proc_port;
	char_array *mon_data;
	mach_msg_type_number_t *mon_dataCnt;
{ return bsd_mon_dump(proc_port, mon_data, mon_dataCnt); }
#else
(
	mach_port_t proc_port,
	char_array *mon_data,
	mach_msg_type_number_t *mon_dataCnt
);
#endif

/* Routine bsd_getattr */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_getattr
#if	defined(LINTLIBRARY)
    (proc_port, fileno, vattr)
	mach_port_t proc_port;
	int fileno;
	vattr_t *vattr;
{ return bsd_getattr(proc_port, fileno, vattr); }
#else
(
	mach_port_t proc_port,
	int fileno,
	vattr_t *vattr
);
#endif

/* Routine bsd_setattr */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_setattr
#if	defined(LINTLIBRARY)
    (proc_port, fileno, vattr)
	mach_port_t proc_port;
	int fileno;
	vattr_t vattr;
{ return bsd_setattr(proc_port, fileno, vattr); }
#else
(
	mach_port_t proc_port,
	int fileno,
	vattr_t vattr
);
#endif

/* Routine bsd_path_getattr */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_path_getattr
#if	defined(LINTLIBRARY)
    (proc_port, follow, path, pathCnt, vattr)
	mach_port_t proc_port;
	boolean_t follow;
	path_name_t path;
	mach_msg_type_number_t pathCnt;
	vattr_t *vattr;
{ return bsd_path_getattr(proc_port, follow, path, pathCnt, vattr); }
#else
(
	mach_port_t proc_port,
	boolean_t follow,
	path_name_t path,
	mach_msg_type_number_t pathCnt,
	vattr_t *vattr
);
#endif

/* Routine bsd_path_setattr */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_path_setattr
#if	defined(LINTLIBRARY)
    (proc_port, follow, path, pathCnt, vattr)
	mach_port_t proc_port;
	boolean_t follow;
	path_name_t path;
	mach_msg_type_number_t pathCnt;
	vattr_t vattr;
{ return bsd_path_setattr(proc_port, follow, path, pathCnt, vattr); }
#else
(
	mach_port_t proc_port,
	boolean_t follow,
	path_name_t path,
	mach_msg_type_number_t pathCnt,
	vattr_t vattr
);
#endif

/* Routine bsd_sysctl */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_sysctl
#if	defined(LINTLIBRARY)
    (proc_port, mib, mibCnt, miblen, old, oldCnt, oldlen, new, newCnt, newlen, retlen)
	mach_port_t proc_port;
	mib_t mib;
	mach_msg_type_number_t mibCnt;
	int miblen;
	char_array *old;
	mach_msg_type_number_t *oldCnt;
	size_t *oldlen;
	char_array new;
	mach_msg_type_number_t newCnt;
	size_t newlen;
	int *retlen;
{ return bsd_sysctl(proc_port, mib, mibCnt, miblen, old, oldCnt, oldlen, new, newCnt, newlen, retlen); }
#else
(
	mach_port_t proc_port,
	mib_t mib,
	mach_msg_type_number_t mibCnt,
	int miblen,
	char_array *old,
	mach_msg_type_number_t *oldCnt,
	size_t *oldlen,
	char_array new,
	mach_msg_type_number_t newCnt,
	size_t newlen,
	int *retlen
);
#endif

/* Routine bsd_set_atexpansion */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_set_atexpansion
#if	defined(LINTLIBRARY)
    (proc_port, what_to_expand, what_to_expandCnt, expansion, expansionCnt)
	mach_port_t proc_port;
	path_name_t what_to_expand;
	mach_msg_type_number_t what_to_expandCnt;
	path_name_t expansion;
	mach_msg_type_number_t expansionCnt;
{ return bsd_set_atexpansion(proc_port, what_to_expand, what_to_expandCnt, expansion, expansionCnt); }
#else
(
	mach_port_t proc_port,
	path_name_t what_to_expand,
	mach_msg_type_number_t what_to_expandCnt,
	path_name_t expansion,
	mach_msg_type_number_t expansionCnt
);
#endif

/* Routine bsd_sendmsg_short */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_sendmsg_short
#if	defined(LINTLIBRARY)
    (proc_port, fileno, flags, data, dataCnt, to, toCnt, cmsg, cmsgCnt, nsent)
	mach_port_t proc_port;
	int fileno;
	int flags;
	small_char_array data;
	mach_msg_type_number_t dataCnt;
	sockarg_t to;
	mach_msg_type_number_t toCnt;
	sockarg_t cmsg;
	mach_msg_type_number_t cmsgCnt;
	size_t *nsent;
{ return bsd_sendmsg_short(proc_port, fileno, flags, data, dataCnt, to, toCnt, cmsg, cmsgCnt, nsent); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int flags,
	small_char_array data,
	mach_msg_type_number_t dataCnt,
	sockarg_t to,
	mach_msg_type_number_t toCnt,
	sockarg_t cmsg,
	mach_msg_type_number_t cmsgCnt,
	size_t *nsent
);
#endif

/* Routine bsd_sendmsg_long */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_sendmsg_long
#if	defined(LINTLIBRARY)
    (proc_port, fileno, flags, data, dataCnt, to, toCnt, cmsg, cmsgCnt, nsent)
	mach_port_t proc_port;
	int fileno;
	int flags;
	char_array data;
	mach_msg_type_number_t dataCnt;
	sockarg_t to;
	mach_msg_type_number_t toCnt;
	sockarg_t cmsg;
	mach_msg_type_number_t cmsgCnt;
	size_t *nsent;
{ return bsd_sendmsg_long(proc_port, fileno, flags, data, dataCnt, to, toCnt, cmsg, cmsgCnt, nsent); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int flags,
	char_array data,
	mach_msg_type_number_t dataCnt,
	sockarg_t to,
	mach_msg_type_number_t toCnt,
	sockarg_t cmsg,
	mach_msg_type_number_t cmsgCnt,
	size_t *nsent
);
#endif

/* Routine bsd_recvmsg_short */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_recvmsg_short
#if	defined(LINTLIBRARY)
    (proc_port, fileno, flags, outflags, fromlen, nreceived, from, fromCnt, cmsglen, cmsg, cmsgCnt, datalen, data, dataCnt)
	mach_port_t proc_port;
	int fileno;
	int flags;
	int *outflags;
	int fromlen;
	size_t *nreceived;
	sockarg_t from;
	mach_msg_type_number_t *fromCnt;
	int cmsglen;
	sockarg_t cmsg;
	mach_msg_type_number_t *cmsgCnt;
	int datalen;
	small_char_array data;
	mach_msg_type_number_t *dataCnt;
{ return bsd_recvmsg_short(proc_port, fileno, flags, outflags, fromlen, nreceived, from, fromCnt, cmsglen, cmsg, cmsgCnt, datalen, data, dataCnt); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int flags,
	int *outflags,
	int fromlen,
	size_t *nreceived,
	sockarg_t from,
	mach_msg_type_number_t *fromCnt,
	int cmsglen,
	sockarg_t cmsg,
	mach_msg_type_number_t *cmsgCnt,
	int datalen,
	small_char_array data,
	mach_msg_type_number_t *dataCnt
);
#endif

/* Routine bsd_recvmsg_long */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_recvmsg_long
#if	defined(LINTLIBRARY)
    (proc_port, fileno, flags, outflags, nreceived, fromlen, from, fromCnt, cmsglen, cmsg, cmsgCnt, datalen, data, dataCnt)
	mach_port_t proc_port;
	int fileno;
	int flags;
	int *outflags;
	size_t *nreceived;
	int fromlen;
	sockarg_t from;
	mach_msg_type_number_t *fromCnt;
	int cmsglen;
	sockarg_t cmsg;
	mach_msg_type_number_t *cmsgCnt;
	int datalen;
	char_array *data;
	mach_msg_type_number_t *dataCnt;
{ return bsd_recvmsg_long(proc_port, fileno, flags, outflags, nreceived, fromlen, from, fromCnt, cmsglen, cmsg, cmsgCnt, datalen, data, dataCnt); }
#else
(
	mach_port_t proc_port,
	int fileno,
	int flags,
	int *outflags,
	size_t *nreceived,
	int fromlen,
	sockarg_t from,
	mach_msg_type_number_t *fromCnt,
	int cmsglen,
	sockarg_t cmsg,
	mach_msg_type_number_t *cmsgCnt,
	int datalen,
	char_array *data,
	mach_msg_type_number_t *dataCnt
);
#endif

/* Routine bsd_exec_done */
#ifdef	mig_external
mig_external
#else
extern
#endif
kern_return_t bsd_exec_done
#if	defined(LINTLIBRARY)
    (proc_port)
	mach_port_t proc_port;
{ return bsd_exec_done(proc_port); }
#else
(
	mach_port_t proc_port
);
#endif

#endif	/* not defined(_bsd_1_user_) */
