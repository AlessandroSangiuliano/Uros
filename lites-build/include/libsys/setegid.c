/* 
 * ../libsys/setegid.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int setegid(gid_t egid)
{
	errno_t err;

	err = e_setegid(egid);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

