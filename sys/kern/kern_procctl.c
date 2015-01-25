/*-
 * Copyright (c) 2014 John Baldwin
 * Copyright (c) 2014 The FreeBSD Foundation
 *
 * Portions of this software were developed by Konstantin Belousov
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/capability.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/procctl.h>
#include <sys/sx.h>
#include <sys/syscallsubr.h>
#include <sys/sysproto.h>
#include <sys/wait.h>

static int
protect_setchild(struct thread *td, struct proc *p, int flags)
{

	PROC_LOCK_ASSERT(p, MA_OWNED);
	if (p->p_flag & P_SYSTEM || p_cansched(td, p) != 0)
		return (0);
	if (flags & PPROT_SET) {
		p->p_flag |= P_PROTECTED;
		if (flags & PPROT_INHERIT)
			p->p_flag2 |= P2_INHERIT_PROTECTED;
	} else {
		p->p_flag &= ~P_PROTECTED;
		p->p_flag2 &= ~P2_INHERIT_PROTECTED;
	}
	return (1);
}

static int
protect_setchildren(struct thread *td, struct proc *top, int flags)
{
	struct proc *p;
	int ret;

	p = top;
	ret = 0;
	sx_assert(&proctree_lock, SX_LOCKED);
	for (;;) {
		ret |= protect_setchild(td, p, flags);
		PROC_UNLOCK(p);
		/*
		 * If this process has children, descend to them next,
		 * otherwise do any siblings, and if done with this level,
		 * follow back up the tree (but not past top).
		 */
		if (!LIST_EMPTY(&p->p_children))
			p = LIST_FIRST(&p->p_children);
		else for (;;) {
			if (p == top) {
				PROC_LOCK(p);
				return (ret);
			}
			if (LIST_NEXT(p, p_sibling)) {
				p = LIST_NEXT(p, p_sibling);
				break;
			}
			p = p->p_pptr;
		}
		PROC_LOCK(p);
	}
}

static int
protect_set(struct thread *td, struct proc *p, int flags)
{
	int error, ret;

	switch (PPROT_OP(flags)) {
	case PPROT_SET:
	case PPROT_CLEAR:
		break;
	default:
		return (EINVAL);
	}

	if ((PPROT_FLAGS(flags) & ~(PPROT_DESCEND | PPROT_INHERIT)) != 0)
		return (EINVAL);

	error = priv_check(td, PRIV_VM_MADV_PROTECT);
	if (error)
		return (error);

	if (flags & PPROT_DESCEND)
		ret = protect_setchildren(td, p, flags);
	else
		ret = protect_setchild(td, p, flags);
	if (ret == 0)
		return (EPERM);
	return (0);
}

static int
reap_acquire(struct thread *td, struct proc *p)
{

	sx_assert(&proctree_lock, SX_XLOCKED);
	if (p != curproc)
		return (EPERM);
	if ((p->p_treeflag & P_TREE_REAPER) != 0)
		return (EBUSY);
	p->p_treeflag |= P_TREE_REAPER;
	/*
	 * We do not reattach existing children and the whole tree
	 * under them to us, since p->p_reaper already seen them.
	 */
	return (0);
}

static int
reap_release(struct thread *td, struct proc *p)
{

	sx_assert(&proctree_lock, SX_XLOCKED);
	if (p != curproc)
		return (EPERM);
	if (p == initproc)
		return (EINVAL);
	if ((p->p_treeflag & P_TREE_REAPER) == 0)
		return (EINVAL);
	reaper_abandon_children(p, false);
	return (0);
}

static int
reap_status(struct thread *td, struct proc *p,
    struct procctl_reaper_status *rs)
{
	struct proc *reap, *p2;

	sx_assert(&proctree_lock, SX_LOCKED);
	bzero(rs, sizeof(*rs));
	if ((p->p_treeflag & P_TREE_REAPER) == 0) {
		reap = p->p_reaper;
	} else {
		reap = p;
		rs->rs_flags |= REAPER_STATUS_OWNED;
	}
	if (reap == initproc)
		rs->rs_flags |= REAPER_STATUS_REALINIT;
	rs->rs_reaper = reap->p_pid;
	rs->rs_descendants = 0;
	rs->rs_children = 0;
	if (!LIST_EMPTY(&reap->p_reaplist)) {
		KASSERT(!LIST_EMPTY(&reap->p_children), ("no children"));
		rs->rs_pid = LIST_FIRST(&reap->p_children)->p_pid;
		LIST_FOREACH(p2, &reap->p_reaplist, p_reapsibling) {
			if (proc_realparent(p2) == reap)
				rs->rs_children++;
			rs->rs_descendants++;
		}
	} else {
		rs->rs_pid = -1;
		KASSERT(LIST_EMPTY(&reap->p_reaplist), ("reap children list"));
		KASSERT(LIST_EMPTY(&reap->p_children), ("children list"));
	}
	return (0);
}

static int
reap_getpids(struct thread *td, struct proc *p, struct procctl_reaper_pids *rp)
{
	struct proc *reap, *p2;
	struct procctl_reaper_pidinfo *pi, *pip;
	u_int i, n;
	int error;

	sx_assert(&proctree_lock, SX_LOCKED);
	PROC_UNLOCK(p);
	reap = (p->p_treeflag & P_TREE_REAPER) == 0 ? p->p_reaper : p;
	n = i = 0;
	error = 0;
	LIST_FOREACH(p2, &reap->p_reaplist, p_reapsibling)
		n++;
	sx_unlock(&proctree_lock);
	if (rp->rp_count < n)
		n = rp->rp_count;
	pi = malloc(n * sizeof(*pi), M_TEMP, M_WAITOK);
	sx_slock(&proctree_lock);
	LIST_FOREACH(p2, &reap->p_reaplist, p_reapsibling) {
		if (i == n)
			break;
		pip = &pi[i];
		bzero(pip, sizeof(*pip));
		pip->pi_pid = p2->p_pid;
		pip->pi_subtree = p2->p_reapsubtree;
		pip->pi_flags = REAPER_PIDINFO_VALID;
		if (proc_realparent(p2) == reap)
			pip->pi_flags |= REAPER_PIDINFO_CHILD;
		i++;
	}
	sx_sunlock(&proctree_lock);
	error = copyout(pi, rp->rp_pids, i * sizeof(*pi));
	free(pi, M_TEMP);
	sx_slock(&proctree_lock);
	PROC_LOCK(p);
	return (error);
}

static int
reap_kill(struct thread *td, struct proc *p, struct procctl_reaper_kill *rk)
{
	struct proc *reap, *p2;
	ksiginfo_t ksi;
	int error, error1;

	sx_assert(&proctree_lock, SX_LOCKED);
	PROC_UNLOCK(p);
	if (IN_CAPABILITY_MODE(td))
		return (ECAPMODE);
	if (rk->rk_sig <= 0 || rk->rk_sig > _SIG_MAXSIG)
		return (EINVAL);
	if ((rk->rk_flags & ~REAPER_KILL_CHILDREN) != 0)
		return (EINVAL);
	reap = (p->p_treeflag & P_TREE_REAPER) == 0 ? p->p_reaper : p;
	ksiginfo_init(&ksi);
	ksi.ksi_signo = rk->rk_sig;
	ksi.ksi_code = SI_USER;
	ksi.ksi_pid = td->td_proc->p_pid;
	ksi.ksi_uid = td->td_ucred->cr_ruid;
	error = ESRCH;
	rk->rk_killed = 0;
	rk->rk_fpid = -1;
	for (p2 = (rk->rk_flags & REAPER_KILL_CHILDREN) != 0 ?
	    LIST_FIRST(&reap->p_children) : LIST_FIRST(&reap->p_reaplist);
	    p2 != NULL;
	    p2 = (rk->rk_flags & REAPER_KILL_CHILDREN) != 0 ?
	    LIST_NEXT(p2, p_sibling) : LIST_NEXT(p2, p_reapsibling)) {
		if ((rk->rk_flags & REAPER_KILL_SUBTREE) != 0 &&
		    p2->p_reapsubtree != rk->rk_subtree)
			continue;
		PROC_LOCK(p2);
		error1 = p_cansignal(td, p2, rk->rk_sig);
		if (error1 == 0) {
			pksignal(p2, rk->rk_sig, &ksi);
			rk->rk_killed++;
			error = error1;
		} else if (error == ESRCH) {
			error = error1;
			rk->rk_fpid = p2->p_pid;
		}
		PROC_UNLOCK(p2);
		/* Do not end the loop on error, signal everything we can. */
	}
	PROC_LOCK(p);
	return (error);
}

static int
trace_ctl(struct thread *td, struct proc *p, int state)
{

	PROC_LOCK_ASSERT(p, MA_OWNED);

	/*
	 * Ktrace changes p_traceflag from or to zero under the
	 * process lock, so the test does not need to acquire ktrace
	 * mutex.
	 */
	if ((p->p_flag & P_TRACED) != 0 || p->p_traceflag != 0)
		return (EBUSY);

	switch (state) {
	case PROC_TRACE_CTL_ENABLE:
		if (td->td_proc != p)
			return (EPERM);
		p->p_flag2 &= ~(P2_NOTRACE | P2_NOTRACE_EXEC);
		break;
	case PROC_TRACE_CTL_DISABLE_EXEC:
		p->p_flag2 |= P2_NOTRACE_EXEC | P2_NOTRACE;
		break;
	case PROC_TRACE_CTL_DISABLE:
		if ((p->p_flag2 & P2_NOTRACE_EXEC) != 0) {
			KASSERT((p->p_flag2 & P2_NOTRACE) != 0,
			    ("dandling P2_NOTRACE_EXEC"));
			if (td->td_proc != p)
				return (EPERM);
			p->p_flag2 &= ~P2_NOTRACE_EXEC;
		} else {
			p->p_flag2 |= P2_NOTRACE;
		}
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
trace_status(struct thread *td, struct proc *p, int *data)
{

	if ((p->p_flag2 & P2_NOTRACE) != 0) {
		KASSERT((p->p_flag & P_TRACED) == 0,
		    ("%d traced but tracing disabled", p->p_pid));
		*data = -1;
	} else if ((p->p_flag & P_TRACED) != 0) {
		*data = p->p_pptr->p_pid;
	} else {
		*data = 0;
	}
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct procctl_args {
	idtype_t idtype;
	id_t	id;
	int	com;
	void	*data;
};
#endif
/* ARGSUSED */
int
sys_procctl(struct thread *td, struct procctl_args *uap)
{
	void *data;
	union {
		struct procctl_reaper_status rs;
		struct procctl_reaper_pids rp;
		struct procctl_reaper_kill rk;
	} x;
	int error, error1, flags;

	switch (uap->com) {
	case PROC_SPROTECT:
	case PROC_TRACE_CTL:
		error = copyin(uap->data, &flags, sizeof(flags));
		if (error != 0)
			return (error);
		data = &flags;
		break;
	case PROC_REAP_ACQUIRE:
	case PROC_REAP_RELEASE:
		if (uap->data != NULL)
			return (EINVAL);
		data = NULL;
		break;
	case PROC_REAP_STATUS:
		data = &x.rs;
		break;
	case PROC_REAP_GETPIDS:
		error = copyin(uap->data, &x.rp, sizeof(x.rp));
		if (error != 0)
			return (error);
		data = &x.rp;
		break;
	case PROC_REAP_KILL:
		error = copyin(uap->data, &x.rk, sizeof(x.rk));
		if (error != 0)
			return (error);
		data = &x.rk;
		break;
	case PROC_TRACE_STATUS:
		data = &flags;
		break;
	default:
		return (EINVAL);
	}
	error = kern_procctl(td, uap->idtype, uap->id, uap->com, data);
	switch (uap->com) {
	case PROC_REAP_STATUS:
		if (error == 0)
			error = copyout(&x.rs, uap->data, sizeof(x.rs));
		break;
	case PROC_REAP_KILL:
		error1 = copyout(&x.rk, uap->data, sizeof(x.rk));
		if (error == 0)
			error = error1;
		break;
	case PROC_TRACE_STATUS:
		if (error == 0)
			error = copyout(&flags, uap->data, sizeof(flags));
		break;
	}
	return (error);
}

static int
kern_procctl_single(struct thread *td, struct proc *p, int com, void *data)
{

	PROC_LOCK_ASSERT(p, MA_OWNED);
	switch (com) {
	case PROC_SPROTECT:
		return (protect_set(td, p, *(int *)data));
	case PROC_REAP_ACQUIRE:
		return (reap_acquire(td, p));
	case PROC_REAP_RELEASE:
		return (reap_release(td, p));
	case PROC_REAP_STATUS:
		return (reap_status(td, p, data));
	case PROC_REAP_GETPIDS:
		return (reap_getpids(td, p, data));
	case PROC_REAP_KILL:
		return (reap_kill(td, p, data));
	case PROC_TRACE_CTL:
		return (trace_ctl(td, p, *(int *)data));
	case PROC_TRACE_STATUS:
		return (trace_status(td, p, data));
	default:
		return (EINVAL);
	}
}

int
kern_procctl(struct thread *td, idtype_t idtype, id_t id, int com, void *data)
{
	struct pgrp *pg;
	struct proc *p;
	int error, first_error, ok;
	bool tree_locked;

	switch (com) {
	case PROC_REAP_ACQUIRE:
	case PROC_REAP_RELEASE:
	case PROC_REAP_STATUS:
	case PROC_REAP_GETPIDS:
	case PROC_REAP_KILL:
	case PROC_TRACE_STATUS:
		if (idtype != P_PID)
			return (EINVAL);
	}

	switch (com) {
	case PROC_SPROTECT:
	case PROC_REAP_STATUS:
	case PROC_REAP_GETPIDS:
	case PROC_REAP_KILL:
	case PROC_TRACE_CTL:
		sx_slock(&proctree_lock);
		tree_locked = true;
		break;
	case PROC_REAP_ACQUIRE:
	case PROC_REAP_RELEASE:
		sx_xlock(&proctree_lock);
		tree_locked = true;
		break;
	case PROC_TRACE_STATUS:
		tree_locked = false;
		break;
	default:
		return (EINVAL);
	}

	switch (idtype) {
	case P_PID:
		p = pfind(id);
		if (p == NULL) {
			error = ESRCH;
			break;
		}
		error = p_cansee(td, p);
		if (error == 0)
			error = kern_procctl_single(td, p, com, data);
		PROC_UNLOCK(p);
		break;
	case P_PGID:
		/*
		 * Attempt to apply the operation to all members of the
		 * group.  Ignore processes in the group that can't be
		 * seen.  Ignore errors so long as at least one process is
		 * able to complete the request successfully.
		 */
		pg = pgfind(id);
		if (pg == NULL) {
			error = ESRCH;
			break;
		}
		PGRP_UNLOCK(pg);
		ok = 0;
		first_error = 0;
		LIST_FOREACH(p, &pg->pg_members, p_pglist) {
			PROC_LOCK(p);
			if (p->p_state == PRS_NEW || p_cansee(td, p) != 0) {
				PROC_UNLOCK(p);
				continue;
			}
			error = kern_procctl_single(td, p, com, data);
			PROC_UNLOCK(p);
			if (error == 0)
				ok = 1;
			else if (first_error == 0)
				first_error = error;
		}
		if (ok)
			error = 0;
		else if (first_error != 0)
			error = first_error;
		else
			/*
			 * Was not able to see any processes in the
			 * process group.
			 */
			error = ESRCH;
		break;
	default:
		error = EINVAL;
		break;
	}
	if (tree_locked)
		sx_unlock(&proctree_lock);
	return (error);
}
