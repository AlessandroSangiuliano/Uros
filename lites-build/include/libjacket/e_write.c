/* 
 * ../libjacket/e_write.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_write(int fd, const void *buf, size_t nbytes, size_t *nwritten)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*nwritten = write(fd, buf, nbytes);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

