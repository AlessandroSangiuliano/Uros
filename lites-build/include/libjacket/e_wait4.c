/* 
 * ../libjacket/e_wait4.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_wait4(pid_t pid, int *status, int options, struct rusage *rusage, pid_t *wpid)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*wpid = wait4(pid, status, options, rusage);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

