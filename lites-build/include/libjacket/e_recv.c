/* 
 * ../libjacket/e_recv.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_recv(int s, char *buf, int len, int flags, int *nreceived)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*nreceived = recv(s, buf, len, flags);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

