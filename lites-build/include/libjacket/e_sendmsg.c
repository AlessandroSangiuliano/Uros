/* 
 * ../libjacket/e_sendmsg.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_sendmsg(int s, const struct msghdr *msg, int flags, int *nsent)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*nsent = sendmsg(s, msg, flags);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

