/* 
 * ../libsys/getrlimit.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int getrlimit(int resource, struct rlimit *rlp)
{
	errno_t err;

	err = e_getrlimit(resource, rlp);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
