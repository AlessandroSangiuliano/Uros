/* 
 * ../libsys/symlink.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int symlink(const char *oname, const char *nname)
{
	errno_t err;

	err = e_symlink(oname, nname);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

