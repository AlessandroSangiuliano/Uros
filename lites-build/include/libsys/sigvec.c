/* 
 * ../libsys/sigvec.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int sigvec(int sig, struct sigvec *vec, struct sigvec *ovec)
{
	errno_t err;

	err = e_sigvec(sig, vec, ovec);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}

