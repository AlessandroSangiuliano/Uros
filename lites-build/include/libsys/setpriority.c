/* 
 * ../libsys/setpriority.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int setpriority(int which, int who, int prio)
{
	errno_t err;

	err = e_setpriority(which, who, prio);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

