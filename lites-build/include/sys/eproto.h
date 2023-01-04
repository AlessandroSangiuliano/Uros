/*
 * Re-entrant system call prototypes.
 *
 * DO NOT EDIT-- this file is automatically generated.
 * created from/home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#if KERNEL
#include <nfs.h>
#include <sysvshm.h>
#include <map_uarea.h>
#include <ktrace.h>
#include <trace.h>
#endif
extern noreturn exit(int status);
extern errno_t e_fork(pid_t *pid);
extern errno_t e_read(int fd, void *buf, size_t nbytes, size_t *nread);
extern errno_t e_write(int fd, const void *buf, size_t nbytes, size_t *nwritten);
extern errno_t e_open(const char *path, int flags, int mode, int *fd);
extern errno_t e_close(int fd);
extern errno_t e_wait4(pid_t pid, int *status, int options, struct rusage *rusage, pid_t *wpid);
extern errno_t e_creat(char *name, mode_t mode, int *fd);
extern errno_t e_link(const char *target, const char *linkname);
extern errno_t e_unlink(const char *path);
extern errno_t e_chdir(const char *path);
extern errno_t e_fchdir(const char *path);
extern errno_t e_mknod(const char *path, mode_t mode, dev_t dev);
extern errno_t e_chmod(const char *path, mode_t mode);
extern errno_t e_chown(const char *path, uid_t owner, gid_t group);
extern errno_t e_obreak(const char *addr);
extern errno_t e_getfsstat(struct statfs *buf, long bufsize, int flags, int *nstructs);
extern errno_t e_lseek(int fd, off_t offset, int sbase, off_t *pos);
extern errno_t e_getpid(pid_t *pid);
extern errno_t e_mount(int type, const char *dir, int flags, void * data);
extern errno_t e_unmount(const char *path, int flags);
extern errno_t e_setuid(uid_t uid);
extern errno_t e_getuid(uid_t *uid);
extern errno_t e_geteuid(uid_t *euid);
extern errno_t e_ptrace(int request, pid_t pid, int *addr, int data);
extern errno_t e_recvmsg(int fd, struct msghdr *msg, int flags, int *nreceived);
extern errno_t e_sendmsg(int s, const struct msghdr *msg, int flags, int *nsent);
extern errno_t e_recvfrom(int s, void *buf, int len, int flags, struct sockaddr *from, int *fromlen, int *nreceived);
extern errno_t e_accept(int s, struct sockaddr *addr, int *addrlen, int *fd);
extern errno_t e_getpeername(int s, struct sockaddr *name, int *namelen);
extern errno_t e_getsockname(int s, struct sockaddr *name, int *namelen);
extern errno_t e_saccess(const char *path, int amode);
extern errno_t e_chflags(const char *path, int flags);
extern errno_t e_fchflags(int fd, int flags);
extern errno_t e_sync(void);
extern errno_t e_kill(pid_t pid, int sig);
extern errno_t e_stat(const char *path, struct stat *buf);
extern errno_t e_getppid(pid_t *pid);
extern errno_t e_lstat(const char *path, struct stat *buf);
extern errno_t e_dup(int fd, int *nfd);
extern errno_t e_pipe(int fds[2]);
extern errno_t e_getegid(gid_t *gid);
extern errno_t e_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
extern errno_t e_getgid(gid_t *gid);
extern errno_t e_sigprocmask(int how, const sigset_t *set, sigset_t *oset);
extern errno_t e_getlogin(char* *name);
extern errno_t e_setlogin(const char *name);
extern errno_t e_sysacct(const char *file);
#if MAP_UAREA
extern errno_t e_sigpending(sigset_t *set);
#else
extern errno_t e_sigpending(sigset_t *set);
#endif
#ifdef notyet
#else
#endif
extern errno_t e_ioctl(int fd, unsigned int cmd, char *argp);
extern errno_t e_reboot(int howto);
extern errno_t e_revoke(char *fname);
extern errno_t e_symlink(const char *oname, const char *nname);
extern errno_t e_readlink(const char *path, char *buf, int bufsize, int *nread);
extern errno_t e_execve(const char *path, const char *argv[], const char *envp[]);
#if NMAP_UAREA
extern errno_t e_umask(mode_t mask, mode_t *omask);
#else
extern errno_t e_umask(mode_t mask, mode_t *omask);
#endif
extern errno_t e_chroot(const char *dirname);
extern errno_t e_fstat(int fd, struct stat *buf);
extern errno_t e_getpagesize(int *bytes);
extern errno_t e_msync(caddr_t addr, int len);
extern errno_t e_vfork(pid_t *pid);
extern errno_t e_sbrk(int incr, caddr_t *obrk);
extern errno_t e_smmap(caddr_t addr, size_t len, int prot, int flags, int fd, off_t offset, caddr_t *addr_out);
extern errno_t e_munmap(caddr_t addr, int len);
extern errno_t e_mprotect(caddr_t addr, int len, int prot);
extern errno_t e_madvise(caddr_t addr, int len, int behav);
extern errno_t e_mincore(caddr_t addr, int len, char *vec);
extern errno_t e_getgroups(u_int gidsetsize, int *gidset, int *ngroups);
extern errno_t e_setgroups(int gidsetsize, gid_t *gidset);
extern errno_t e_getpgrp(pid_t *pgrp);
extern errno_t e_setpgid(pid_t pid, pid_t pgrp);
extern errno_t e_setitimer(int which, struct itimerval *value, struct itimerval *ovalue);
extern errno_t e_wait(int *retval);
extern errno_t e_swapon(const char *special);
extern errno_t e_getitimer(int which, struct itimervalue *value);
extern errno_t e_gethostname(char *name, int namelen);
extern errno_t e_sethostname(const char *name, int namelen);
extern errno_t e_getdtablesize(int *size);
extern errno_t e_dup2(int ofd, int nfd);
extern errno_t e_fcntl(int fd, int cmd, int arg, int *whatever);
extern errno_t e_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout, int *nready);
extern errno_t e_fsync(int fd);
extern errno_t e_setpriority(int which, int who, int prio);
extern errno_t e_socket(int domain, int type, int protocol, int *fd);
extern errno_t e_connect(int fd, const struct sockaddr *name, int namelen);
extern errno_t e_getpriority(int which, int who, int *prio);
extern errno_t e_send(int s, char *msg, int len, int flags, int *nsent);
extern errno_t e_recv(int s, char *buf, int len, int flags, int *nreceived);
extern errno_t e_sigreturn(struct sigcontext *scp);
extern errno_t e_bind(int fd, const struct sockaddr *name, int namelen);
extern errno_t e_setsockopt(int fd, int level, int optname, const void *optval, int optlen);
extern errno_t e_listen(int fd, int backlog);
extern errno_t e_sigvec(int sig, struct sigvec *vec, struct sigvec *ovec);
#if MAP_UAREA
#else
#endif
extern errno_t e_sigsuspend(const sigset_t *sigmask);
extern errno_t e_gettimeofday(struct timeval *tp, struct timezone *tzp);
extern errno_t e_getrusage(int who, struct rusage *rusage);
extern errno_t e_getsockopt(int fd, int level, int optname, void *optval, int *optlen);
extern errno_t e_readv(int fd, struct iovec *iov, int iovcnt, int *nread);
extern errno_t e_writev(int fd, struct iovec *iov, int iovcnt, int *nwritten);
extern errno_t e_settimeofday(struct timeval *tp, struct timezone *tzp);
extern errno_t e_fchown(int fd, uid_t owner, gid_t group);
extern errno_t e_fchmod(int fd, mode_t mode);
extern errno_t e_rename(const char *old, const char *new);
extern errno_t e_truncate(const char *path, off_t length);
extern errno_t e_ftruncate(int fd, off_t length);
extern errno_t e_flock(int fd, int operation);
extern errno_t e_mkfifo(const char *path, mode_t mode);
extern errno_t e_sendto(int s, const void *msg, int len, int flags, const struct sockaddr *to, int tolen, int *nsent);
extern errno_t e_shutdown(int fd, int how);
extern errno_t e_socketpair(int domain, int type, int protocol, int *sv);
extern errno_t e_mkdir(const char *path, mode_t mode);
extern errno_t e_rmdir(const char *path);
extern errno_t e_utimes(const char *file, const struct timeval *times);
extern errno_t e_adjtime(struct timeval *delta, struct timeval *olddelta);
extern errno_t e_gethostid(int *id);
extern errno_t e_sethostid(int hostid);
#if MAP_UAREA
extern errno_t e_getrlimit(int resource, struct rlimit *rlp);
#else
extern errno_t e_getrlimit(int resource, struct rlimit *rlp);
#endif
extern errno_t e_setrlimit(int resource, const struct rlimit *rlp);
extern errno_t e_setsid(pid_t *pgrp);
extern errno_t e_quotactl(const char *path, int cmd, int id, char *addr);
extern errno_t e_quota(int a1, int a2, int a3, int a4, int *retval);
#if NFS
extern errno_t e_nfssvc(int sock, struct sockaddr *mask, int mask_length, struct sockaddr *match, int match_length);
#else
#endif
extern errno_t e_getdirentries(int fd, char *buf, int nbytes, long *basep, int *nread);
extern errno_t e_statfs(const char *path, struct statfs *buf);
extern errno_t e_fstatfs(int fd, struct statfs *buf);
#if NFS
extern errno_t e_getfh(const char *path, struct fhandle *fhp);
#else
#endif
extern errno_t e_getdomainname(char *name, int namelen);
extern errno_t e_setdomainname(const char *name, int namelen);
extern errno_t e_uname(struct utsname *name);
#if SYSVSHM
extern errno_t e_shmsys(int a1, int a2, int a3, int a4, int *retval);
#else
#endif
extern errno_t e_setgid(gid_t gid);
extern errno_t e_setegid(gid_t egid);
extern errno_t e_seteuid(uid_t euid);
extern errno_t e_table(int id, int index, char *addr, int nel, u_int lel, int *cel);
extern errno_t e_sysctrace(pid_t pid);
extern errno_t e___sysctl(int *name, unsigned int namelen, void *old, size_t *oldlenp, void *new, size_t newlen, int *retlen);
