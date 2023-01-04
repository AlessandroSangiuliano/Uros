/* 
 * ../libjacket/e_socketpair.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_socketpair(int domain, int type, int protocol, int *sv)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	socketpair(domain, type, protocol, sv);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

