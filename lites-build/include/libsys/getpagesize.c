/* 
 * ../libsys/getpagesize.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int getpagesize()
{
	int bytes = 0;
	errno_t err;

	err = e_getpagesize(&bytes);
	if (err != 0) {
		bytes = -1;
		errno = err;
	}
	return bytes;
}

