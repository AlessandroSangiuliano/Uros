/* 
 * ../libjacket/e_recvmsg.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_recvmsg(int fd, struct msghdr *msg, int flags, int *nreceived)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*nreceived = recvmsg(fd, msg, flags);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

