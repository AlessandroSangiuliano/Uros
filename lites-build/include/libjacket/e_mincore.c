/* 
 * ../libjacket/e_mincore.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_mincore(caddr_t addr, int len, char *vec)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	mincore(addr, len, vec);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
