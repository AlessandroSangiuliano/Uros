/* 
 * ../libsys/mknod.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int mknod(const char *path, mode_t mode, dev_t dev)
{
	errno_t err;

	err = e_mknod(path, mode, dev);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

