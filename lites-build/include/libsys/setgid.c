/* 
 * ../libsys/setgid.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int setgid(gid_t gid)
{
	errno_t err;

	err = e_setgid(gid);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

