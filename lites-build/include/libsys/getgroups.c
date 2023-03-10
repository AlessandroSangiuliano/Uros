/* 
 * ../libsys/getgroups.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int getgroups(u_int gidsetsize, int *gidset)
{
	int ngroups = 0;
	errno_t err;

	err = e_getgroups(gidsetsize, gidset, &ngroups);
	if (err != 0) {
		ngroups = -1;
		errno = err;
	}
	return ngroups;
}

