/* 
 * ../libjacket/e_getpeername.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_getpeername(int s, struct sockaddr *name, int *namelen)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	getpeername(s, name, namelen);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

