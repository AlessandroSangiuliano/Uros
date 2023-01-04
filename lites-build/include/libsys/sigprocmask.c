/* 
 * ../libsys/sigprocmask.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
	errno_t err;

	err = e_sigprocmask(how, set, oset);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
