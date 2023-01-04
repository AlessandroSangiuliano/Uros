/* 
 * Mach Operating System
 * Copyright (c) 1992 Carnegie Mellon University
 * Copyright (c) 1994 Johannes Helander
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON AND JOHANNES HELANDER ALLOW FREE USE OF THIS
 * SOFTWARE IN ITS "AS IS" CONDITION.  CARNEGIE MELLON AND JOHANNES
 * HELANDER DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES
 * WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 */
/*
 * HISTORY
 * $Log: kern_sig.c,v $
 * Revision 1.2  2000/10/27 01:58:45  welchd
 *
 * Updated to latest source
 *
 * Revision 1.3  1996/03/14  21:08:27  sclawson
 * Ian Dall's signal fixes.
 *
 * Revision 1.2  1995/09/08  16:44:49  mike
 * elaborate on race condition
 *
 * Revision 1.1.1.1  1995/03/02  21:49:45  mike
 * Initial Lites release from hut.fi
 *
 * 12-Oct-95  Ian Dall (dall@hfrd.dsto.gov.au)
 *	Use ffs() instead of find_first_set_bit() since the former can be
 *	inlined.
 *
 * 12-Oct-95  Ian Dall (dall@hfrd.dsto.gov.au)
 *	Ignore value returned by send_signal().
 *
 * 30-Sep-95  Ian Dall (dall@hfrd.dsto.gov.au)
 *	Re-enable direct signals. Race appears to be fixed.
 *
 */
/* 
 *	File:	 kern_sig.c
 *	Authors:
 *	Randall Dean, Carnegie Mellon University, 1992.
 *	Johannes Helander, Helsinki University of Technology, 1994.
 *	Origin:	 Adapted to Lites from 4.4 BSD Lite.
 *
 * 	Signal delivery.
 */
/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_sig.c	8.7 (Berkeley) 4/18/94
 */

#include "compat_43.h"
#include "diagnostic.h"
#include "data_synch.h"

#include <serv/server_defs.h>
#include <serv/synch_prim.h>

#define	SIGPROP		/* include signal properties table */
#include <sys/param.h>
#include <sys/signalvar.h>
#include <sys/resourcevar.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/timeb.h>
#include <sys/times.h>
#include <sys/buf.h>
#include <sys/acct.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/wait.h>
#include <sys/ktrace.h>
#include <sys/syslog.h>
#include <sys/stat.h>
#include <sys/synch.h>

#include <machine/cpu.h>

#include <vm/vm.h>
#include <sys/user.h>		/* for coredump */

extern void sigexit(struct proc *p, int signum);
extern void stop(struct proc *p);
extern void setsigvec(struct proc *p, int signum, struct sigaction *sa);

#if MAP_UAREA
#define siglock(p) share_lock(&(p)->p_shared_rw->us_siglock,(p))
#define sigunlock(p) share_unlock(&(p)->p_shared_rw->us_siglock,(p))
#else
#define siglock(p)
#define sigunlock(p)
#endif

#define SIGNAL_DEBUG 1
#if SIGNAL_DEBUG
extern int signal_debug;
#define SD(statement) if (signal_debug) statement
int signal_send_running = 0;
#else
#define SD(x)
#endif SIGNAL_DEBUG

/*
 * Can process p, with pcred pc, send the signal signum to process q?
 */
#define CANSIGNAL(p, pc, q, signum) \
	((pc)->pc_ucred->cr_uid == 0 || \
	    (pc)->p_ruid == (q)->p_cred->p_ruid || \
	    (pc)->pc_ucred->cr_uid == (q)->p_cred->p_ruid || \
	    (pc)->p_ruid == (q)->p_ucred->cr_uid || \
	    (pc)->pc_ucred->cr_uid == (q)->p_ucred->cr_uid || \
	    ((signum) == SIGCONT && (q)->p_session == (p)->p_session))

struct sigaction_args {
	int	signum;
	struct	sigaction *nsa;
	struct	sigaction *osa;
};
/* ARGSUSED */
mach_error_t sigaction(
	struct proc *p,
	struct sigaction_args *uap,
	int *retval)
{
	struct sigaction vec;
	struct sigaction *sa;
	struct sigacts *ps = p->p_sigacts;
	int signum;
	int bit, error;

	signum = uap->signum;
	if (signum <= 0 || signum >= NSIG ||
	    signum == SIGKILL || signum == SIGSTOP)
		return (EINVAL);
	sa = &vec;
	if (uap->osa) {
		sa->sa_handler = ps->ps_sigact[signum];
		sa->sa_mask = ps->ps_catchmask[signum];
		bit = sigmask(signum);
		sa->sa_flags = 0;
		if ((ps->ps_sigonstack & bit) != 0)
			sa->sa_flags |= SA_ONSTACK;
		if ((ps->ps_sigintr & bit) == 0)
			sa->sa_flags |= SA_RESTART;
		if (p->p_flag & P_NOCLDSTOP)
			sa->sa_flags |= SA_NOCLDSTOP;
		if (error = copyout((caddr_t)sa, (caddr_t)uap->osa,
		    sizeof (vec)))
			return (error);
	}
	if (uap->nsa) {
		if (error = copyin((caddr_t)uap->nsa, (caddr_t)sa,
		    sizeof (vec)))
			return (error);
		setsigvec(p, signum, sa);
	}
	return (0);
}

void setsigvec(
	struct proc *p,
	int signum,
	struct sigaction *sa)
{
	register struct sigacts *ps = p->p_sigacts;
	register int bit;

	bit = sigmask(signum);
	/*
	 * Change setting atomically.
	 */
	(void) splhigh();
	ps->ps_sigact[signum] = sa->sa_handler;
	ps->ps_catchmask[signum] = sa->sa_mask &~ sigcantmask;
	if ((sa->sa_flags & SA_RESTART) == 0)
		ps->ps_sigintr |= bit;
	else
		ps->ps_sigintr &= ~bit;
	if (sa->sa_flags & SA_ONSTACK)
		ps->ps_sigonstack |= bit;
	else
		ps->ps_sigonstack &= ~bit;
#ifdef COMPAT_SUNOS
	if (sa->sa_flags & SA_USERTRAMP)
		ps->ps_usertramp |= bit;
	else
		ps->ps_usertramp &= ~bit;
#endif
	if (signum == SIGCHLD) {
		if (sa->sa_flags & SA_NOCLDSTOP)
			p->p_flag |= P_NOCLDSTOP;
		else
			p->p_flag &= ~P_NOCLDSTOP;
	}
	/*
	 * Set bit in p_sigignore for signals that are set to SIG_IGN,
	 * and for signals set to SIG_DFL where the default is to ignore.
	 * However, don't put SIGCONT in p_sigignore,
	 * as we have to restart the process.
	 */
	if (sa->sa_handler == SIG_IGN ||
	    (sigprop[signum] & SA_IGNORE && sa->sa_handler == SIG_DFL)) {
		p->p_siglist &= ~bit;		/* never to be seen again */
		if (signum != SIGCONT)
			p->p_sigignore |= bit;	/* easier in psignal */
		p->p_sigcatch &= ~bit;
	} else {
		p->p_sigignore &= ~bit;
		if (sa->sa_handler == SIG_DFL)
			p->p_sigcatch &= ~bit;
		else
			p->p_sigcatch |= bit;
	}
	(void) spl0();
}

/*
 * Initialize signal state for process 0;
 * set to ignore signals that are ignored by default.
 */
void
siginit(p)
	struct proc *p;
{
	register int i;

	for (i = 0; i < NSIG; i++)
		if (sigprop[i] & SA_IGNORE && i != SIGCONT)
			p->p_sigignore |= sigmask(i);
}

/*
 * Reset signals for an exec of the specified process.
 */
void
execsigs(p)
	register struct proc *p;
{
	register struct sigacts *ps = p->p_sigacts;
	register int nc, mask;

	/*
	 * Reset caught signals.  Held signals remain held
	 * through p_sigmask (unless they were caught,
	 * and are now ignored by default).
	 */
	while (p->p_sigcatch) {
		nc = ffs((long)p->p_sigcatch);
		mask = sigmask(nc);
		p->p_sigcatch &= ~mask;
		if (sigprop[nc] & SA_IGNORE) {
			if (nc != SIGCONT)
				p->p_sigignore |= mask;
			p->p_siglist &= ~mask;
		}
		ps->ps_sigact[nc] = SIG_DFL;
	}
	/*
	 * Reset stack state to the user stack.
	 * Clear set of signals caught on the signal stack.
	 */
	ps->ps_sigstk.ss_flags = SA_DISABLE;
	ps->ps_sigstk.ss_size = 0;
	ps->ps_sigstk.ss_base = 0;
	ps->ps_flags = 0;
}

/*
 * Manipulate signal mask.
 * Note that we receive new mask, not pointer,
 * and return old mask as return value;
 * the library stub does the rest.
 */
struct sigprocmask_args {
	int	how;
	sigset_t mask;
};
mach_error_t sigprocmask(p, uap, retval)
	register struct proc *p;
	struct sigprocmask_args *uap;
	int *retval;
{
	int error = 0;

	*retval = p->p_sigmask;
	(void) splhigh();
	siglock(p);

	switch (uap->how) {
	case SIG_BLOCK:
		p->p_sigmask |= uap->mask &~ sigcantmask;
		break;

	case SIG_UNBLOCK:
		p->p_sigmask &= ~uap->mask;
		break;

	case SIG_SETMASK:
		p->p_sigmask = uap->mask &~ sigcantmask;
		break;
	
	default:
		error = EINVAL;
		break;
	}
	sigunlock(p);
	(void) spl0();
	return (error);
}

struct sigpending_args {
	int	dummy;
};
/* ARGSUSED */
mach_error_t sigpending(p, uap, retval)
	struct proc *p;
	struct sigpending_args *uap;
	int *retval;
{

	*retval = p->p_siglist;
	return (0);
}

#if COMPAT_43 || defined(COMPAT_SUNOS)
/*
 * Generalized interface signal handler, 4.3-compatible.
 */
struct osigvec_args {
	int	signum;
	struct	sigvec *nsv;
	struct	sigvec *osv;
};
/* ARGSUSED */
mach_error_t osigvec(p, uap, retval)
	struct proc *p;
	register struct osigvec_args *uap;
	int *retval;
{
	struct sigvec vec;
	register struct sigacts *ps = p->p_sigacts;
	register struct sigvec *sv;
	register int signum;
	int bit, error;

	signum = uap->signum;
	if (signum <= 0 || signum >= NSIG ||
	    signum == SIGKILL || signum == SIGSTOP)
		return (EINVAL);
	sv = &vec;
	if (uap->osv) {
		*(sig_t *)&sv->sv_handler = ps->ps_sigact[signum];
		sv->sv_mask = ps->ps_catchmask[signum];
		bit = sigmask(signum);
		sv->sv_flags = 0;
		if ((ps->ps_sigonstack & bit) != 0)
			sv->sv_flags |= SV_ONSTACK;
		if ((ps->ps_sigintr & bit) != 0)
			sv->sv_flags |= SV_INTERRUPT;
#ifndef COMPAT_SUNOS
		if (p->p_flag & P_NOCLDSTOP)
			sv->sv_flags |= SA_NOCLDSTOP;
#endif
		if (error = copyout((caddr_t)sv, (caddr_t)uap->osv,
		    sizeof (vec)))
			return (error);
	}
	if (uap->nsv) {
		if (error = copyin((caddr_t)uap->nsv, (caddr_t)sv,
		    sizeof (vec)))
			return (error);
#ifdef COMPAT_SUNOS
		/*
		 * SunOS uses this bit (4, aka SA_DISABLE) as SV_RESETHAND,
		 * `reset to SIG_DFL on delivery'. We have no such option
		 * now or ever!
		 */
		if (sv->sv_flags & SA_DISABLE)
			return (EINVAL);
		sv->sv_flags |= SA_USERTRAMP;
#endif
		sv->sv_flags ^= SA_RESTART;	/* opposite of SV_INTERRUPT */
		setsigvec(p, signum, (struct sigaction *)sv);
	}
	return (0);
}

struct osigblock_args {
	int	mask;
};
mach_error_t osigblock(p, uap, retval)
	register struct proc *p;
	struct osigblock_args *uap;
	int *retval;
{

	(void) splhigh();
	siglock(p);
	*retval = p->p_sigmask;
	p->p_sigmask |= uap->mask &~ sigcantmask;
	sigunlock(p);
	(void) spl0();
	return (0);
}

struct osigsetmask_args {
	int	mask;
};
mach_error_t osigsetmask(p, uap, retval)
	struct proc *p;
	struct osigsetmask_args *uap;
	int *retval;
{

	(void) splhigh();
	siglock(p);
	*retval = p->p_sigmask;
	p->p_sigmask = uap->mask &~ sigcantmask;
	sigunlock(p);
	(void) spl0();
	return (0);
}
#endif /* COMPAT_43 || COMPAT_SUNOS */

/*
 * Suspend process until signal, providing mask to be set
 * in the meantime.  Note nonstandard calling convention:
 * libc stub passes mask, not pointer, to save a copyin.
 */
struct sigsuspend_args {
	sigset_t mask;
};
/* ARGSUSED */
mach_error_t sigsuspend(p, uap, retval)
	register struct proc *p;
	struct sigsuspend_args *uap;
	int *retval;
{
	register struct sigacts *ps = p->p_sigacts;

	/*
	 * When returning from sigpause, we want
	 * the old mask to be restored after the
	 * signal handler has finished.  Thus, we
	 * save it here and mark the sigacts structure
	 * to indicate this.
	 */
	ps->ps_oldmask = p->p_sigmask;
	ps->ps_flags |= SAS_OLDMASK;
	siglock(p);
	p->p_sigmask = uap->mask &~ sigcantmask;
	sigunlock(p);
	/* 
	 * XXX The while loop has been removed and thus we might wake
	 * XXX up for no apparent reason. Shouldn't be too bad though.
	 */
	tsleep((caddr_t) ps, PPAUSE|PCATCH, "pause", 0);
	/* always return EINTR rather than ERESTART... */
	return (EINTR);
}

#if COMPAT_43 || defined(COMPAT_SUNOS)
struct osigstack_args {
	struct	sigstack *nss;
	struct	sigstack *oss;
};
/* ARGSUSED */
mach_error_t osigstack(p, uap, retval)
	struct proc *p;
	register struct osigstack_args *uap;
	int *retval;
{
	struct sigstack ss;
	struct sigacts *psp;
	int error = 0;

	psp = p->p_sigacts;
	ss.ss_sp = psp->ps_sigstk.ss_base;
	ss.ss_onstack = psp->ps_sigstk.ss_flags & SA_ONSTACK;
	if (uap->oss && (error = copyout((caddr_t)&ss, (caddr_t)uap->oss,
	    sizeof (struct sigstack))))
		return (error);
	if (uap->nss && (error = copyin((caddr_t)uap->nss, (caddr_t)&ss,
	    sizeof (ss))) == 0) {
		psp->ps_sigstk.ss_base = ss.ss_sp;
		psp->ps_sigstk.ss_size = 0;
		psp->ps_sigstk.ss_flags |= ss.ss_onstack & SA_ONSTACK;
		psp->ps_flags |= SAS_ALTSTACK;
	}
	return (error);
}
#endif /* COMPAT_43 || COMPAT_SUNOS */

struct sigaltstack_args {
	struct	sigaltstack *nss;
	struct	sigaltstack *oss;
};
/* ARGSUSED */
mach_error_t sigaltstack(p, uap, retval)
	struct proc *p;
	register struct sigaltstack_args *uap;
	int *retval;
{
	struct sigacts *psp;
	struct sigaltstack ss;
	int error;

	psp = p->p_sigacts;
	if ((psp->ps_flags & SAS_ALTSTACK) == 0)
		psp->ps_sigstk.ss_flags |= SA_DISABLE;
	if (uap->oss && (error = copyout((caddr_t)&psp->ps_sigstk,
	    (caddr_t)uap->oss, sizeof (struct sigaltstack))))
		return (error);
	if (uap->nss == 0)
		return (0);
	if (error = copyin((caddr_t)uap->nss, (caddr_t)&ss, sizeof (ss)))
		return (error);
	if (ss.ss_flags & SA_DISABLE) {
		if (psp->ps_sigstk.ss_flags & SA_ONSTACK)
			return (EINVAL);
		psp->ps_flags &= ~SAS_ALTSTACK;
		psp->ps_sigstk.ss_flags = ss.ss_flags;
		return (0);
	}
	if (ss.ss_size < MINSIGSTKSZ)
		return (ENOMEM);
	psp->ps_flags |= SAS_ALTSTACK;
	psp->ps_sigstk= ss;
	return (0);
}

struct kill_args {
	int	pid;
	int	signum;
};
/* ARGSUSED */
mach_error_t kill(cp, uap, retval)
	register struct proc *cp;
	register struct kill_args *uap;
	int *retval;
{
	register struct proc *p;
	register struct pcred *pc = cp->p_cred;

	if ((u_int)uap->signum >= NSIG)
		return (EINVAL);
	if (uap->pid > 0) {
		/* kill single process */
		if ((p = pfind(uap->pid)) == NULL)
			return (ESRCH);
		if (!CANSIGNAL(cp, pc, p, uap->signum))
			return (EPERM);
		if (uap->signum)
			psignal(p, uap->signum);
		return (0);
	}
	switch (uap->pid) {
	case -1:		/* broadcast signal */
		return (killpg1(cp, uap->signum, 0, 1));
	case 0:			/* signal own process group */
		return (killpg1(cp, uap->signum, 0, 0));
	default:		/* negative explicit process group */
		return (killpg1(cp, uap->signum, -uap->pid, 0));
	}
	/* NOTREACHED */
}

#if COMPAT_43 || defined(COMPAT_SUNOS)
struct okillpg_args {
	int	pgid;
	int	signum;
};
/* ARGSUSED */
mach_error_t okillpg(p, uap, retval)
	struct proc *p;
	register struct okillpg_args *uap;
	int *retval;
{

	if ((u_int)uap->signum >= NSIG)
		return (EINVAL);
	return (killpg1(p, uap->signum, uap->pgid, 0));
}
#endif /* COMPAT_43 || COMPAT_SUNOS */

/*
 * Common code for kill process group/broadcast kill.
 * cp is calling process.
 */
mach_error_t killpg1(cp, signum, pgid, all)
	register struct proc *cp;
	int signum, pgid, all;
{
	register struct proc *p;
	register struct pcred *pc = cp->p_cred;
	struct pgrp *pgrp;
	int nfound = 0;
	
	if (all)	
		/* 
		 * broadcast 
		 */
		for (p = (struct proc *)allproc; p != NULL; p = p->p_next) {
			if (p->p_pid <= 1 || p->p_flag & P_SYSTEM || 
			    p == cp || !CANSIGNAL(cp, pc, p, signum))
				continue;
			nfound++;
			if (signum)
				psignal(p, signum);
		}
	else {
		if (pgid == 0)		
			/* 
			 * zero pgid means send to my process group.
			 */
			pgrp = cp->p_pgrp;
		else {
			pgrp = pgfind(pgid);
			if (pgrp == NULL)
				return (ESRCH);
		}
		for (p = pgrp->pg_mem; p != NULL; p = p->p_pgrpnxt) {
			if (p->p_pid <= 1 || p->p_flag & P_SYSTEM ||
			    p->p_stat == SZOMB ||
			    !CANSIGNAL(cp, pc, p, signum))
				continue;
			nfound++;
			if (signum)
				psignal(p, signum);
		}
	}
	return (nfound ? 0 : ESRCH);
}

/*
 * Send a signal to a process group.
 */
void
gsignal(pgid, signum)
	int pgid, signum;
{
	struct pgrp *pgrp;

	if (pgid && (pgrp = pgfind(pgid)))
		pgsignal(pgrp, signum, 0);
}

/*
 * Send a signal to a  process group.  If checktty is 1,
 * limit to members which have a controlling terminal.
 */
void
pgsignal(pgrp, signum, checkctty)
	struct pgrp *pgrp;
	int signum, checkctty;
{
	register struct proc *p;

	if (pgrp)
		for (p = pgrp->pg_mem; p != NULL; p = p->p_pgrpnxt)
			if (checkctty == 0 || p->p_flag & P_CONTROLT)
				psignal(p, signum);
}

#ifndef LITES /* XXX for now */
/*
 * Send a signal caused by a trap to the current process.
 * If it will be caught immediately, deliver it with correct code.
 * Otherwise, post it normally.
 */
void
trapsignal(p, signum, code)
	struct proc *p;
	register int signum;
	u_int code;
{
	register struct sigacts *ps = p->p_sigacts;
	int mask;

	mask = sigmask(signum);
	if ((p->p_flag & P_TRACED) == 0 && (p->p_sigcatch & mask) != 0 &&
	    (p->p_sigmask & mask) == 0) {
		p->p_stats->p_ru.ru_nsignals++;
#ifdef KTRACE
		if (KTRPOINT(p, KTR_PSIG))
			ktrpsig(p->p_tracep, signum, ps->ps_sigact[signum], 
				p->p_sigmask, code);
#endif
		sendsig(ps->ps_sigact[signum], signum, p->p_sigmask, code);
		p->p_sigmask |= ps->ps_catchmask[signum] | mask;
	} else {
		ps->ps_code = code;	/* XXX for core dump/debugger */
		psignal(p, signum);
	}
}
#endif /* LITES */

/*
 * Send the signal to the process.  If the signal has an action, the action
 * is usually performed by the target process rather than the caller; we add
 * the signal to the set of pending signals for the process.
 *
 * LITES: for stop and kill take action directe.
 *
 * Exceptions:
 *   o When a stop signal is sent to a sleeping process that takes the
 *     default action, the process is stopped without awakening it.
 *   o SIGCONT restarts stopped processes (or puts them back to sleep)
 *     regardless of the signal action (eg, blocked or ignored).
 *
 * Other ignored signals are discarded immediately.
 */

/* 
 * LITES (maybe) TODO:
 * Reduce relevant process states to two: suspended and not suspended (SRUN).
 * Use p_servers_count to determine if system calls are active.
 * Use locks on p.
 * Simplify and clean up.
 *
 * Direct default action seems to almost work even now.
 */
#define SIGX1 1
#define SIGX2 0
#define SIGX3 0
#define SIGX4 1

void
psignal(p, signum)
	register struct proc *p;
	register int signum;
{
	proc_invocation_t pk = get_proc_invocation();
	register int s, prop;
	register sig_t action;
	int mask;
	kern_return_t kr;

	if ((u_int)signum >= NSIG || signum == 0)
		panic("psignal signal number");
	mask = sigmask(signum);
	prop = sigprop[signum];

	/* Called from both timer and almost any other place */
	s = splhigh();

	/*
	 * If proc is traced, always give parent a chance.
	 */
	if (p->p_flag & P_TRACED)
		action = SIG_DFL;
	else {
		/*
		 * If the signal is being ignored,
		 * then we forget about it immediately.
		 * (Note: we don't set SIGCONT in p_sigignore,
		 * and if it is set to SIG_IGN,
		 * action will be SIG_DFL here.)
		 */
		if (p->p_sigignore & mask) {
			splx(s);
			return;
		}
		if (p->p_sigmask & mask)
			action = SIG_HOLD;
		else if (p->p_sigcatch & mask)
			action = SIG_CATCH;
		else
			action = SIG_DFL;
	}

	if (p->p_nice > NZERO && action == SIG_DFL && (prop & SA_KILL) &&
	    (p->p_flag & P_TRACED) == 0)
		p->p_nice = NZERO;

	if (prop & SA_CONT)
		p->p_siglist &= ~stopsigmask;

	if (prop & SA_STOP) {
		/*
		 * If sending a tty stop signal to a member of an orphaned
		 * process group, discard the signal here if the action
		 * is default; don't stop the process below if sleeping,
		 * and don't clear any pending SIGCONT.
		 */
		if (prop & SA_TTYSTOP && p->p_pgrp->pg_jobc == 0 &&
		    action == SIG_DFL) {
			splx(s);
		        return;
		}
		p->p_siglist &= ~contsigmask;
	}
	p->p_siglist |= mask;

	/*
	 * Defer further processing for signals which are held,
	 * except that stopped processes must be continued by SIGCONT.
	 */
	if (action == SIG_HOLD
	    && ((prop & SA_CONT) == 0 || p->p_stat != SSTOP)) {
		splx(s);
		return;
	}

	switch (p->p_stat) {

	case SSLEEP:
		/*
		 * If process is sleeping uninterruptibly
		 * we can't interrupt the sleep... the signal will
		 * be noticed when the process returns through
		 * trap() or syscall().
		 */
		if ((p->p_flag & P_SINTR) == 0)
			goto out;
		/*
		 * Process is sleeping and traced... make it runnable
		 * so it can discover the signal in issignal() and stop
		 * for the parent.
		 */
		if (p->p_flag & P_TRACED)
			goto run;
		/*
		 * If SIGCONT is default (or ignored) and process is
		 * asleep, we are finished; the process should not
		 * be awakened.
		 */
		if ((prop & SA_CONT) && action == SIG_DFL) {
			p->p_siglist &= ~mask;
			goto out;
		}
		/*
		 * When a sleeping process receives a stop
		 * signal, process immediately if possible.
		 * All other (caught or default) signals
		 * cause the process to run.
		 */
		if (prop & SA_STOP) {
			if (action != SIG_DFL)
				goto runfast;
			/*
			 * If a child holding parent blocked,
			 * stopping could cause deadlock.
			 */
			if (p->p_flag & P_PPWAIT)
				goto out;
			p->p_siglist &= ~mask;
			p->p_xstat = signum;
			if ((p->p_pptr->p_flag & P_NOCLDSTOP) == 0)
				psignal(p->p_pptr, SIGCHLD);
			stop(p);
			goto out;
		} else
			goto runfast;
		/*NOTREACHED*/

	case SSTOP:
		/*
		 * If traced process is already stopped,
		 * then no further action is necessary.
		 */
		if (p->p_flag & P_TRACED)
			goto out;

		/*
		 * Kill signal always sets processes running.
		 */
		if (signum == SIGKILL)
			goto killit;
		if (prop & SA_CONT) {
			/*
			 * If SIGCONT is default (or ignored), we continue the
			 * process but don't leave the signal in p_siglist, as
			 * it has no further action.  If SIGCONT is held, we
			 * continue the process and leave the signal in
			 * p_siglist.  If the process catches SIGCONT, let it
			 * handle the signal itself.  If it isn't waiting on
			 * an event, then it goes back to run state.
			 * Otherwise, process goes back to sleep state.
			 */
			if (action == SIG_DFL)
				p->p_siglist &= ~mask;
			if (action == SIG_CATCH)
				goto runfast;
			if (pk->k_wchan == 0)
#if SIGX1
				goto runfast;
#else
				goto run;
#endif
			p->p_stat = SSLEEP;
			kr = task_resume(p->p_task);
			goto out;
		}

		if (prop & SA_STOP) {
			/*
			 * Already stopped, don't need to stop again.
			 * (If we did the shell could get confused.)
			 */
			p->p_siglist &= ~mask;		/* take it away */
			goto out;
		}

		/*
		 * If process is sleeping interruptibly, then simulate a
		 * wakeup so that when it is continued, it will be made
		 * runnable and can look at the signal.  But don't make
		 * the process runnable, leave it stopped.
		 */
#if DATA_SYNCH
		proc_wakeup_locked(p, PKW_SIGNAL);
#else
		if (pk->k_wchan && p->p_flag) /* & P_SINTR) */
			unsleep_process(p);
#endif
		goto out;

#if SIGX1
	case SRUN:
		/* LITES: action directe if no server threads active */
		if (action == SIG_DFL) {
			if (prop & SA_KILL)
			    goto killit;
			if (prop & SA_STOP) {
				kr = task_suspend(p->p_task);
				assert(kr == KERN_SUCCESS);
				p->p_stat = SSTOP;
				/* 
				 * Without resetting P_WAITED wait4
				 * will work only once. What P_WAITED
				 * is supposed to do I don't know.
				 * The resetting might belong at the
				 * SIGCHLD below?
				 */
				p->p_flag &= ~P_WAITED;

				/*
				 * If a child holding parent blocked,
				 * stopping could cause deadlock.
				 */
				if (p->p_flag & P_PPWAIT)
				    goto out;
				p->p_siglist &= ~mask;
				p->p_xstat = signum;
				if ((p->p_pptr->p_flag & P_NOCLDSTOP) == 0) {
					psignal(p->p_pptr, SIGCHLD);
					wakeup(p->p_pptr);
				}
				goto out;
			}
#if SIGX3
			if (prop & SA_CONT) {
				kr = task_resume(p->p_task);
				/* ignore result */
				goto out;
			}
#endif
		}
#if SIGX4
/* XXX there is apparently a race here.  With this defined we (on the PA) get
   regular segmentation faults from various programs.  What must be happening
   is that we mark the signal, send a message to the signal thread, the
   real thread is scheduled first and sees the pending signal and Something
   Bad happens.  The downside of commenting this section out is that you
   cannot interrupt (only SIGKILL) a process that isn't making system calls,
   e.g., is stuck in an infinite loop.  Need to fix this.
 */
#if SIGNAL_DEBUG
		if (signal_send_running)
#endif
		if (action == SIG_CATCH) {
			if (p->p_servers_count)
#if DATA_SYNCH
			    proc_wakeup_locked(p, PKW_SIGNAL);
#else
			    unsleep_process(p);
#endif
			else
			    (void) send_signal(p, p->p_thread, signum, 0);
			goto out;
		}
#endif /* 0: avoid race */

#endif	/* SIGX1 */
	default:
		/*
		 * SRUN, SIDL, SZOMB do nothing with the signal,
		 * other than kicking ourselves if we are running.
		 * It will either never be noticed, or noticed very soon.
		 */
#if SIGX2
		if (p == get_proc())
			signal_notify(p->p_sigport, p->p_thread);
#endif
		goto out;
	}
	/*NOTREACHED*/

killit:
#if SIGX1
	proc_zap(p, W_EXITCODE(0, signum));
#endif
runfast:
	/*
	 * Raise priority to at least PUSER.
	 */
	p->p_stat = SRUN;
	wakeup((caddr_t)&p->p_stat);
	task_resume(p->p_task);
run:
	/* XXX need only do the following if p_servers_count != 0? */
#if DATA_SYNCH
	proc_wakeup_locked(p, PKW_SIGNAL);
#else
	unsleep_process(p);	/* or thread? XXX */
#endif
	if (p->p_servers_count == 0) {
		struct proc *curproc = get_proc();
		/* 
		 * There is no invocation to handle the signal.
		 * Take Action Directe.
		 * (this code fragment is from thread_signal).
		 */
		SD(printf("%8x[%d]: psignalDIRECT %x[%d] %x\n",curproc,curproc->p_pid, p,p->p_pid, signum));
		switch ((integer_t)p->p_sigacts->ps_sigact[signum]) {
		      case SIG_IGN:
		      case SIG_HOLD:
			break;

		      case SIG_DFL:
			sig_default(p, signum);
			break;

		      default:
			/*
			 * Send signal to user thread.
			 */
			(void) send_signal(p, p->p_thread, signum, 0);
			break;
		}
	}
out:
	splx(s);
}

/*
 * If the current process has received a signal (should be caught or cause
 * termination, should interrupt current syscall), return the signal number.
 * Stop signals with default action are processed immediately, then cleared;
 * they aren't returned.  This is checked after each entry to the system for
 * a syscall or trap (though this can usually be done without calling issignal
 * by checking the pending signal masks in the CURSIG macro.) The normal call
 * sequence is
 *
 *	while (signum = CURSIG(curproc))
 *		postsig(signum);
 */
int issignal(p)
	register struct proc *p;
{
	register int signum, mask, prop;

	for (;;) {
		mask = p->p_siglist & ~p->p_sigmask;
		if (p->p_flag & P_PPWAIT)
			mask &= ~stopsigmask;
		if (mask == 0)	 	/* no signal to send */
			return (0);
		signum = ffs((long)mask);
		mask = sigmask(signum);
		prop = sigprop[signum];
		/*
		 * We should see pending but ignored signals
		 * only if P_TRACED was on when they were posted.
		 */
		if (mask & p->p_sigignore && (p->p_flag & P_TRACED) == 0) {
			p->p_siglist &= ~mask;
			continue;
		}
		if (p->p_flag & P_TRACED && (p->p_flag & P_PPWAIT) == 0) {
			/*
			 * If traced, always stop, and stay
			 * stopped until released by the parent.
			 */
			p->p_xstat = signum;
			psignal(p->p_pptr, SIGCHLD);
			do {
				stop(p);
#ifndef LITES
				mi_switch();
#endif
			} while (!trace_req(p) && p->p_flag & P_TRACED);

			/*
			 * If the traced bit got turned off, go back up
			 * to the top to rescan signals.  This ensures
			 * that p_sig* and ps_sigact are consistent.
			 */
			if ((p->p_flag & P_TRACED) == 0)
				continue;

			/*
			 * If parent wants us to take the signal,
			 * then it will leave it in p->p_xstat;
			 * otherwise we just look for signals again.
			 */
			p->p_siglist &= ~mask;	/* clear the old signal */
			signum = p->p_xstat;
			if (signum == 0)
				continue;

			/*
			 * Put the new signal into p_siglist.  If the
			 * signal is being masked, look for other signals.
			 */
			mask = sigmask(signum);
			p->p_siglist |= mask;
			if (p->p_sigmask & mask)
				continue;
		}

		/*
		 * Decide whether the signal should be returned.
		 * Return the signal's number, or fall through
		 * to clear it from the pending mask.
		 */
		switch ((integer_t)p->p_sigacts->ps_sigact[signum]) {

		case SIG_DFL:
			/*
			 * Don't take default actions on system processes.
			 */
			if (p->p_pid <= 1) {
#if DIAGNOSTIC
				/*
				 * Are you sure you want to ignore SIGSEGV
				 * in init? XXX
				 */
				printf("Process (pid %d) got signal %d\n",
					p->p_pid, signum);
#endif
				break;		/* == ignore */
			}
			/*
			 * If there is a pending stop signal to process
			 * with default action, stop here,
			 * then clear the signal.  However,
			 * if process is member of an orphaned
			 * process group, ignore tty stop signals.
			 */
			if (prop & SA_STOP) {
				if (p->p_flag & P_TRACED ||
		    		    (p->p_pgrp->pg_jobc == 0 &&
				    prop & SA_TTYSTOP))
					break;	/* == ignore */
				p->p_xstat = signum;
				stop(p);
				if ((p->p_pptr->p_flag & P_NOCLDSTOP) == 0)
					psignal(p->p_pptr, SIGCHLD);
#ifndef LITES
				mi_switch();
#endif
				break;
			} else if (prop & SA_IGNORE) {
				/*
				 * Except for SIGCONT, shouldn't get here.
				 * Default action is to ignore; drop it.
				 */
				break;		/* == ignore */
			} else
				return (signum);
			/*NOTREACHED*/

		case SIG_IGN:
			/*
			 * Masking above should prevent us ever trying
			 * to take action on an ignored signal other
			 * than SIGCONT, unless process is traced.
			 */
			if ((prop & SA_CONT) == 0 &&
			    (p->p_flag & P_TRACED) == 0)
				printf("issignal\n");
			break;		/* == ignore */

		default:
			/*
			 * This signal has an action, let
			 * postsig() process it.
			 */
			return (signum);
		}
		p->p_siglist &= ~mask;		/* take the signal! */
	}
	/* NOTREACHED */
}

/*
 * Put the argument process into the stopped state and notify the parent
 * via wakeup.  Signals are handled elsewhere.  The process must not be
 * on the run queue.
 */
void stop(struct proc *p)
{
#if defined(LITES) && 0
	kern_return_t kr;
	kr = task_suspend(p->p_task);
	assert(kr == KERN_SUCCESS);
#endif /* LITES */
	p->p_stat = SSTOP;
	p->p_flag &= ~P_WAITED;
	wakeup((caddr_t)p->p_pptr);
}

/*
 * Take the action for the specified signal
 * from the current set of pending signals.
 */
void
postsig(signum)
	register int signum;
{
	register struct proc *p = get_proc();
	register struct sigacts *ps = p->p_sigacts;
	register sig_t action;
	int code, mask, returnmask;

#if DIAGNOSTIC
	if (signum == 0)
		panic("postsig");
#endif
	mask = sigmask(signum);
	p->p_siglist &= ~mask;
	action = ps->ps_sigact[signum];
#ifdef KTRACE
	if (KTRPOINT(p, KTR_PSIG))
		ktrpsig(p->p_tracep,
		    signum, action, ps->ps_flags & SAS_OLDMASK ?
		    ps->ps_oldmask : p->p_sigmask, 0);
#endif
	if (action == SIG_DFL) {
		/*
		 * Default action, where the default is to kill
		 * the process.  (Other cases were ignored above.)
		 */
		sigexit(p, signum);
		/* NOTREACHED */
	} else {
		/*
		 * If we get here, the signal must be caught.
		 */
#if DIAGNOSTIC
		if (action == SIG_IGN || (p->p_sigmask & mask))
			panic("postsig action");
#endif
		/*
		 * Set the new mask value and also defer further
		 * occurences of this signal.
		 *
		 * Special case: user has done a sigpause.  Here the
		 * current mask is not of interest, but rather the
		 * mask from before the sigpause is what we want
		 * restored after the signal processing is completed.
		 */
		(void) splhigh();
		if (ps->ps_flags & SAS_OLDMASK) {
			returnmask = ps->ps_oldmask;
			ps->ps_flags &= ~SAS_OLDMASK;
		} else
			returnmask = p->p_sigmask;
		p->p_sigmask |= ps->ps_catchmask[signum] | mask;
		(void) spl0();
		p->p_stats->p_ru.ru_nsignals++;
		if (ps->ps_sig != signum) {
			code = 0;
		} else {
			code = ps->ps_code;
			ps->ps_code = 0;
		}
#ifdef LITES
		panic("postsig: sendsig(action, signum, returnmask, code);");
#else /* LITES */
		sendsig(action, signum, returnmask, code);
#endif /* LITES */
	}
}

/*
 * Kill the current process for stated reason.
 */
void killproc(p, why)
	struct proc *p;
	char *why;
{

	log(LOG_ERR, "pid %d was killed: %s\n", p->p_pid, why);
	uprintf("sorry, pid %d was killed: %s\n", p->p_pid, why);
	psignal(p, SIGKILL);
}

/*
 * Force the current process to exit with the specified signal, dumping core
 * if appropriate.  We bypass the normal tests for masked and caught signals,
 * allowing unrecoverable failures to terminate the process without changing
 * signal state.  Mark the accounting record with the signal termination.
 * If dumping core, save the signal number for the debugger.  Calls exit and
 * does not return.
 */
void sigexit(
	struct proc *p,
	int signum)
{

	p->p_acflag |= AXSIG;
	if (sigprop[signum] & SA_CORE) {
		p->p_sigacts->ps_sig = signum;
		if (coredump(p) == 0)
			signum |= WCOREFLAG;
	}
	proc_zap(p, W_EXITCODE(0, signum));
}

/*
 * Dump core, into a file named "progname.core", unless the process was
 * setuid/setgid.
 */
mach_error_t coredump(struct proc *p)
{
	register struct vnode *vp;
	register struct pcred *pcred = p->p_cred;
	register struct ucred *cred = pcred->pc_ucred;
	register struct vmspace *vm = p->p_vmspace;
	struct nameidata nd;
	struct vattr vattr;
	int error, error1;
	char name[MAXCOMLEN+6];		/* progname.core */

#ifdef LITES
	return 0;		/* XXX */
#endif
	if (pcred->p_svuid != pcred->p_ruid || pcred->p_svgid != pcred->p_rgid)
		return (EFAULT);
	if (ctob(UPAGES + vm->vm_dsize + vm->vm_ssize) >=
	    p->p_rlimit[RLIMIT_CORE].rlim_cur)
		return (EFAULT);
	sprintf(name, "%s.core", p->p_comm);
	NDINIT(&nd, LOOKUP, FOLLOW, UIO_SYSSPACE, name, p);
	if (error = vn_open(&nd,
	    O_CREAT | FWRITE, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))
		return (error);
	vp = nd.ni_vp;

	/* Don't dump to non-regular files or files with links. */
	if (vp->v_type != VREG ||
	    VOP_GETATTR(vp, &vattr, cred, p) || vattr.va_nlink != 1) {
		error = EFAULT;
		goto out;
	}
	VATTR_NULL(&vattr);
	vattr.va_size = 0;
	LEASE_CHECK(vp, p, cred, LEASE_WRITE);
	VOP_SETATTR(vp, &vattr, cred, p);
	p->p_acflag |= ACORE;
	bcopy(p, &p->p_addr->u_kproc.kp_proc, sizeof(struct proc));
	fill_eproc(p, &p->p_addr->u_kproc.kp_eproc);
	error = cpu_coredump(p, vp, cred);
	if (error == 0)
		error = vn_rdwr(UIO_WRITE, vp, vm->vm_daddr,
		    (int)ctob(vm->vm_dsize), (off_t)ctob(UPAGES), UIO_USERSPACE,
		    IO_NODELOCKED|IO_UNIT, cred, (int *) NULL, p);
	if (error == 0)
		error = vn_rdwr(UIO_WRITE, vp,
		    (caddr_t) trunc_page(USRSTACK - ctob(vm->vm_ssize)),
		    round_page(ctob(vm->vm_ssize)),
		    (off_t)ctob(UPAGES) + ctob(vm->vm_dsize), UIO_USERSPACE,
		    IO_NODELOCKED|IO_UNIT, cred, (int *) NULL, p);
out:
	VOP_UNLOCK(vp);
	error1 = vn_close(vp, FWRITE, cred, p);
	if (error == 0)
		error = error1;
	return (error);
}

/*
 * Nonexistent system call-- signal process (may want to handle it).
 * Flag error in case process won't see signal immediately (blocked or ignored).
 */
struct nosys_args {
	int	dummy;
};
/* ARGSUSED */
mach_error_t nosys(p, args, retval)
	struct proc *p;
	struct nosys_args *args;
	int *retval;
{

	psignal(p, SIGSYS);
	return (EINVAL);
}
