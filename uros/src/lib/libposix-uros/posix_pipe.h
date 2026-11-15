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
 *
 * port, token[] and ends[] are pipe_server's side (pipe.defs): a send right
 * to the pipe's port, this task's token for each end, and the descriptions
 * of this task that name each end.  When an end's last description goes the
 * task gives back its token, and pipe_server counts one holder fewer; when
 * the last holder of an end has gone, the other side sees end of file or
 * EPIPE.  All three are MACH_PORT_NULL where there is no pipe_server: such a
 * pipe never says that an end has gone.  posix_fd.c changes ends[] under
 * pfd_lock, with __uros_pipe_end_get() and __uros_pipe_end_put().
 */
struct uros_pipe {
	int				in_use;
	int				refs;
	struct flipc2_channel		*ch;
	struct uros_pipe_shared		*sh;
	unsigned int			port;		/* mach_port_t */
	unsigned int			token[2];	/* by PIPE_END_* */
	int				ends[2];
};

/* The layout of struct iovec, which writev() and readv() hand down. */
struct uros_pipe_iov {
	unsigned long	base;
	unsigned long	len;
};

/* A new pipe, with refs 0, registered with pipe_server and holding a token
 * for each end where there is one.  0, or a negative errno. */
int  __uros_pipe_new(struct uros_pipe **out);

/* This task's mapping of the pipe and its handle go, with its rights to the
 * pipe; other holders keep theirs.  Called once refs has reached 0, without
 * pfd_lock. */
void __uros_pipe_free(struct uros_pipe *p);

/* A description of end `end' (PIPE_END_READ or PIPE_END_WRITE) made, and one
 * freed, under pfd_lock.  _put answers the token to give back with
 * __uros_pipe_token_put() once pfd_lock is dropped, or 0. */
void         __uros_pipe_end_get(struct uros_pipe *p, int end);
unsigned int __uros_pipe_end_put(struct uros_pipe *p, int end);
void         __uros_pipe_token_put(unsigned int token);

/* execve(), the new image: end `end' of the pipe whose port and token came
 * under these names, mapped once a pipe.  NULL when it cannot be. */
struct uros_pipe *__uros_pipe_adopt(unsigned int port, unsigned int token,
				    int end);

/* fork(), posix_fork.c: the child (a task port) given this task's pipes
 * under the names it knows, and a token of its own for each end, asked for
 * child_pid.  Before the child's first instruction. */
void __uros_pipe_fork(unsigned int child, unsigned int child_pid);

/* read() and readv(), write() and writev(), on the pipe's two ends: bytes
 * moved, or a negative errno. */
long __uros_pipe_read(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		      int iovcnt, int nonblock);
long __uros_pipe_write(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		       int iovcnt, int nonblock);

#endif /* UROS_POSIX_PIPE_H */
