/* 
 * ../libsys/sysctrace.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int sysctrace(pid_t pid)
{
	errno_t err;

	err = e_sysctrace(pid);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
