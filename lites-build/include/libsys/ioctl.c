/* 
 * ../libsys/ioctl.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int ioctl(int fd, unsigned int cmd, char *argp)
{
	errno_t err;

	err = e_ioctl(fd, cmd, argp);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
