/* 
 * ../libsys/brk.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int brk(const char *addr)
{
	errno_t err;

	err = e_obreak(addr);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
