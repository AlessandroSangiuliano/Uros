/* 
 * ../libsys/sigaction.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
	errno_t err;

	err = e_sigaction(sig, act, oact);
	if (err != 0) {
		errno = err;
		return -1;
	}
	return 0;
}
