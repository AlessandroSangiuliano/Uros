/* 
 * ../libjacket/e_smmap.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_smmap(caddr_t addr, size_t len, int prot, int flags, int fd, off_t offset, caddr_t *addr_out)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	*addr_out = mmap(addr, len, prot, flags, fd, offset);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

