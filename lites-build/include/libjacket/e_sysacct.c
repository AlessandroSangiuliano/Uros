/* 
 * ../libjacket/e_sysacct.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libjacket.h>

errno_t e_sysacct(const char *file)
{
	errno_t err, tmp;

	errno_jacket_lock();
	tmp = errno;
	errno = 0;
	acct(file);
	err = errno;
	errno = tmp;
	errno_jacket_unlock();

	return err;
}
