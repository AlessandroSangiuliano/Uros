/* 
 * ../libsys/write.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

size_t write(int fd, const void *buf, size_t nbytes)
{
	size_t nwritten = 0;
	errno_t err;

	err = e_write(fd, buf, nbytes, &nwritten);
	if (err != 0) {
		nwritten = -1;
		errno = err;
	}
	return nwritten;
}
