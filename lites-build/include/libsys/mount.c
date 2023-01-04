/* 
 * ../libsys/mount.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int mount(int type, const char *dir, int flags, void * data)
{
	errno_t err;

	err = e_mount(type, dir, flags, data);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
