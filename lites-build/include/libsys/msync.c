/* 
 * ../libsys/msync.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int msync(caddr_t addr, int len)
{
	errno_t err;

	err = e_msync(addr, len);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

