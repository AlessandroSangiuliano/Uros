/* 
 * ../libjacket/e_creat.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_creat(char *name, mode_t mode, int *fd)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*fd = creat(name, mode);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
