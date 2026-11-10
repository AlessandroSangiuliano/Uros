/*
 * libposix-uros — pipes (#904): what posix_fd.c needs of posix_pipe.c.
 * Internal, not installed.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#ifndef UROS_POSIX_PIPE_H
#define UROS_POSIX_PIPE_H

struct flipc2_channel;		/* flipc2.h, which only posix_pipe.c includes */
struct uros_pipe_shared;

/*
 * This task's hold on one pipe: the channel's handle, which is this task's
 * own, and the pipe's shared page in the channel.
 *
 * refs counts the descriptions of this task that name the pipe, its read
 * end's and its write end's, and the reads and writes going on in it.
 * posix_fd.c changes it under pfd_lock; at 0 the task lets the pipe go with
 * __uros_pipe_free(), with that lock dropped.  A forked child has a copy of
 * this, with the same count, as it has a copy of the descriptions.
 */
struct uros_pipe {
	int				in_use;
	int				refs;
	struct flipc2_channel		*ch;
	struct uros_pipe_shared		*sh;
};

/* The layout of struct iovec, which writev() and readv() hand down. */
struct uros_pipe_iov {
	unsigned long	base;
	unsigned long	len;
};

/* A new pipe, with refs 0.  0, or a negative errno. */
int  __uros_pipe_new(struct uros_pipe **out);

/* This task's mapping of the pipe and its handle go; other holders keep
 * theirs.  Called once refs has reached 0, without pfd_lock. */
void __uros_pipe_free(struct uros_pipe *p);

/* read() and readv(), write() and writev(), on the pipe's two ends: bytes
 * moved, or a negative errno. */
long __uros_pipe_read(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		      int iovcnt, int nonblock);
long __uros_pipe_write(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		       int iovcnt, int nonblock);

#endif /* UROS_POSIX_PIPE_H */
