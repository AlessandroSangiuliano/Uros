/* 
 * ../libsys/pipe.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int pipe(int fds[2])
{
	errno_t err;

	err = e_pipe(fds);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
