/* 
 * ../libjacket/e_sigvec.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_sigvec(int sig, struct sigvec *vec, struct sigvec *ovec)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	sigvec(sig, vec, ovec);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}

