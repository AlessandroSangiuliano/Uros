/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

/*
 * File system independent interfaces definitions
 */

#ifndef _FS_FILE_SYSTEM_H_
#define _FS_FILE_SYSTEM_H_

#include <string.h>
#include <mach.h>
#include <device/device_types.h>
#include <mach/boot_info.h>

typedef struct fs_ops 	*fs_ops_t;
typedef void 		*fs_private_t;

struct page_cache;		/* page_cache.h */
struct blk_dev;			/* libblk/blk.h */

struct device {
	mach_port_t	dev_port;	/* port to device (legacy, use blk) */
	unsigned int	rec_size;	/* record size */
	struct page_cache *cache;	/* optional block cache (NULL = uncached) */
	struct blk_dev	*blk;		/* block layer handle (NULL = direct) */
	void		*mount_data;	/* FS-specific per-mount state (NULL = none) */
};

/*
 * In-core open file.
 */
struct file {
	fs_ops_t	f_ops;
	fs_private_t	f_private;	/* file system dependent */
	struct device	f_dev;		/* device */
};

/*
 * One directory entry returned by readdir_file().  Fixed-size so the
 * bootstrap can enumerate module directories with a plain stack/VM
 * buffer, no malloc.
 */
#define FS_DIRENT_NAME_MAX	255

#define FS_DT_UNKNOWN	0
#define FS_DT_REG	8
#define FS_DT_DIR	4

struct fs_dirent {
	unsigned long	ino;				/* inode number */
	unsigned char	type;				/* FS_DT_* */
	char		name[FS_DIRENT_NAME_MAX + 1];	/* NUL-terminated */
};

struct fs_ops {
	int		(*open_file)(struct device *,
				     const char *,
				     fs_private_t *);
	void		(*close_file)(fs_private_t);
    	int 		(*read_file)(fs_private_t,
				     vm_offset_t,
				     vm_offset_t,
				     vm_size_t);
    	size_t 		(*file_size)(fs_private_t);
	boolean_t 	(*file_is_directory)(fs_private_t);
	boolean_t 	(*file_is_executable)(fs_private_t);
	/*
	 * Enumerate directory entries.  fp must be a directory opened
	 * via open_file().  Fills up to max entries into out[], writes
	 * the count into *out_count.  Returns 0 on success.  May be
	 * NULL — caller should check.
	 */
	int		(*readdir)(fs_private_t,
				   struct fs_dirent *out,
				   unsigned int max,
				   unsigned int *out_count);
	vm_size_t	mount_size;	/* per-mount state size; 0 = none.
					   When > 0, the dispatch layer
					   vm_allocate's mount_data on
					   first open. */
};

/*
 * Error codes for file system errors.
 */
#define	FS_NOT_DIRECTORY	5000	/* not a directory */
#define	FS_NO_ENTRY		5001	/* name not found */
#define	FS_NAME_TOO_LONG	5002	/* name too long */
#define	FS_SYMLINK_LOOP		5003	/* symbolic link loop */
#define	FS_INVALID_FS		5004	/* bad file system */
#define	FS_NOT_IN_FILE		5005	/* offset not in file */
#define	FS_INVALID_PARAMETER	5006	/* bad parameter to a routine */

extern int open_file(mach_port_t, const char *, struct file *);
extern int open_file_on_port(mach_port_t, const char *, struct file *);
extern void close_file(struct file *);
extern int read_file(struct file *, vm_offset_t, vm_offset_t, vm_size_t);
extern size_t file_size(struct file *);
extern boolean_t file_is_directory(struct file *);
extern boolean_t file_is_executable(struct file *);
extern int readdir_file(struct file *, struct fs_dirent *, unsigned int,
			unsigned int *);

#endif /* _FS_FILE_SYSTEM_H_ */
