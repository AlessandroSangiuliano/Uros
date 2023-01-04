/* 
 * ../libsys/geteuid.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

uid_t geteuid()
{
	uid_t euid = 0;
	errno_t err;

	err = e_geteuid(&euid);
	if (err != 0) {
		euid = -1;
		errno = err;
	}
	return euid;
}

