/* 
 * ../libsys/setitimer.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	errno_t err;

	err = e_setitimer(which, value, ovalue);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

