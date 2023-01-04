/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software donated to Berkeley by
 * Jan-Simon Pendry.
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
 *	@(#)kernfs_vfsops.c	8.4 (Berkeley) 1/21/94
 */

/*
 * Kernel params Filesystem
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/malloc.h>

#include <miscfs/specfs/specdev.h>
#include <miscfs/kernfs/kernfs.h>

struct vnode *rrootvp;

/*
 * Create a vnode for a character device.
 */
int
cdevvp(dev, vpp)
	dev_t dev;
	struct vnode **vpp;
{
	register struct vnode *vp;
	struct vnode *nvp;
	int error;

	if (dev == NODEV)
		return (0);
	error = getnewvnode(VT_NON, (struct mount *)0, spec_vnodeop_p, &nvp);
	if (error) {
		*vpp = 0;
		return (error);
	}
	vp = nvp;
	insmntque(vp, 0);

	vp->v_type = VCHR;
	if (nvp = checkalias(vp, dev, (struct mount *)0)) {
		vput(vp);
		vp = nvp;
	}
	*vpp = vp;
	return (0);
}

mach_error_t kernfs_init()
{
	int cmaj;
	int bmaj = major(rootdev);
	int error = ENXIO;

#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_init\n");		/* printed during system boot */
#endif

#ifdef KERNFS_RROOTVP
	for (cmaj = 0; cmaj < nchrdev; cmaj++) {
		if (cdevsw[cmaj].d_open == bdevsw[bmaj].d_open) {
			dev_t cdev = makedev(cmaj, minor(rootdev));
			error = cdevvp(cdev, &rrootvp);
			if (error == 0)
				break;
		}
	}

	if (error) {
		printf("kernfs: no raw boot device\n");
		rrootvp = 0;
	}
#endif
	return KERN_SUCCESS;
}

/*
 * Mount the Kernel params filesystem
 */
mach_error_t kernfs_mount(mp, path, data, ndp, p)
	struct mount *mp;
	char *path;
	caddr_t data;
	struct nameidata *ndp;
	struct proc *p;
{
	int error = 0;
	u_int size;
	struct kernfs_mount *fmp;
	struct vnode *rvp;

#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_mount(mp = %x)\n", mp);
#endif

	/*
	 * Update is a no-op
	 */
	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);

	error = getnewvnode(VT_KERNFS, mp, kernfs_vnodeop_p, &rvp);	/* XXX */
	if (error)
		return (error);

	MALLOC(fmp, struct kernfs_mount *, sizeof(struct kernfs_mount),
				M_UFSMNT, M_WAITOK);	/* XXX */
        insmntque(rvp, mp);
	rvp->v_type = VDIR;
	rvp->v_flag |= VROOT;
#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_mount: root vp = %x\n", rvp);
#endif
	fmp->kf_root = rvp;
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_data = (qaddr_t) fmp;
	getnewfsid(mp, MOUNT_KERNFS);

	(void) copyinstr(path, mp->mnt_stat.f_mntonname, MNAMELEN - 1, &size);
	bzero(mp->mnt_stat.f_mntonname + size, MNAMELEN - size);
	bzero(mp->mnt_stat.f_mntfromname, MNAMELEN);
	bcopy("kernfs", mp->mnt_stat.f_mntfromname, sizeof("kernfs"));
#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_mount: at %s\n", mp->mnt_stat.f_mntonname);
#endif
	return (0);
}

mach_error_t kernfs_start(mp, flags, p)
	struct mount *mp;
	int flags;
	struct proc *p;
{
	return (0);
}

mach_error_t kernfs_unmount(mp, mntflags, p)
	struct mount *mp;
	int mntflags;
	struct proc *p;
{
	int error;
	int flags = 0;
	extern int doforce;
	struct vnode *rootvp = VFSTOKERNFS(mp)->kf_root;

#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_unmount(mp = %x)\n", mp);
#endif

	if (mntflags & MNT_FORCE) {
		/* kernfs can never be rootfs so don't check for it */
		if (!doforce)
			return (EINVAL);
		flags |= FORCECLOSE;
	}

	/*
	 * Clear out buffer cache.  I don't think we
	 * ever get anything cached at this level at the
	 * moment, but who knows...
	 */
	if (rootvp->v_usecount > 1)
		return (EBUSY);
#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_unmount: calling vflush\n");
#endif
	if (error = vflush(mp, rootvp, flags))
		return (error);

#ifdef KERNFS_DIAGNOSTIC
	vprint("kernfs root", rootvp);
#endif
	/*
	 * Release reference on underlying root vnode
	 */
	vrele(rootvp);
	/*
	 * And blow it away for future re-use
	 */
	vgone(rootvp);
	/*
	 * Finally, throw away the kernfs_mount structure
	 */
	bsd_free(mp->mnt_data, M_UFSMNT);	/* XXX */
	mp->mnt_data = 0;
	return 0;
}

mach_error_t kernfs_root(mp, vpp)
	struct mount *mp;
	struct vnode **vpp;
{
	struct vnode *vp;

#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_root(mp = %x)\n", mp);
#endif

	/*
	 * Return locked reference to root.
	 */
	vp = VFSTOKERNFS(mp)->kf_root;
	VREF(vp);
	VOP_LOCK(vp);
	*vpp = vp;
	return (0);
}

mach_error_t kernfs_quotactl(mp, cmd, uid, arg, p)
	struct mount *mp;
	int cmd;
	uid_t uid;
	caddr_t arg;
	struct proc *p;
{
	return (EOPNOTSUPP);
}

mach_error_t kernfs_statfs(mp, sbp, p)
	struct mount *mp;
	struct statfs *sbp;
	struct proc *p;
{
#ifdef KERNFS_DIAGNOSTIC
	printf("kernfs_statfs(mp = %x)\n", mp);
#endif

	sbp->f_type = MOUNT_KERNFS;
	sbp->f_flags = 0;
	sbp->f_bsize = DEV_BSIZE;
	sbp->f_iosize = DEV_BSIZE;
	sbp->f_blocks = 2;		/* 1K to keep df happy */
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 0;
	sbp->f_ffree = 0;
	if (sbp != &mp->mnt_stat) {
		bcopy(&mp->mnt_stat.f_fsid, &sbp->f_fsid, sizeof(sbp->f_fsid));
		bcopy(mp->mnt_stat.f_mntonname, sbp->f_mntonname, MNAMELEN);
		bcopy(mp->mnt_stat.f_mntfromname, sbp->f_mntfromname, MNAMELEN);
	}
        strncpy(sbp->f_fstypename, mp->mnt_op->vfs_name, MFSNAMELEN);
	return (0);
}

mach_error_t kernfs_sync(mp, waitfor)
	struct mount *mp;
	int waitfor;
{
	return (0);
}

/*
 * Kernfs flat namespace lookup.
 * Currently unsupported.
 */
mach_error_t kernfs_vget(mp, ino, vpp)
	struct mount *mp;
	ino_t ino;
	struct vnode **vpp;
{

	return (EOPNOTSUPP);
}


mach_error_t kernfs_fhtovp(mp, fhp, setgen, vpp)
	struct mount *mp;
	struct fid *fhp;
	int setgen;
	struct vnode **vpp;
{
	return (EOPNOTSUPP);
}

mach_error_t kernfs_vptofh(vp, fhp)
	struct vnode *vp;
	struct fid *fhp;
{
	return (EOPNOTSUPP);
}

struct vfsops kernfs_vfsops = {
	"kernfs",
	kernfs_mount,
	kernfs_start,
	kernfs_unmount,
	kernfs_root,
	kernfs_quotactl,
	kernfs_statfs,
	kernfs_sync,
	kernfs_vget,
	kernfs_fhtovp,
	kernfs_vptofh,
	kernfs_init,
};
