/* 
 * ../libsys/mkdir.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int mkdir(const char *path, mode_t mode)
{
	errno_t err;

	err = e_mkdir(path, mode);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

