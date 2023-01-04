/* 
 * ../libjacket/e_mkfifo.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_mkfifo(const char *path, mode_t mode)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	mkfifo(path, mode);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
