/* 
 * ../libsys/link.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int link(const char *target, const char *linkname)
{
	errno_t err;

	err = e_link(target, linkname);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
