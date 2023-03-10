/* 
 * ../libsys/getfsstat.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int getfsstat(struct statfs *buf, long bufsize, int flags)
{
	int nstructs = 0;
	errno_t err;

	err = e_getfsstat(buf, bufsize, flags, &nstructs);
	if (err != 0) {
		nstructs = -1;
		errno = err;
	}
	return nstructs;
}

