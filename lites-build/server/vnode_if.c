/*
 * This file is produced automatically.
 * Do not modify anything in here by hand.
 *
 * Created from @(#)vnode_if.sh	8.1 (Berkeley) 6/10/93
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/vnode.h>

struct vnodeop_desc vop_default_desc = {
	0,
	"default",
	0,
	NULL,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_lookup_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_lookup_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_lookup_desc = {
	0,
	"vop_lookup",
	0,
	vop_lookup_vp_offsets,
	VOPARG_OFFSETOF(struct vop_lookup_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_lookup_args,a_cnp),
	NULL,
};

int vop_create_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_create_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_create_desc = {
	0,
	"vop_create",
	VDESC_VP0_WILLRELE,
	vop_create_vp_offsets,
	VOPARG_OFFSETOF(struct vop_create_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_create_args,a_cnp),
	NULL,
};

int vop_mknod_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_mknod_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_mknod_desc = {
	0,
	"vop_mknod",
	VDESC_VP0_WILLRELE|VDESC_VPP_WILLRELE,
	vop_mknod_vp_offsets,
	VOPARG_OFFSETOF(struct vop_mknod_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_mknod_args,a_cnp),
	NULL,
};

int vop_open_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_open_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_open_desc = {
	0,
	"vop_open",
	0,
	vop_open_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_open_args,a_cred),
	VOPARG_OFFSETOF(struct vop_open_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_close_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_close_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_close_desc = {
	0,
	"vop_close",
	0,
	vop_close_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_close_args,a_cred),
	VOPARG_OFFSETOF(struct vop_close_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_access_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_access_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_access_desc = {
	0,
	"vop_access",
	0,
	vop_access_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_access_args,a_cred),
	VOPARG_OFFSETOF(struct vop_access_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_getattr_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_getattr_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_getattr_desc = {
	0,
	"vop_getattr",
	0,
	vop_getattr_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_getattr_args,a_cred),
	VOPARG_OFFSETOF(struct vop_getattr_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_setattr_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_setattr_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_setattr_desc = {
	0,
	"vop_setattr",
	0,
	vop_setattr_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_setattr_args,a_cred),
	VOPARG_OFFSETOF(struct vop_setattr_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_read_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_read_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_read_desc = {
	0,
	"vop_read",
	0,
	vop_read_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_read_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_write_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_write_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_write_desc = {
	0,
	"vop_write",
	0,
	vop_write_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_write_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_ioctl_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_ioctl_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_ioctl_desc = {
	0,
	"vop_ioctl",
	0,
	vop_ioctl_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_ioctl_args,a_cred),
	VOPARG_OFFSETOF(struct vop_ioctl_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_select_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_select_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_select_desc = {
	0,
	"vop_select",
	0,
	vop_select_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_select_args,a_cred),
	VOPARG_OFFSETOF(struct vop_select_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_mmap_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_mmap_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_mmap_desc = {
	0,
	"vop_mmap",
	0,
	vop_mmap_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_mmap_args,a_cred),
	VOPARG_OFFSETOF(struct vop_mmap_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_fsync_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_fsync_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_fsync_desc = {
	0,
	"vop_fsync",
	0,
	vop_fsync_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_fsync_args,a_cred),
	VOPARG_OFFSETOF(struct vop_fsync_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_seek_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_seek_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_seek_desc = {
	0,
	"vop_seek",
	0,
	vop_seek_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_seek_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_remove_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_remove_args,a_dvp),
	VOPARG_OFFSETOF(struct vop_remove_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_remove_desc = {
	0,
	"vop_remove",
	VDESC_VP0_WILLRELE|VDESC_VP1_WILLRELE,
	vop_remove_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_remove_args,a_cnp),
	NULL,
};

int vop_link_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_link_args,a_vp),
	VOPARG_OFFSETOF(struct vop_link_args,a_tdvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_link_desc = {
	0,
	"vop_link",
	VDESC_VP0_WILLRELE,
	vop_link_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_link_args,a_cnp),
	NULL,
};

int vop_rename_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_rename_args,a_fdvp),
	VOPARG_OFFSETOF(struct vop_rename_args,a_fvp),
	VOPARG_OFFSETOF(struct vop_rename_args,a_tdvp),
	VOPARG_OFFSETOF(struct vop_rename_args,a_tvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_rename_desc = {
	0,
	"vop_rename",
	VDESC_VP0_WILLRELE|VDESC_VP1_WILLRELE|VDESC_VP2_WILLRELE|VDESC_VP3_WILLRELE,
	vop_rename_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_rename_args,a_fcnp),
	NULL,
};

int vop_mkdir_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_mkdir_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_mkdir_desc = {
	0,
	"vop_mkdir",
	VDESC_VP0_WILLRELE,
	vop_mkdir_vp_offsets,
	VOPARG_OFFSETOF(struct vop_mkdir_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_mkdir_args,a_cnp),
	NULL,
};

int vop_rmdir_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_rmdir_args,a_dvp),
	VOPARG_OFFSETOF(struct vop_rmdir_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_rmdir_desc = {
	0,
	"vop_rmdir",
	VDESC_VP0_WILLRELE|VDESC_VP1_WILLRELE,
	vop_rmdir_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_rmdir_args,a_cnp),
	NULL,
};

int vop_symlink_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_symlink_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_symlink_desc = {
	0,
	"vop_symlink",
	VDESC_VP0_WILLRELE|VDESC_VPP_WILLRELE,
	vop_symlink_vp_offsets,
	VOPARG_OFFSETOF(struct vop_symlink_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_symlink_args,a_cnp),
	NULL,
};

int vop_readdir_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_readdir_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_readdir_desc = {
	0,
	"vop_readdir",
	0,
	vop_readdir_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_readdir_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_readlink_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_readlink_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_readlink_desc = {
	0,
	"vop_readlink",
	0,
	vop_readlink_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_readlink_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_abortop_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_abortop_args,a_dvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_abortop_desc = {
	0,
	"vop_abortop",
	0,
	vop_abortop_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_abortop_args,a_cnp),
	NULL,
};

int vop_inactive_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_inactive_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_inactive_desc = {
	0,
	"vop_inactive",
	0,
	vop_inactive_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_reclaim_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_reclaim_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_reclaim_desc = {
	0,
	"vop_reclaim",
	0,
	vop_reclaim_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_lock_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_lock_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_lock_desc = {
	0,
	"vop_lock",
	0,
	vop_lock_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_unlock_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_unlock_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_unlock_desc = {
	0,
	"vop_unlock",
	0,
	vop_unlock_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_bmap_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_bmap_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_bmap_desc = {
	0,
	"vop_bmap",
	0,
	vop_bmap_vp_offsets,
	VOPARG_OFFSETOF(struct vop_bmap_args,a_vpp),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_print_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_print_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_print_desc = {
	0,
	"vop_print",
	0,
	vop_print_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_islocked_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_islocked_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_islocked_desc = {
	0,
	"vop_islocked",
	0,
	vop_islocked_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_pathconf_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_pathconf_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_pathconf_desc = {
	0,
	"vop_pathconf",
	0,
	vop_pathconf_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_advlock_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_advlock_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_advlock_desc = {
	0,
	"vop_advlock",
	0,
	vop_advlock_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_blkatoff_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_blkatoff_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_blkatoff_desc = {
	0,
	"vop_blkatoff",
	0,
	vop_blkatoff_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_valloc_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_valloc_args,a_pvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_valloc_desc = {
	0,
	"vop_valloc",
	0,
	vop_valloc_vp_offsets,
	VOPARG_OFFSETOF(struct vop_valloc_args,a_vpp),
	VOPARG_OFFSETOF(struct vop_valloc_args,a_cred),
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_reallocblks_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_reallocblks_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_reallocblks_desc = {
	0,
	"vop_reallocblks",
	0,
	vop_reallocblks_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_vfree_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_vfree_args,a_pvp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_vfree_desc = {
	0,
	"vop_vfree",
	0,
	vop_vfree_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_truncate_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_truncate_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_truncate_desc = {
	0,
	"vop_truncate",
	0,
	vop_truncate_vp_offsets,
	VDESC_NO_OFFSET,
	VOPARG_OFFSETOF(struct vop_truncate_args,a_cred),
	VOPARG_OFFSETOF(struct vop_truncate_args,a_p),
	VDESC_NO_OFFSET,
	NULL,
};

int vop_update_vp_offsets[] = {
	VOPARG_OFFSETOF(struct vop_update_args,a_vp),
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_update_desc = {
	0,
	"vop_update",
	0,
	vop_update_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

int vop_strategy_vp_offsets[] = {
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_strategy_desc = {
	0,
	"vop_strategy",
	0,
	vop_strategy_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};
int vop_bwrite_vp_offsets[] = {
	VDESC_NO_OFFSET
};
struct vnodeop_desc vop_bwrite_desc = {
	0,
	"vop_bwrite",
	0,
	vop_bwrite_vp_offsets,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	VDESC_NO_OFFSET,
	NULL,
};

struct vnodeop_desc *vfs_op_descs[] = {
	&vop_default_desc,	/* MUST BE FIRST */
	&vop_strategy_desc,	/* XXX: SPECIAL CASE */
	&vop_bwrite_desc,	/* XXX: SPECIAL CASE */
	&vop_lookup_desc,
	&vop_create_desc,
	&vop_mknod_desc,
	&vop_open_desc,
	&vop_close_desc,
	&vop_access_desc,
	&vop_getattr_desc,
	&vop_setattr_desc,
	&vop_read_desc,
	&vop_write_desc,
	&vop_ioctl_desc,
	&vop_select_desc,
	&vop_mmap_desc,
	&vop_fsync_desc,
	&vop_seek_desc,
	&vop_remove_desc,
	&vop_link_desc,
	&vop_rename_desc,
	&vop_mkdir_desc,
	&vop_rmdir_desc,
	&vop_symlink_desc,
	&vop_readdir_desc,
	&vop_readlink_desc,
	&vop_abortop_desc,
	&vop_inactive_desc,
	&vop_reclaim_desc,
	&vop_lock_desc,
	&vop_unlock_desc,
	&vop_bmap_desc,
	&vop_print_desc,
	&vop_islocked_desc,
	&vop_pathconf_desc,
	&vop_advlock_desc,
	&vop_blkatoff_desc,
	&vop_valloc_desc,
	&vop_reallocblks_desc,
	&vop_vfree_desc,
	&vop_truncate_desc,
	&vop_update_desc,
	NULL
};
