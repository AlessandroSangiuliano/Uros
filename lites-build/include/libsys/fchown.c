/* 
 * ../libsys/fchown.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int fchown(int fd, uid_t owner, gid_t group)
{
	errno_t err;

	err = e_fchown(fd, owner, group);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

