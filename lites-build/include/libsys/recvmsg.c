/* 
 * ../libsys/recvmsg.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int recvmsg(int fd, struct msghdr *msg, int flags)
{
	int nreceived = 0;
	errno_t err;

	err = e_recvmsg(fd, msg, flags, &nreceived);
	if (err != 0) {
		nreceived = -1;
		errno = err;
	}
	return nreceived;
}

