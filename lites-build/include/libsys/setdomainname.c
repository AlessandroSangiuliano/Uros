/* 
 * ../libsys/setdomainname.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int setdomainname(const char *name, int namelen)
{
	errno_t err;

	err = e_setdomainname(name, namelen);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

