/* 
 * ../libjacket/e_getfsstat.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_getfsstat(struct statfs *buf, long bufsize, int flags, int *nstructs)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*nstructs = getfsstat(buf, bufsize, flags);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
