/* 
 * ../libjacket/e_getitimer.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_getitimer(int which, struct itimervalue *value)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	getitimer(which, value);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

