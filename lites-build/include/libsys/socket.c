/* 
 * ../libsys/socket.c
 * DO NOT EDIT-- this file is automatically generated.
 * created from /home/slex/Scaricati/xMach-main/lites/server/kern/syscalls.master
 */

#include <sys/libsys.h>

int socket(int domain, int type, int protocol)
{
	int fd = 0;
	errno_t err;

	err = e_socket(domain, type, protocol, &fd);
	if (err != 0) {
		fd = -1;
		errno = err;
	}
	return fd;
}
