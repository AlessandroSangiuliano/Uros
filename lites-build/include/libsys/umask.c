/* 
 * ../libsys/umask.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

mode_t umask(mode_t mask)
{
	mode_t omask = 0;
	errno_t err;

	err = e_umask(mask, &omask);
	if (err != 0) {
		omask = -1;
		errno = err;
	}
	return omask;
}

