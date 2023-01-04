/* 
 * ../libjacket/e_ioctl.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_ioctl(int fd, unsigned int cmd, char *argp)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	ioctl(fd, cmd, argp);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
