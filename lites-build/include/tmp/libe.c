noreturn exit(int status)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int status;
	} a;
	a.status = status;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_exit, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*X = rv[0];
	return err;
}

errno_t e_fork(pid_t *pid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fork, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pid = rv[0];
	return err;
}

errno_t e_read(int fd, void *buf, size_t nbytes, size_t *nread)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		void *buf;
		size_t nbytes;
	} a;
	a.fd = fd;
	a.buf = buf;
	a.nbytes = nbytes;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_read, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nread = rv[0];
	return err;
}

errno_t e_write(int fd, const void *buf, size_t nbytes, size_t *nwritten)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		const void *buf;
		size_t nbytes;
	} a;
	a.fd = fd;
	a.buf = buf;
	a.nbytes = nbytes;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_write, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nwritten = rv[0];
	return err;
}

errno_t e_open(const char *path, int flags, int mode, int *fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		int flags;
		int mode;
	} a;
	a.path = path;
	a.flags = flags;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_open, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*fd = rv[0];
	return err;
}

errno_t e_close(int fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
	} a;
	a.fd = fd;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_close, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_wait4(pid_t pid, int *status, int options, struct rusage *rusage, pid_t *wpid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		pid_t pid;
		int *status;
		int options;
		struct rusage *rusage;
	} a;
	a.pid = pid;
	a.status = status;
	a.options = options;
	a.rusage = rusage;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_wait4, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*wpid = rv[0];
	return err;
}

errno_t e_creat(char *name, mode_t mode, int *fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		char *name;
		mode_t mode;
	} a;
	a.name = name;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_creat, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*fd = rv[0];
	return err;
}

errno_t e_link(const char *target, const char *linkname)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *target;
		const char *linkname;
	} a;
	a.target = target;
	a.linkname = linkname;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_link, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_unlink(const char *path)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
	} a;
	a.path = path;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_unlink, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_chdir(const char *path)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
	} a;
	a.path = path;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_chdir, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fchdir(const char *path)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
	} a;
	a.path = path;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fchdir, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_mknod(const char *path, mode_t mode, dev_t dev)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		mode_t mode;
		dev_t dev;
	} a;
	a.path = path;
	a.mode = mode;
	a.dev = dev;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mknod, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_chmod(const char *path, mode_t mode)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		mode_t mode;
	} a;
	a.path = path;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_chmod, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_chown(const char *path, uid_t owner, gid_t group)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		uid_t owner;
		gid_t group;
	} a;
	a.path = path;
	a.owner = owner;
	a.group = group;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_chown, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_obreak(const char *addr)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *addr;
	} a;
	a.addr = addr;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_brk, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getfsstat(struct statfs *buf, long bufsize, int flags, int *nstructs)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct statfs *buf;
		long bufsize;
		int flags;
	} a;
	a.buf = buf;
	a.bufsize = bufsize;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getfsstat, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nstructs = rv[0];
	return err;
}

errno_t e_lseek(int fd, off_t offset, int sbase, off_t *pos)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		off_t offset;
		int sbase;
	} a;
	a.fd = fd;
	a.offset = offset;
	a.sbase = sbase;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_lseek, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pos = rv[0];
	return err;
}

errno_t e_getpid(pid_t *pid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getpid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pid = rv[0];
	return err;
}

errno_t e_mount(int type, const char *dir, int flags, void * data)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int type;
		const char *dir;
		int flags;
		void * data;
	} a;
	a.type = type;
	a.dir = dir;
	a.flags = flags;
	a.data = data;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mount, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_unmount(const char *path, int flags)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		int flags;
	} a;
	a.path = path;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_unmount, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setuid(uid_t uid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		uid_t uid;
	} a;
	a.uid = uid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setuid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getuid(uid_t *uid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getuid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*uid = rv[0];
	return err;
}

errno_t e_geteuid(uid_t *euid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_geteuid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*euid = rv[0];
	return err;
}

errno_t e_ptrace(int request, pid_t pid, int *addr, int data)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int request;
		pid_t pid;
		int *addr;
		int data;
	} a;
	a.request = request;
	a.pid = pid;
	a.addr = addr;
	a.data = data;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_ptrace, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_recvmsg(int fd, struct msghdr *msg, int flags, int *nreceived)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		struct msghdr *msg;
		int flags;
	} a;
	a.fd = fd;
	a.msg = msg;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_recvmsg, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nreceived = rv[0];
	return err;
}

errno_t e_sendmsg(int s, const struct msghdr *msg, int flags, int *nsent)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		const struct msghdr *msg;
		int flags;
	} a;
	a.s = s;
	a.msg = msg;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sendmsg, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nsent = rv[0];
	return err;
}

errno_t e_recvfrom(int s, void *buf, int len, int flags, struct sockaddr *from, int *fromlen, int *nreceived)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		void *buf;
		int len;
		int flags;
		struct sockaddr *from;
		int *fromlen;
	} a;
	a.s = s;
	a.buf = buf;
	a.len = len;
	a.flags = flags;
	a.from = from;
	a.fromlen = fromlen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_recvfrom, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nreceived = rv[0];
	return err;
}

errno_t e_accept(int s, struct sockaddr *addr, int *addrlen, int *fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		struct sockaddr *addr;
		int *addrlen;
	} a;
	a.s = s;
	a.addr = addr;
	a.addrlen = addrlen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_accept, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*fd = rv[0];
	return err;
}

errno_t e_getpeername(int s, struct sockaddr *name, int *namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		struct sockaddr *name;
		int *namelen;
	} a;
	a.s = s;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getpeername, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getsockname(int s, struct sockaddr *name, int *namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		struct sockaddr *name;
		int *namelen;
	} a;
	a.s = s;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getsockname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_saccess(const char *path, int amode)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		int amode;
	} a;
	a.path = path;
	a.amode = amode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_access, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_chflags(const char *path, int flags)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		int flags;
	} a;
	a.path = path;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_chflags, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fchflags(int fd, int flags)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int flags;
	} a;
	a.fd = fd;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fchflags, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sync()
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sync, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_kill(pid_t pid, int sig)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		pid_t pid;
		int sig;
	} a;
	a.pid = pid;
	a.sig = sig;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_kill, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_stat(const char *path, struct stat *buf)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		struct stat *buf;
	} a;
	a.path = path;
	a.buf = buf;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_stat, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getppid(pid_t *pid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getppid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pid = rv[0];
	return err;
}

errno_t e_lstat(const char *path, struct stat *buf)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		struct stat *buf;
	} a;
	a.path = path;
	a.buf = buf;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_lstat, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_dup(int fd, int *nfd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
	} a;
	a.fd = fd;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_dup, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nfd = rv[0];
	return err;
}

errno_t e_pipe(int fds[2])
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fds[2];
	} a;
	a.fds = fds;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_pipe, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getegid(gid_t *gid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getegid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*gid = rv[0];
	return err;
}

errno_t e_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int sig;
		const struct sigaction *act;
		struct sigaction *oact;
	} a;
	a.sig = sig;
	a.act = act;
	a.oact = oact;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigaction, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getgid(gid_t *gid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getgid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*gid = rv[0];
	return err;
}

errno_t e_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int how;
		const sigset_t *set;
		sigset_t *oset;
	} a;
	a.how = how;
	a.set = set;
	a.oset = oset;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigprocmask, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getlogin(char* *name)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getlogin, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*name = rv[0];
	return err;
}

errno_t e_setlogin(const char *name)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *name;
	} a;
	a.name = name;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setlogin, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sysacct(const char *file)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *file;
	} a;
	a.file = file;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_acct, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sigpending(sigset_t *set)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		sigset_t *set;
	} a;
	a.set = set;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigpending, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sigpending(sigset_t *set)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		sigset_t *set;
	} a;
	a.set = set;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigpending, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_ioctl(int fd, unsigned int cmd, char *argp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		unsigned int cmd;
		char *argp;
	} a;
	a.fd = fd;
	a.cmd = cmd;
	a.argp = argp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_ioctl, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_reboot(int howto)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int howto;
	} a;
	a.howto = howto;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_reboot, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_revoke(char *fname)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		char *fname;
	} a;
	a.fname = fname;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_revoke, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_symlink(const char *oname, const char *nname)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *oname;
		const char *nname;
	} a;
	a.oname = oname;
	a.nname = nname;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_symlink, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_readlink(const char *path, char *buf, int bufsize, int *nread)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		char *buf;
		int bufsize;
	} a;
	a.path = path;
	a.buf = buf;
	a.bufsize = bufsize;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_readlink, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nread = rv[0];
	return err;
}

errno_t e_execve(const char *path, const char *argv[], const char *envp[])
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		const char *argv[];
		const char *envp[];
	} a;
	a.path = path;
	a.argv = argv;
	a.envp = envp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_execve, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_umask(mode_t mask, mode_t *omask)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		mode_t mask;
	} a;
	a.mask = mask;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_umask, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*omask = rv[0];
	return err;
}

errno_t e_umask(mode_t mask, mode_t *omask)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		mode_t mask;
	} a;
	a.mask = mask;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_umask, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*omask = rv[0];
	return err;
}

errno_t e_chroot(const char *dirname)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *dirname;
	} a;
	a.dirname = dirname;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_chroot, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fstat(int fd, struct stat *buf)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		struct stat *buf;
	} a;
	a.fd = fd;
	a.buf = buf;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fstat, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getpagesize(int *bytes)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getpagesize, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*bytes = rv[0];
	return err;
}

errno_t e_msync(caddr_t addr, int len)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		int len;
	} a;
	a.addr = addr;
	a.len = len;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_msync, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_vfork(pid_t *pid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_vfork, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pid = rv[0];
	return err;
}

errno_t e_sbrk(int incr, caddr_t *obrk)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int incr;
	} a;
	a.incr = incr;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sbrk, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*obrk = rv[0];
	return err;
}

errno_t e_smmap(caddr_t addr, size_t len, int prot, int flags, int fd, off_t offset, caddr_t *addr_out)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		size_t len;
		int prot;
		int flags;
		int fd;
		off_t offset;
	} a;
	a.addr = addr;
	a.len = len;
	a.prot = prot;
	a.flags = flags;
	a.fd = fd;
	a.offset = offset;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mmap, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*addr_out = rv[0];
	return err;
}

errno_t e_munmap(caddr_t addr, int len)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		int len;
	} a;
	a.addr = addr;
	a.len = len;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_munmap, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_mprotect(caddr_t addr, int len, int prot)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		int len;
		int prot;
	} a;
	a.addr = addr;
	a.len = len;
	a.prot = prot;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mprotect, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_madvise(caddr_t addr, int len, int behav)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		int len;
		int behav;
	} a;
	a.addr = addr;
	a.len = len;
	a.behav = behav;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_madvise, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_mincore(caddr_t addr, int len, char *vec)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		caddr_t addr;
		int len;
		char *vec;
	} a;
	a.addr = addr;
	a.len = len;
	a.vec = vec;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mincore, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getgroups(u_int gidsetsize, int *gidset, int *ngroups)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		u_int gidsetsize;
		int *gidset;
	} a;
	a.gidsetsize = gidsetsize;
	a.gidset = gidset;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getgroups, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*ngroups = rv[0];
	return err;
}

errno_t e_setgroups(int gidsetsize, gid_t *gidset)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int gidsetsize;
		gid_t *gidset;
	} a;
	a.gidsetsize = gidsetsize;
	a.gidset = gidset;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setgroups, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getpgrp(pid_t *pgrp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getpgrp, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pgrp = rv[0];
	return err;
}

errno_t e_setpgid(pid_t pid, pid_t pgrp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		pid_t pid;
		pid_t pgrp;
	} a;
	a.pid = pid;
	a.pgrp = pgrp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setpgid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int which;
		struct itimerval *value;
		struct itimerval *ovalue;
	} a;
	a.which = which;
	a.value = value;
	a.ovalue = ovalue;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setitimer, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_wait(int *retval)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_wait, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*retval = rv[0];
	return err;
}

errno_t e_swapon(const char *special)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *special;
	} a;
	a.special = special;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_swapon, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getitimer(int which, struct itimervalue *value)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int which;
		struct itimervalue *value;
	} a;
	a.which = which;
	a.value = value;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getitimer, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_gethostname(char *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		char *name;
		int namelen;
	} a;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_gethostname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sethostname(const char *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *name;
		int namelen;
	} a;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sethostname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getdtablesize(int *size)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getdtablesize, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*size = rv[0];
	return err;
}

errno_t e_dup2(int ofd, int nfd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int ofd;
		int nfd;
	} a;
	a.ofd = ofd;
	a.nfd = nfd;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_dup2, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fcntl(int fd, int cmd, int arg, int *whatever)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int cmd;
		int arg;
	} a;
	a.fd = fd;
	a.cmd = cmd;
	a.arg = arg;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fcntl, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*whatever = rv[0];
	return err;
}

errno_t e_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout, int *nready)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int nfds;
		fd_set *readfds;
		fd_set *writefds;
		fd_set *exceptfds;
		struct timeval *timeout;
	} a;
	a.nfds = nfds;
	a.readfds = readfds;
	a.writefds = writefds;
	a.exceptfds = exceptfds;
	a.timeout = timeout;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_select, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nready = rv[0];
	return err;
}

errno_t e_fsync(int fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
	} a;
	a.fd = fd;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fsync, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setpriority(int which, int who, int prio)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int which;
		int who;
		int prio;
	} a;
	a.which = which;
	a.who = who;
	a.prio = prio;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setpriority, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_socket(int domain, int type, int protocol, int *fd)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int domain;
		int type;
		int protocol;
	} a;
	a.domain = domain;
	a.type = type;
	a.protocol = protocol;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_socket, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*fd = rv[0];
	return err;
}

errno_t e_connect(int fd, const struct sockaddr *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		const struct sockaddr *name;
		int namelen;
	} a;
	a.fd = fd;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_connect, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getpriority(int which, int who, int *prio)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int which;
		int who;
	} a;
	a.which = which;
	a.who = who;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getpriority, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*prio = rv[0];
	return err;
}

errno_t e_send(int s, char *msg, int len, int flags, int *nsent)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		char *msg;
		int len;
		int flags;
	} a;
	a.s = s;
	a.msg = msg;
	a.len = len;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_send, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nsent = rv[0];
	return err;
}

errno_t e_recv(int s, char *buf, int len, int flags, int *nreceived)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		char *buf;
		int len;
		int flags;
	} a;
	a.s = s;
	a.buf = buf;
	a.len = len;
	a.flags = flags;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_recv, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nreceived = rv[0];
	return err;
}

errno_t e_sigreturn(struct sigcontext *scp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct sigcontext *scp;
	} a;
	a.scp = scp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigreturn, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_bind(int fd, const struct sockaddr *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		const struct sockaddr *name;
		int namelen;
	} a;
	a.fd = fd;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_bind, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setsockopt(int fd, int level, int optname, const void *optval, int optlen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int level;
		int optname;
		const void *optval;
		int optlen;
	} a;
	a.fd = fd;
	a.level = level;
	a.optname = optname;
	a.optval = optval;
	a.optlen = optlen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setsockopt, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_listen(int fd, int backlog)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int backlog;
	} a;
	a.fd = fd;
	a.backlog = backlog;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_listen, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sigvec(int sig, struct sigvec *vec, struct sigvec *ovec)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int sig;
		struct sigvec *vec;
		struct sigvec *ovec;
	} a;
	a.sig = sig;
	a.vec = vec;
	a.ovec = ovec;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigvec, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sigsuspend(const sigset_t *sigmask)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const sigset_t *sigmask;
	} a;
	a.sigmask = sigmask;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sigsuspend, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_gettimeofday(struct timeval *tp, struct timezone *tzp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct timeval *tp;
		struct timezone *tzp;
	} a;
	a.tp = tp;
	a.tzp = tzp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_gettimeofday, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getrusage(int who, struct rusage *rusage)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int who;
		struct rusage *rusage;
	} a;
	a.who = who;
	a.rusage = rusage;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getrusage, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getsockopt(int fd, int level, int optname, void *optval, int *optlen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int level;
		int optname;
		void *optval;
		int *optlen;
	} a;
	a.fd = fd;
	a.level = level;
	a.optname = optname;
	a.optval = optval;
	a.optlen = optlen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getsockopt, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_readv(int fd, struct iovec *iov, int iovcnt, int *nread)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		struct iovec *iov;
		int iovcnt;
	} a;
	a.fd = fd;
	a.iov = iov;
	a.iovcnt = iovcnt;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_readv, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nread = rv[0];
	return err;
}

errno_t e_writev(int fd, struct iovec *iov, int iovcnt, int *nwritten)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		struct iovec *iov;
		int iovcnt;
	} a;
	a.fd = fd;
	a.iov = iov;
	a.iovcnt = iovcnt;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_writev, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nwritten = rv[0];
	return err;
}

errno_t e_settimeofday(struct timeval *tp, struct timezone *tzp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct timeval *tp;
		struct timezone *tzp;
	} a;
	a.tp = tp;
	a.tzp = tzp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_settimeofday, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fchown(int fd, uid_t owner, gid_t group)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		uid_t owner;
		gid_t group;
	} a;
	a.fd = fd;
	a.owner = owner;
	a.group = group;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fchown, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fchmod(int fd, mode_t mode)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		mode_t mode;
	} a;
	a.fd = fd;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fchmod, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_rename(const char *old, const char *new)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *old;
		const char *new;
	} a;
	a.old = old;
	a.new = new;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_rename, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_truncate(const char *path, off_t length)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		off_t length;
	} a;
	a.path = path;
	a.length = length;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_truncate, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_ftruncate(int fd, off_t length)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		off_t length;
	} a;
	a.fd = fd;
	a.length = length;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_ftruncate, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_flock(int fd, int operation)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int operation;
	} a;
	a.fd = fd;
	a.operation = operation;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_flock, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_mkfifo(const char *path, mode_t mode)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		mode_t mode;
	} a;
	a.path = path;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mkfifo, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_sendto(int s, const void *msg, int len, int flags, const struct sockaddr *to, int tolen, int *nsent)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int s;
		const void *msg;
		int len;
		int flags;
		const struct sockaddr *to;
		int tolen;
	} a;
	a.s = s;
	a.msg = msg;
	a.len = len;
	a.flags = flags;
	a.to = to;
	a.tolen = tolen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sendto, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nsent = rv[0];
	return err;
}

errno_t e_shutdown(int fd, int how)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		int how;
	} a;
	a.fd = fd;
	a.how = how;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_shutdown, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_socketpair(int domain, int type, int protocol, int *sv)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int domain;
		int type;
		int protocol;
		int *sv;
	} a;
	a.domain = domain;
	a.type = type;
	a.protocol = protocol;
	a.sv = sv;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_socketpair, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_mkdir(const char *path, mode_t mode)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		mode_t mode;
	} a;
	a.path = path;
	a.mode = mode;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_mkdir, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_rmdir(const char *path)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
	} a;
	a.path = path;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_rmdir, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_utimes(const char *file, const struct timeval *times)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *file;
		const struct timeval *times;
	} a;
	a.file = file;
	a.times = times;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_utimes, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_adjtime(struct timeval *delta, struct timeval *olddelta)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct timeval *delta;
		struct timeval *olddelta;
	} a;
	a.delta = delta;
	a.olddelta = olddelta;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_adjtime, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_gethostid(int *id)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_gethostid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*id = rv[0];
	return err;
}

errno_t e_sethostid(int hostid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int hostid;
	} a;
	a.hostid = hostid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sethostid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getrlimit(int resource, struct rlimit *rlp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int resource;
		struct rlimit *rlp;
	} a;
	a.resource = resource;
	a.rlp = rlp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getrlimit, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getrlimit(int resource, struct rlimit *rlp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int resource;
		struct rlimit *rlp;
	} a;
	a.resource = resource;
	a.rlp = rlp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getrlimit, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setrlimit(int resource, const struct rlimit *rlp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int resource;
		const struct rlimit *rlp;
	} a;
	a.resource = resource;
	a.rlp = rlp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setrlimit, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setsid(pid_t *pgrp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int dummy;
	} a;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setsid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*pgrp = rv[0];
	return err;
}

errno_t e_quotactl(const char *path, int cmd, int id, char *addr)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		int cmd;
		int id;
		char *addr;
	} a;
	a.path = path;
	a.cmd = cmd;
	a.id = id;
	a.addr = addr;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_quotactl, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_quota(int a1, int a2, int a3, int a4, int *retval)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int a1;
		int a2;
		int a3;
		int a4;
	} a;
	a.a1 = a1;
	a.a2 = a2;
	a.a3 = a3;
	a.a4 = a4;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_quota, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*retval = rv[0];
	return err;
}

errno_t e_nfssvc(int sock, struct sockaddr *mask, int mask_length, struct sockaddr *match, int match_length)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int sock;
		struct sockaddr *mask;
		int mask_length;
		struct sockaddr *match;
		int match_length;
	} a;
	a.sock = sock;
	a.mask = mask;
	a.mask_length = mask_length;
	a.match = match;
	a.match_length = match_length;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_nfssvc, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getdirentries(int fd, char *buf, int nbytes, long *basep, int *nread)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		char *buf;
		int nbytes;
		long *basep;
	} a;
	a.fd = fd;
	a.buf = buf;
	a.nbytes = nbytes;
	a.basep = basep;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getdirentries, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*nread = rv[0];
	return err;
}

errno_t e_statfs(const char *path, struct statfs *buf)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		struct statfs *buf;
	} a;
	a.path = path;
	a.buf = buf;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_statfs, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_fstatfs(int fd, struct statfs *buf)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int fd;
		struct statfs *buf;
	} a;
	a.fd = fd;
	a.buf = buf;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_fstatfs, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getfh(const char *path, struct fhandle *fhp)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *path;
		struct fhandle *fhp;
	} a;
	a.path = path;
	a.fhp = fhp;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getfh, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_getdomainname(char *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		char *name;
		int namelen;
	} a;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_getdomainname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setdomainname(const char *name, int namelen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		const char *name;
		int namelen;
	} a;
	a.name = name;
	a.namelen = namelen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setdomainname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_uname(struct utsname *name)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		struct utsname *name;
	} a;
	a.name = name;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_uname, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_shmsys(int a1, int a2, int a3, int a4, int *retval)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int a1;
		int a2;
		int a3;
		int a4;
	} a;
	a.a1 = a1;
	a.a2 = a2;
	a.a3 = a3;
	a.a4 = a4;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_shmsys, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*retval = rv[0];
	return err;
}

errno_t e_setgid(gid_t gid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		gid_t gid;
	} a;
	a.gid = gid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setgid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_setegid(gid_t egid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		gid_t egid;
	} a;
	a.egid = egid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_setegid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_seteuid(uid_t euid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		uid_t euid;
	} a;
	a.euid = euid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_seteuid, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e_table(int id, int index, char *addr, int nel, u_int lel, int *cel)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int id;
		int index;
		char *addr;
		int nel;
		u_int lel;
	} a;
	a.id = id;
	a.index = index;
	a.addr = addr;
	a.nel = nel;
	a.lel = lel;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_table, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*cel = rv[0];
	return err;
}

errno_t e_sysctrace(pid_t pid)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		pid_t pid;
	} a;
	a.pid = pid;

	kr = emul_generic(our_bsd_server_port, &intr, SYS_sysctrace, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	return err;
}

errno_t e___sysctl(int *name, unsigned int namelen, void *old, size_t *oldlenp, void *new, size_t newlen, int *retlen)
{
	errno_t err;
	kern_return_t kr;
	int intr, rv[2];

	struct {
		int *name;
		unsigned int namelen;
		void *old;
		size_t *oldlenp;
		void *new;
		size_t newlen;
	} a;
	a.name = name;
	a.namelen = namelen;
	a.old = old;
	a.oldlenp = oldlenp;
	a.new = new;
	a.newlen = newlen;

	kr = emul_generic(our_bsd_server_port, &intr, SYS___sysctl, &a, &rv);
	err = kr ? e_mach_error_to_errno(kr) : 0;
	if (err == 0)
		*retlen = rv[0];
	return err;
}

