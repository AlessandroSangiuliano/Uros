/* 
 * ../libjacket/e_uname.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_uname(struct utsname *name)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	uname(name);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
