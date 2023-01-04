/* 
 * ../libsys/send.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int send(int s, char *msg, int len, int flags)
{
	int nsent = 0;
	errno_t err;

	err = e_send(s, msg, len, flags, &nsent);
	if (err != 0) {
		nsent = -1;
		errno = err;
	}
	return nsent;
}
