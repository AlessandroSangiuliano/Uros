/* 
 * ../libjacket/e_munmap.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_munmap(caddr_t addr, int len)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	munmap(addr, len);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

