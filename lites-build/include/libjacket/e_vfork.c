/* 
 * ../libjacket/e_vfork.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_vfork(pid_t *pid)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*pid = vfork();
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

