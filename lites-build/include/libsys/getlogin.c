/* 
 * ../libsys/getlogin.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

char* getlogin()
{
	char* name = 0;
	errno_t err;

	err = e_getlogin(&name);
	if (err != 0) {
		name = -1;
		errno = err;
	}
	return name;
}

