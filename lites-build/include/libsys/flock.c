/* 
 * ../libsys/flock.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int flock(int fd, int operation)
{
	errno_t err;

	err = e_flock(fd, operation);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

