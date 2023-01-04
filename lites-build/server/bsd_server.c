/* Module bsd_1 */

#define EXPORT_BOOLEAN
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/mig_support.h>

#ifndef	mig_internal
#define	mig_internal	static
#endif

#ifndef	mig_external
#define mig_external
#endif

#ifndef	TypeCheck
#define	TypeCheck 1
#endif

#ifndef	UseExternRCSId
#define	UseExternRCSId		1
#endif

#define msgh_request_port	msgh_local_port
#define MACH_MSGH_BITS_REQUEST(bits)	MACH_MSGH_BITS_LOCAL(bits)
#define msgh_reply_port		msgh_remote_port
#define MACH_MSGH_BITS_REPLY(bits)	MACH_MSGH_BITS_REMOTE(bits)

#include <mach/std_types.h>
#include <mach/mach_types.h>
#include <serv/bsd_types.h>
#include <mach_debug/mach_debug_types.h>

/* Routine bsd_execve */
mig_internal void _Xbsd_execve
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t arg_addrType;
		vm_address_t arg_addr;
		mach_msg_type_t arg_sizeType;
		vm_size_t arg_size;
		mach_msg_type_t arg_countType;
		int arg_count;
		mach_msg_type_t env_countType;
		int env_count;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_execve
		(mach_port_t proc_port, mach_port_seqno_t seqno, vm_address_t arg_addr, vm_size_t arg_size, int arg_count, int env_count, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t arg_addrCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t arg_sizeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t arg_countCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t env_countCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 60) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->arg_addrType != * (int *) &arg_addrCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->arg_sizeType != * (int *) &arg_sizeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->arg_countType != * (int *) &arg_countCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->env_countType != * (int *) &env_countCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 60 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_execve(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->arg_addr, In0P->arg_size, In0P->arg_count, In0P->env_count, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_after_exec */
mig_internal void _Xbsd_after_exec
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t arg_addrType;
		vm_address_t arg_addr;
		mach_msg_type_t arg_sizeType;
		vm_size_t arg_size;
		mach_msg_type_t arg_countType;
		int arg_count;
		mach_msg_type_t env_countType;
		int env_count;
		mach_msg_type_t image_portType;
		mach_port_t image_port;
		mach_msg_type_t emul_nameType;
		char emul_name[1024];
		mach_msg_type_t fnameType;
		char fname[1024];
		mach_msg_type_t cfnameType;
		char cfname[1024];
		mach_msg_type_t cfargType;
		char cfarg[1024];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_after_exec
		(mach_port_t proc_port, mach_port_seqno_t seqno, vm_address_t *arg_addr, vm_size_t *arg_size, int *arg_count, int *env_count, mach_port_t *image_port, path_name_t emul_name, mach_msg_type_number_t *emul_nameCnt, path_name_t fname, mach_msg_type_number_t *fnameCnt, path_name_t cfname, mach_msg_type_number_t *cfnameCnt, path_name_t cfarg, mach_msg_type_number_t *cfargCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t arg_addrType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t arg_sizeType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t arg_countType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t env_countType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t image_portType = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t emul_nameType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		1024,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t fnameType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		1024,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cfnameType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		1024,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cfargType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		1024,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_number_t emul_nameCnt;
	char fname[1024];
	mach_msg_type_number_t fnameCnt;
	char cfname[1024];
	mach_msg_type_number_t cfnameCnt;
	char cfarg[1024];
	mach_msg_type_number_t cfargCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 24) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	emul_nameCnt = 1024;

	fnameCnt = 1024;

	cfnameCnt = 1024;

	cfargCnt = 1024;

	OutP->RetCode = bsd_after_exec(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &OutP->arg_addr, &OutP->arg_size, &OutP->arg_count, &OutP->env_count, &OutP->image_port, OutP->emul_name, &emul_nameCnt, fname, &fnameCnt, cfname, &cfnameCnt, cfarg, &cfargCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;

	OutP->arg_addrType = arg_addrType;

	OutP->arg_sizeType = arg_sizeType;

	OutP->arg_countType = arg_countType;

	OutP->env_countType = env_countType;

	OutP->image_portType = image_portType;

	OutP->emul_nameType = emul_nameType;

	OutP->emul_nameType.msgt_number = emul_nameCnt;
	msgh_size_delta = (emul_nameCnt + 3) & ~3;
	msgh_size = 88 + msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 1024);

	OutP->fnameType = fnameType;

	memcpy(OutP->fname, fname, fnameCnt);

	OutP->fnameType.msgt_number = fnameCnt;
	msgh_size_delta = (fnameCnt + 3) & ~3;
	msgh_size += msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 1024);

	OutP->cfnameType = cfnameType;

	memcpy(OutP->cfname, cfname, cfnameCnt);

	OutP->cfnameType.msgt_number = cfnameCnt;
	msgh_size_delta = (cfnameCnt + 3) & ~3;
	msgh_size += msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 1024);

	OutP->cfargType = cfargType;

	memcpy(OutP->cfarg, cfarg, cfargCnt);

	OutP->cfargType.msgt_number = cfargCnt;
	msgh_size += (cfargCnt + 3) & ~3;

	OutP = (Reply *) OutHeadP;
	OutP->Head.msgh_size = msgh_size;
}

/* Routine bsd_vm_map */
mig_internal void _Xbsd_vm_map
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t addressType;
		vm_address_t address;
		mach_msg_type_t sizeType;
		vm_size_t size;
		mach_msg_type_t maskType;
		vm_address_t mask;
		mach_msg_type_t anywhereType;
		boolean_t anywhere;
		mach_msg_type_t memory_object_representativeType;
		mach_port_t memory_object_representative;
		mach_msg_type_t offsetType;
		vm_offset_t offset;
		mach_msg_type_t copyType;
		boolean_t copy;
		mach_msg_type_t cur_protectionType;
		vm_prot_t cur_protection;
		mach_msg_type_t max_protectionType;
		vm_prot_t max_protection;
		mach_msg_type_t inheritanceType;
		vm_inherit_t inheritance;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t addressType;
		vm_address_t address;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_vm_map
		(mach_port_t proc_port, mach_port_seqno_t seqno, vm_address_t *address, vm_size_t size, vm_address_t mask, boolean_t anywhere, mach_port_t memory_object_representative, vm_offset_t offset, boolean_t copy, vm_prot_t cur_protection, vm_prot_t max_protection, vm_inherit_t inheritance);

	static const mach_msg_type_t addressCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t sizeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t maskCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t anywhereCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t memory_object_representativeCheck = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t offsetCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t copyCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cur_protectionCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t max_protectionCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t inheritanceCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t addressType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 104) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->addressType != * (int *) &addressCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sizeType != * (int *) &sizeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->maskType != * (int *) &maskCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->anywhereType != * (int *) &anywhereCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->memory_object_representativeType != * (int *) &memory_object_representativeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->offsetType != * (int *) &offsetCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->copyType != * (int *) &copyCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->cur_protectionType != * (int *) &cur_protectionCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->max_protectionType != * (int *) &max_protectionCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->inheritanceType != * (int *) &inheritanceCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_vm_map(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &In0P->address, In0P->size, In0P->mask, In0P->anywhere, In0P->memory_object_representative, In0P->offset, In0P->copy, In0P->cur_protection, In0P->max_protection, In0P->inheritance);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->addressType = addressType;

	OutP->address = In0P->address;
}

/* Routine bsd_fd_to_file_port */
mig_internal void _Xbsd_fd_to_file_port
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t portType;
		mach_port_t port;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_fd_to_file_port
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, mach_port_t *port);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t portType = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_fd_to_file_port(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, &OutP->port);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 40;

	OutP->portType = portType;
}

/* Routine bsd_file_port_open */
mig_internal void _Xbsd_file_port_open
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t modeType;
		int mode;
		mach_msg_type_t crtmodeType;
		int crtmode;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t portType;
		mach_port_t port;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_file_port_open
		(mach_port_t proc_port, mach_port_seqno_t seqno, int mode, int crtmode, path_name_t fname, mach_msg_type_number_t fnameCnt, mach_port_t *port);

	unsigned int msgh_size;

	static const mach_msg_type_t modeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t crtmodeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t portType = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 44) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->modeType != * (int *) &modeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->crtmodeType != * (int *) &crtmodeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 44 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_file_port_open(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->mode, In0P->crtmode, In0P->fname, In0P->fnameType.msgt_number, &OutP->port);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 40;

	OutP->portType = portType;
}

/* Routine bsd_zone_info */
mig_internal void _Xbsd_zone_info
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t namesCntType;
		mach_msg_type_number_t namesCnt;
		mach_msg_type_t infoCntType;
		mach_msg_type_number_t infoCnt;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_long_t namesType;
		zone_name_t names[25];
		mach_msg_type_long_t infoType;
		zone_info_t info[56];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_zone_info
		(mach_port_t proc_port, mach_port_seqno_t seqno, zone_name_array_t *names, mach_msg_type_number_t *namesCnt, zone_info_array_t *info, mach_msg_type_number_t *infoCnt);

	boolean_t msgh_simple;
	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t namesCntCheck = {
		/* msgt_name = */		MACH_MSG_TYPE_INTEGER_32,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t infoCntCheck = {
		/* msgt_name = */		MACH_MSG_TYPE_INTEGER_32,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t namesType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	2000,
	};

	static const mach_msg_type_long_t infoType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	2,
		/* msgtl_size = */	32,
		/* msgtl_number = */	504,
	};

	mach_msg_type_number_t namesCnt;
	zone_info_t info[56];
	mach_msg_type_number_t infoCnt;

	zone_name_t *namesP;
	zone_info_t *infoP;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 40) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->namesCntType != * (int *) &namesCntCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->infoCntType != * (int *) &infoCntCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	namesP = OutP->names;
	namesCnt = 25;
	if (In0P->namesCnt < namesCnt)
		namesCnt = In0P->namesCnt;

	infoP = info;
	infoCnt = 56;
	if (In0P->infoCnt < infoCnt)
		infoCnt = In0P->infoCnt;

	OutP->RetCode = bsd_zone_info(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &namesP, &namesCnt, &infoP, &infoCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	msgh_simple = TRUE;

	OutP->namesType = namesType;
	if (namesP != OutP->names) {
		OutP->namesType.msgtl_header.msgt_inline = FALSE;
		OutP->namesType.msgtl_header.msgt_deallocate = TRUE;
		*((zone_name_t **)OutP->names) = namesP;
		msgh_simple = FALSE;
	}

	OutP->namesType.msgtl_number = 80 * namesCnt;
	msgh_size_delta = (OutP->namesType.msgtl_header.msgt_inline) ? 80 * namesCnt : sizeof(zone_name_t *);
	msgh_size = 56 + msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 2000);

	OutP->infoType = infoType;

	if (infoP != info) {
		OutP->infoType.msgtl_header.msgt_inline = FALSE;
		OutP->infoType.msgtl_header.msgt_deallocate = TRUE;
		*((zone_info_t **)OutP->info) = infoP;
		msgh_simple = FALSE;
	}
	else {
		memcpy(OutP->info, info, 36 * infoCnt);
	}

	OutP->infoType.msgtl_number = 9 * infoCnt;
	msgh_size += (OutP->infoType.msgtl_header.msgt_inline) ? 36 * infoCnt : sizeof(zone_info_t *);

	OutP = (Reply *) OutHeadP;
	if (!msgh_simple)
		OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = msgh_size;
}

/* Routine bsd_signal_port_register */
mig_internal void _Xbsd_signal_port_register
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sigportType;
		mach_port_t sigport;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_signal_port_register
		(mach_port_t proc_port, mach_port_seqno_t seqno, mach_port_t sigport);

	static const mach_msg_type_t sigportCheck = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sigportType != * (int *) &sigportCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_signal_port_register(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->sigport);
}

/* Routine bsd_fork */
mig_internal void _Xbsd_fork
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t new_stateType;
		natural_t new_state[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t child_pidType;
		int child_pid;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_fork
		(mach_port_t proc_port, mach_port_seqno_t seqno, thread_state_t new_state, mach_msg_type_number_t new_stateCnt, int *child_pid);

	unsigned int msgh_size;

	static const mach_msg_type_t child_pidType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->new_stateType.msgt_inline != TRUE) ||
	    (In0P->new_stateType.msgt_longform != FALSE) ||
	    (In0P->new_stateType.msgt_name != 2) ||
	    (In0P->new_stateType.msgt_size != 32))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + (4 * In0P->new_stateType.msgt_number))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_fork(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->new_state, In0P->new_stateType.msgt_number, &OutP->child_pid);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->child_pidType = child_pidType;
}

/* Routine bsd_vfork */
mig_internal void _Xbsd_vfork
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t new_stateType;
		natural_t new_state[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t child_pidType;
		int child_pid;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_vfork
		(mach_port_t proc_port, mach_port_seqno_t seqno, thread_state_t new_state, mach_msg_type_number_t new_stateCnt, int *child_pid);

	unsigned int msgh_size;

	static const mach_msg_type_t child_pidType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->new_stateType.msgt_inline != TRUE) ||
	    (In0P->new_stateType.msgt_longform != FALSE) ||
	    (In0P->new_stateType.msgt_name != 2) ||
	    (In0P->new_stateType.msgt_size != 32))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + (4 * In0P->new_stateType.msgt_number))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_vfork(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->new_state, In0P->new_stateType.msgt_number, &OutP->child_pid);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->child_pidType = child_pidType;
}

/* Routine bsd_take_signal */
mig_internal void _Xbsd_take_signal
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t old_maskType;
		sigset_t old_mask;
		mach_msg_type_t old_onstackType;
		int old_onstack;
		mach_msg_type_t sigType;
		int sig;
		mach_msg_type_t codeType;
		integer_t code;
		mach_msg_type_t handlerType;
		vm_offset_t handler;
		mach_msg_type_t new_spType;
		vm_offset_t new_sp;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_take_signal
		(mach_port_t proc_port, mach_port_seqno_t seqno, sigset_t *old_mask, int *old_onstack, int *sig, integer_t *code, vm_offset_t *handler, vm_offset_t *new_sp);

	static const mach_msg_type_t old_maskType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t old_onstackType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t sigType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t codeType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t handlerType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t new_spType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 24) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_take_signal(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &OutP->old_mask, &OutP->old_onstack, &OutP->sig, &OutP->code, &OutP->handler, &OutP->new_sp);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 80;

	OutP->old_maskType = old_maskType;

	OutP->old_onstackType = old_onstackType;

	OutP->sigType = sigType;

	OutP->codeType = codeType;

	OutP->handlerType = handlerType;

	OutP->new_spType = new_spType;
}

/* Routine bsd_sigreturn */
mig_internal void _Xbsd_sigreturn
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t old_on_stackType;
		int old_on_stack;
		mach_msg_type_t old_sigmaskType;
		sigset_t old_sigmask;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_sigreturn
		(mach_port_t proc_port, mach_port_seqno_t seqno, int old_on_stack, sigset_t old_sigmask);

	static const mach_msg_type_t old_on_stackCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t old_sigmaskCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 40) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->old_on_stackType != * (int *) &old_on_stackCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->old_sigmaskType != * (int *) &old_sigmaskCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_sigreturn(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->old_on_stack, In0P->old_sigmask);
}

/* Routine bsd_getrusage */
mig_internal void _Xbsd_getrusage
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t whichType;
		int which;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t rusageType;
		rusage_t rusage;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_getrusage
		(mach_port_t proc_port, mach_port_seqno_t seqno, int which, rusage_t *rusage);

	static const mach_msg_type_t whichCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t rusageType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		18,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->whichType != * (int *) &whichCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_getrusage(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->which, &OutP->rusage);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 108;

	OutP->rusageType = rusageType;
}

/* Routine bsd_chdir */
mig_internal void _Xbsd_chdir
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_chdir
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_chdir(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_chroot */
mig_internal void _Xbsd_chroot
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_chroot
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_chroot(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_open */
mig_internal void _Xbsd_open
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t modeType;
		int mode;
		mach_msg_type_t crtmodeType;
		int crtmode;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t filenoType;
		int fileno;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_open
		(mach_port_t proc_port, mach_port_seqno_t seqno, int mode, int crtmode, path_name_t fname, mach_msg_type_number_t fnameCnt, int *fileno);

	unsigned int msgh_size;

	static const mach_msg_type_t modeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t crtmodeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t filenoType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 44) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->modeType != * (int *) &modeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->crtmodeType != * (int *) &crtmodeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 44 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_open(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->mode, In0P->crtmode, In0P->fname, In0P->fnameType.msgt_number, &OutP->fileno);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->filenoType = filenoType;
}

/* Routine bsd_mknod */
mig_internal void _Xbsd_mknod
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t fmodeType;
		int fmode;
		mach_msg_type_t devType;
		int dev;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_mknod
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fmode, int dev, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t fmodeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t devCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 44) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->fmodeType != * (int *) &fmodeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->devType != * (int *) &devCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 44 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_mknod(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fmode, In0P->dev, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_link */
mig_internal void _Xbsd_link
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t targetType;
		char target[1024];
		mach_msg_type_t linknameType;
		char linkname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_link
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t target, mach_msg_type_number_t targetCnt, path_name_t linkname, mach_msg_type_number_t linknameCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->targetType.msgt_inline != TRUE) ||
	    (In0P->targetType.msgt_longform != FALSE) ||
	    (In0P->targetType.msgt_name != 8) ||
	    (In0P->targetType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->targetType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 32 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 1024);

#if	TypeCheck
	if ((In1P->linknameType.msgt_inline != TRUE) ||
	    (In1P->linknameType.msgt_longform != FALSE) ||
	    (In1P->linknameType.msgt_name != 8) ||
	    (In1P->linknameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 32 + ((In1P->linknameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_link(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->target, In0P->targetType.msgt_number, In1P->linkname, In1P->linknameType.msgt_number);
}

/* Routine bsd_symlink */
mig_internal void _Xbsd_symlink
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t targetType;
		char target[1024];
		mach_msg_type_t linknameType;
		char linkname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_symlink
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t target, mach_msg_type_number_t targetCnt, path_name_t linkname, mach_msg_type_number_t linknameCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->targetType.msgt_inline != TRUE) ||
	    (In0P->targetType.msgt_longform != FALSE) ||
	    (In0P->targetType.msgt_name != 8) ||
	    (In0P->targetType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->targetType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 32 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 1024);

#if	TypeCheck
	if ((In1P->linknameType.msgt_inline != TRUE) ||
	    (In1P->linknameType.msgt_longform != FALSE) ||
	    (In1P->linknameType.msgt_name != 8) ||
	    (In1P->linknameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 32 + ((In1P->linknameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_symlink(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->target, In0P->targetType.msgt_number, In1P->linkname, In1P->linknameType.msgt_number);
}

/* Routine bsd_unlink */
mig_internal void _Xbsd_unlink
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_unlink
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_unlink(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_access */
mig_internal void _Xbsd_access
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t fmodeType;
		int fmode;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_access
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fmode, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t fmodeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->fmodeType != * (int *) &fmodeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_access(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fmode, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_readlink */
mig_internal void _Xbsd_readlink
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t countType;
		int count;
		mach_msg_type_t nameType;
		char name[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_long_t bufType;
		char buf[8192];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_readlink
		(mach_port_t proc_port, mach_port_seqno_t seqno, int count, path_name_t name, mach_msg_type_number_t nameCnt, small_char_array buf, mach_msg_type_number_t *bufCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t countCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t bufType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	8192,
	};

	mach_msg_type_number_t bufCnt;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->countType != * (int *) &countCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->nameType.msgt_inline != TRUE) ||
	    (In0P->nameType.msgt_longform != FALSE) ||
	    (In0P->nameType.msgt_name != 8) ||
	    (In0P->nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	bufCnt = 8192;

	OutP->RetCode = bsd_readlink(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->count, In0P->name, In0P->nameType.msgt_number, OutP->buf, &bufCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->bufType = bufType;

	OutP->bufType.msgtl_number = bufCnt;
	OutP->Head.msgh_size = 44 + ((bufCnt + 3) & ~3);
}

/* Routine bsd_utimes */
mig_internal void _Xbsd_utimes
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t timesType;
		timeval_2_t times;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_utimes
		(mach_port_t proc_port, mach_port_seqno_t seqno, timeval_2_t times, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t timesCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		4,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 48) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->timesType != * (int *) &timesCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 48 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_utimes(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->times, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_rename */
mig_internal void _Xbsd_rename
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t from_nameType;
		char from_name[1024];
		mach_msg_type_t to_nameType;
		char to_name[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_rename
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t from_name, mach_msg_type_number_t from_nameCnt, path_name_t to_name, mach_msg_type_number_t to_nameCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->from_nameType.msgt_inline != TRUE) ||
	    (In0P->from_nameType.msgt_longform != FALSE) ||
	    (In0P->from_nameType.msgt_name != 8) ||
	    (In0P->from_nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->from_nameType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 32 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 1024);

#if	TypeCheck
	if ((In1P->to_nameType.msgt_inline != TRUE) ||
	    (In1P->to_nameType.msgt_longform != FALSE) ||
	    (In1P->to_nameType.msgt_name != 8) ||
	    (In1P->to_nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 32 + ((In1P->to_nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_rename(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->from_name, In0P->from_nameType.msgt_number, In1P->to_name, In1P->to_nameType.msgt_number);
}

/* Routine bsd_mkdir */
mig_internal void _Xbsd_mkdir
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t dmodeType;
		int dmode;
		mach_msg_type_t nameType;
		char name[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_mkdir
		(mach_port_t proc_port, mach_port_seqno_t seqno, int dmode, path_name_t name, mach_msg_type_number_t nameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t dmodeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->dmodeType != * (int *) &dmodeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->nameType.msgt_inline != TRUE) ||
	    (In0P->nameType.msgt_longform != FALSE) ||
	    (In0P->nameType.msgt_name != 8) ||
	    (In0P->nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_mkdir(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->dmode, In0P->name, In0P->nameType.msgt_number);
}

/* Routine bsd_rmdir */
mig_internal void _Xbsd_rmdir
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t nameType;
		char name[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_rmdir
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t name, mach_msg_type_number_t nameCnt);

	unsigned int msgh_size;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 28) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->nameType.msgt_inline != TRUE) ||
	    (In0P->nameType.msgt_longform != FALSE) ||
	    (In0P->nameType.msgt_name != 8) ||
	    (In0P->nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 28 + ((In0P->nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_rmdir(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->name, In0P->nameType.msgt_number);
}

/* Routine bsd_acct */
mig_internal void _Xbsd_acct
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t acct_onType;
		boolean_t acct_on;
		mach_msg_type_t fnameType;
		char fname[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_acct
		(mach_port_t proc_port, mach_port_seqno_t seqno, boolean_t acct_on, path_name_t fname, mach_msg_type_number_t fnameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t acct_onCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->acct_onType != * (int *) &acct_onCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->fnameType.msgt_inline != TRUE) ||
	    (In0P->fnameType.msgt_longform != FALSE) ||
	    (In0P->fnameType.msgt_name != 8) ||
	    (In0P->fnameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->fnameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_acct(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->acct_on, In0P->fname, In0P->fnameType.msgt_number);
}

/* Routine bsd_write_short */
mig_internal void _Xbsd_write_short
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_long_t dataType;
		char data[8192];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t amount_writtenType;
		size_t amount_written;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_write_short
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, small_char_array data, mach_msg_type_number_t dataCnt, size_t *amount_written);

	unsigned int msgh_size;

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t amount_writtenType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 44) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->dataType.msgtl_header.msgt_inline != TRUE) ||
	    (In0P->dataType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->dataType.msgtl_name != 8) ||
	    (In0P->dataType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 44 + ((In0P->dataType.msgtl_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_write_short(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->data, In0P->dataType.msgtl_number, &OutP->amount_written);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->amount_writtenType = amount_writtenType;
}

/* Routine bsd_write_long */
mig_internal void _Xbsd_write_long
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_long_t dataType;
		char_array data;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t amount_writtenType;
		size_t amount_written;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_write_long
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, char_array data, mach_msg_type_number_t dataCnt, size_t *amount_written);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t amount_writtenType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 48) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->dataType.msgtl_header.msgt_inline != FALSE) ||
	    (In0P->dataType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->dataType.msgtl_name != 8) ||
	    (In0P->dataType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_write_long(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->data, In0P->dataType.msgtl_number, &OutP->amount_written);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->amount_writtenType = amount_writtenType;
}

/* Routine bsd_read_short */
mig_internal void _Xbsd_read_short
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t datalenType;
		int datalen;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t nreadType;
		size_t nread;
		mach_msg_type_long_t dataType;
		char data[8192];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_read_short
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int datalen, size_t *nread, small_char_array data, mach_msg_type_number_t *dataCnt);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t datalenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nreadType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t dataType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	8192,
	};

	mach_msg_type_number_t dataCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 40) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->datalenType != * (int *) &datalenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	dataCnt = 8192;

	OutP->RetCode = bsd_read_short(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->datalen, &OutP->nread, OutP->data, &dataCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->nreadType = nreadType;

	OutP->dataType = dataType;

	OutP->dataType.msgtl_number = dataCnt;
	OutP->Head.msgh_size = 52 + ((dataCnt + 3) & ~3);
}

/* Routine bsd_read_long */
mig_internal void _Xbsd_read_long
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t datalenType;
		int datalen;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t nreadType;
		size_t nread;
		mach_msg_type_long_t dataType;
		char_array data;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_read_long
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int datalen, size_t *nread, char_array *data, mach_msg_type_number_t *dataCnt);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t datalenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nreadType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t dataType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		FALSE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		TRUE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	0,
	};

	mach_msg_type_number_t dataCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 40) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->datalenType != * (int *) &datalenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_read_long(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->datalen, &OutP->nread, &OutP->data, &dataCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 56;

	OutP->nreadType = nreadType;

	OutP->dataType = dataType;

	OutP->dataType.msgtl_number = dataCnt;
}

/* Routine bsd_select */
mig_internal void _Xbsd_select
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t ndType;
		int nd;
		mach_msg_type_t in_setType;
		fd_set in_set;
		mach_msg_type_t ou_setType;
		fd_set ou_set;
		mach_msg_type_t ex_setType;
		fd_set ex_set;
		mach_msg_type_t in_validType;
		boolean_t in_valid;
		mach_msg_type_t ou_validType;
		boolean_t ou_valid;
		mach_msg_type_t ex_validType;
		boolean_t ex_valid;
		mach_msg_type_t do_timeoutType;
		boolean_t do_timeout;
		mach_msg_type_t tvType;
		timeval_t tv;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t in_setType;
		fd_set in_set;
		mach_msg_type_t ou_setType;
		fd_set ou_set;
		mach_msg_type_t ex_setType;
		fd_set ex_set;
		mach_msg_type_t rvalType;
		integer_t rval;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_select
		(mach_port_t proc_port, mach_port_seqno_t seqno, int nd, fd_set *in_set, fd_set *ou_set, fd_set *ex_set, boolean_t in_valid, boolean_t ou_valid, boolean_t ex_valid, boolean_t do_timeout, timeval_t tv, integer_t *rval);

	static const mach_msg_type_t ndCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t in_setCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ou_setCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ex_setCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t in_validCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ou_validCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ex_validCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t do_timeoutCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t tvCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t in_setType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ou_setType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ex_setType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		8,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t rvalType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 184) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->ndType != * (int *) &ndCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->in_setType != * (int *) &in_setCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->ou_setType != * (int *) &ou_setCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->ex_setType != * (int *) &ex_setCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->in_validType != * (int *) &in_validCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->ou_validType != * (int *) &ou_validCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->ex_validType != * (int *) &ex_validCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->do_timeoutType != * (int *) &do_timeoutCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->tvType != * (int *) &tvCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_select(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->nd, &In0P->in_set, &In0P->ou_set, &In0P->ex_set, In0P->in_valid, In0P->ou_valid, In0P->ex_valid, In0P->do_timeout, In0P->tv, &OutP->rval);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 148;

	OutP->in_setType = in_setType;

	OutP->in_set = In0P->in_set;

	OutP->ou_setType = ou_setType;

	OutP->ou_set = In0P->ou_set;

	OutP->ex_setType = ex_setType;

	OutP->ex_set = In0P->ex_set;

	OutP->rvalType = rvalType;
}

/* Routine bsd_task_by_pid */
mig_internal void _Xbsd_task_by_pid
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t pidType;
		int pid;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t taskType;
		task_t task;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_task_by_pid
		(mach_port_t proc_port, mach_port_seqno_t seqno, int pid, task_t *task, mach_msg_type_name_t *taskPoly);

	boolean_t msgh_simple;
	static const mach_msg_type_t pidCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t taskType = {
		/* msgt_name = */		-1,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_name_t taskPoly;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->pidType != * (int *) &pidCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_task_by_pid(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->pid, &OutP->task, &taskPoly);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	msgh_simple = TRUE;
	OutP->Head.msgh_size = 40;

	OutP->taskType = taskType;

	if (MACH_MSG_TYPE_PORT_ANY(taskPoly))
		msgh_simple = FALSE;

	OutP->taskType.msgt_name = taskPoly;

	if (!msgh_simple)
		OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
}

/* Routine bsd_setgroups */
mig_internal void _Xbsd_setgroups
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t gidsetsizeType;
		int gidsetsize;
		mach_msg_type_t gidsetType;
		gidset_t gidset;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_setgroups
		(mach_port_t proc_port, mach_port_seqno_t seqno, int gidsetsize, gidset_t gidset);

	static const mach_msg_type_t gidsetsizeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t gidsetCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		16,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 100) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->gidsetsizeType != * (int *) &gidsetsizeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->gidsetType != * (int *) &gidsetCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_setgroups(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->gidsetsize, In0P->gidset);
}

/* Routine bsd_setrlimit */
mig_internal void _Xbsd_setrlimit
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t whichType;
		int which;
		mach_msg_type_t limType;
		rlimit_t lim;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_setrlimit
		(mach_port_t proc_port, mach_port_seqno_t seqno, int which, rlimit_t lim);

	static const mach_msg_type_t whichCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t limCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		4,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 52) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->whichType != * (int *) &whichCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->limType != * (int *) &limCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_setrlimit(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->which, In0P->lim);
}

/* Routine bsd_sigstack */
mig_internal void _Xbsd_sigstack
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t have_nssType;
		boolean_t have_nss;
		mach_msg_type_t nssType;
		sigstack_t nss;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t ossType;
		sigstack_t oss;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_sigstack
		(mach_port_t proc_port, mach_port_seqno_t seqno, boolean_t have_nss, sigstack_t nss, sigstack_t *oss);

	static const mach_msg_type_t have_nssCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nssCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t ossType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 44) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->have_nssType != * (int *) &have_nssCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->nssType != * (int *) &nssCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_sigstack(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->have_nss, In0P->nss, &OutP->oss);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 44;

	OutP->ossType = ossType;
}

/* Routine bsd_settimeofday */
mig_internal void _Xbsd_settimeofday
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t have_tvType;
		boolean_t have_tv;
		mach_msg_type_t tvType;
		timeval_t tv;
		mach_msg_type_t have_tzType;
		boolean_t have_tz;
		mach_msg_type_t tzType;
		timezone_t tz;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_settimeofday
		(mach_port_t proc_port, mach_port_seqno_t seqno, boolean_t have_tv, timeval_t tv, boolean_t have_tz, timezone_t tz);

	static const mach_msg_type_t have_tvCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t tvCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t have_tzCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t tzCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 64) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->have_tvType != * (int *) &have_tvCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->tvType != * (int *) &tvCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->have_tzType != * (int *) &have_tzCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->tzType != * (int *) &tzCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_settimeofday(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->have_tv, In0P->tv, In0P->have_tz, In0P->tz);
}

/* Routine bsd_adjtime */
mig_internal void _Xbsd_adjtime
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t deltaType;
		timeval_t delta;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t olddeltaType;
		timeval_t olddelta;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_adjtime
		(mach_port_t proc_port, mach_port_seqno_t seqno, timeval_t delta, timeval_t *olddelta);

	static const mach_msg_type_t deltaCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t olddeltaType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		2,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->deltaType != * (int *) &deltaCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_adjtime(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->delta, &OutP->olddelta);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 44;

	OutP->olddeltaType = olddeltaType;
}

/* Routine bsd_setitimer */
mig_internal void _Xbsd_setitimer
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t whichType;
		int which;
		mach_msg_type_t have_itvType;
		boolean_t have_itv;
		mach_msg_type_t itvType;
		itimerval_t itv;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t oitvType;
		itimerval_t oitv;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_setitimer
		(mach_port_t proc_port, mach_port_seqno_t seqno, int which, boolean_t have_itv, itimerval_t *oitv, itimerval_t itv);

	static const mach_msg_type_t whichCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t have_itvCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t itvCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		4,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t oitvType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		4,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 60) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->whichType != * (int *) &whichCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->have_itvType != * (int *) &have_itvCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->itvType != * (int *) &itvCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_setitimer(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->which, In0P->have_itv, &OutP->oitv, In0P->itv);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 52;

	OutP->oitvType = oitvType;
}

/* Routine bsd_bind */
mig_internal void _Xbsd_bind
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sType;
		int s;
		mach_msg_type_t nameType;
		char name[128];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_bind
		(mach_port_t proc_port, mach_port_seqno_t seqno, int s, sockarg_t name, mach_msg_type_number_t nameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t sCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sType != * (int *) &sCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->nameType.msgt_inline != TRUE) ||
	    (In0P->nameType.msgt_longform != FALSE) ||
	    (In0P->nameType.msgt_name != 8) ||
	    (In0P->nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_bind(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->s, In0P->name, In0P->nameType.msgt_number);
}

/* Routine bsd_connect */
mig_internal void _Xbsd_connect
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sType;
		int s;
		mach_msg_type_t nameType;
		char name[128];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_connect
		(mach_port_t proc_port, mach_port_seqno_t seqno, int s, sockarg_t name, mach_msg_type_number_t nameCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t sCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sType != * (int *) &sCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->nameType.msgt_inline != TRUE) ||
	    (In0P->nameType.msgt_longform != FALSE) ||
	    (In0P->nameType.msgt_name != 8) ||
	    (In0P->nameType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->nameType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_connect(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->s, In0P->name, In0P->nameType.msgt_number);
}

/* Routine bsd_setsockopt */
mig_internal void _Xbsd_setsockopt
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sType;
		int s;
		mach_msg_type_t levelType;
		int level;
		mach_msg_type_t nameType;
		int name;
		mach_msg_type_t valType;
		char val[128];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_setsockopt
		(mach_port_t proc_port, mach_port_seqno_t seqno, int s, int level, int name, sockarg_t val, mach_msg_type_number_t valCnt);

	unsigned int msgh_size;

	static const mach_msg_type_t sCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t levelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nameCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 52) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sType != * (int *) &sCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->levelType != * (int *) &levelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->nameType != * (int *) &nameCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->valType.msgt_inline != TRUE) ||
	    (In0P->valType.msgt_longform != FALSE) ||
	    (In0P->valType.msgt_name != 8) ||
	    (In0P->valType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 52 + ((In0P->valType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_setsockopt(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->s, In0P->level, In0P->name, In0P->val, In0P->valType.msgt_number);
}

/* Routine bsd_getsockopt */
mig_internal void _Xbsd_getsockopt
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sType;
		int s;
		mach_msg_type_t levelType;
		int level;
		mach_msg_type_t nameType;
		int name;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t valType;
		char val[128];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_getsockopt
		(mach_port_t proc_port, mach_port_seqno_t seqno, int s, int level, int name, sockarg_t val, mach_msg_type_number_t *valCnt);

	static const mach_msg_type_t sCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t levelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nameCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t valType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		128,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_number_t valCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 48) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sType != * (int *) &sCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->levelType != * (int *) &levelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->nameType != * (int *) &nameCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	valCnt = 128;

	OutP->RetCode = bsd_getsockopt(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->s, In0P->level, In0P->name, OutP->val, &valCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->valType = valType;

	OutP->valType.msgt_number = valCnt;
	OutP->Head.msgh_size = 36 + ((valCnt + 3) & ~3);
}

/* Routine bsd_init_process */
mig_internal void _Xbsd_init_process
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_init_process
		(mach_port_t proc_port, mach_port_seqno_t seqno);

#if	TypeCheck
	if ((In0P->Head.msgh_size != 24) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_init_process(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno);
}

/* Routine bsd_table_set */
mig_internal void _Xbsd_table_set
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t idType;
		int id;
		mach_msg_type_t indexType;
		int index;
		mach_msg_type_t lelType;
		int lel;
		mach_msg_type_t nelType;
		int nel;
		mach_msg_type_long_t addrType;
		char addr[8192];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t nel_doneType;
		int nel_done;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_table_set
		(mach_port_t proc_port, mach_port_seqno_t seqno, int id, int index, int lel, int nel, small_char_array addr, mach_msg_type_number_t addrCnt, int *nel_done);

	unsigned int msgh_size;

	static const mach_msg_type_t idCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t indexCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t lelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nel_doneType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 68) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->idType != * (int *) &idCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->indexType != * (int *) &indexCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->lelType != * (int *) &lelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->nelType != * (int *) &nelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->addrType.msgtl_header.msgt_inline != TRUE) ||
	    (In0P->addrType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->addrType.msgtl_name != 8) ||
	    (In0P->addrType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 68 + ((In0P->addrType.msgtl_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_table_set(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->id, In0P->index, In0P->lel, In0P->nel, In0P->addr, In0P->addrType.msgtl_number, &OutP->nel_done);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->nel_doneType = nel_doneType;
}

/* Routine bsd_table_get */
mig_internal void _Xbsd_table_get
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t idType;
		int id;
		mach_msg_type_t indexType;
		int index;
		mach_msg_type_t lelType;
		int lel;
		mach_msg_type_t nelType;
		int nel;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_long_t addrType;
		char_array addr;
		mach_msg_type_t nel_doneType;
		int nel_done;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_table_get
		(mach_port_t proc_port, mach_port_seqno_t seqno, int id, int index, int lel, int nel, char_array *addr, mach_msg_type_number_t *addrCnt, int *nel_done);

	static const mach_msg_type_t idCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t indexCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t lelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nelCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t addrType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		FALSE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		TRUE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	0,
	};

	static const mach_msg_type_t nel_doneType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_number_t addrCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 56) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->idType != * (int *) &idCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->indexType != * (int *) &indexCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->lelType != * (int *) &lelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->nelType != * (int *) &nelCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_table_get(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->id, In0P->index, In0P->lel, In0P->nel, &OutP->addr, &addrCnt, &OutP->nel_done);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 56;

	OutP->addrType = addrType;

	OutP->addrType.msgtl_number = addrCnt;

	OutP->nel_doneType = nel_doneType;
}

/* Routine bsd_emulator_error */
mig_internal void _Xbsd_emulator_error
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_long_t err_messageType;
		char err_message[8192];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_emulator_error
		(mach_port_t proc_port, mach_port_seqno_t seqno, small_char_array err_message, mach_msg_type_number_t err_messageCnt);

	unsigned int msgh_size;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->err_messageType.msgtl_header.msgt_inline != TRUE) ||
	    (In0P->err_messageType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->err_messageType.msgtl_name != 8) ||
	    (In0P->err_messageType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->err_messageType.msgtl_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_emulator_error(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->err_message, In0P->err_messageType.msgtl_number);
}

/* Routine bsd_share_wakeup */
mig_internal void _Xbsd_share_wakeup
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t lock_offsetType;
		int lock_offset;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_share_wakeup
		(mach_port_t proc_port, mach_port_seqno_t seqno, int lock_offset);

	static const mach_msg_type_t lock_offsetCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->lock_offsetType != * (int *) &lock_offsetCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_share_wakeup(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->lock_offset);
}

/* Routine bsd_maprw_request_it */
mig_internal void _Xbsd_maprw_request_it
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_maprw_request_it
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_maprw_request_it(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno);
}

/* Routine bsd_maprw_release_it */
mig_internal void _Xbsd_maprw_release_it
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_maprw_release_it
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_maprw_release_it(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno);
}

/* Routine bsd_maprw_remap */
mig_internal void _Xbsd_maprw_remap
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t offsetType;
		int offset;
		mach_msg_type_t sizeType;
		int size;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_maprw_remap
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int offset, int size);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t offsetCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t sizeCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 48) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->offsetType != * (int *) &offsetCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sizeType != * (int *) &sizeCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_maprw_remap(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->offset, In0P->size);
}

/* Routine bsd_pid_by_task */
mig_internal void _Xbsd_pid_by_task
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t taskType;
		mach_port_t task;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t pidType;
		int pid;
		mach_msg_type_t commandType;
		char command[1024];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_pid_by_task
		(mach_port_t proc_port, mach_port_seqno_t seqno, mach_port_t task, int *pid, path_name_t command, mach_msg_type_number_t *commandCnt);

	static const mach_msg_type_t taskCheck = {
		/* msgt_name = */		17,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t pidType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t commandType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		1024,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_number_t commandCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->taskType != * (int *) &taskCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	commandCnt = 1024;

	OutP->RetCode = bsd_pid_by_task(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->task, &OutP->pid, OutP->command, &commandCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->pidType = pidType;

	OutP->commandType = commandType;

	OutP->commandType.msgt_number = commandCnt;
	OutP->Head.msgh_size = 44 + ((commandCnt + 3) & ~3);
}

/* Routine bsd_mon_switch */
mig_internal void _Xbsd_mon_switch
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t sample_flavorType;
		int sample_flavor;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t sample_flavorType;
		int sample_flavor;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_mon_switch
		(mach_port_t proc_port, mach_port_seqno_t seqno, int *sample_flavor);

	static const mach_msg_type_t sample_flavorCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t sample_flavorType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->sample_flavorType != * (int *) &sample_flavorCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_mon_switch(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &In0P->sample_flavor);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->sample_flavorType = sample_flavorType;

	OutP->sample_flavor = In0P->sample_flavor;
}

/* Routine bsd_mon_dump */
mig_internal void _Xbsd_mon_dump
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_long_t mon_dataType;
		char_array mon_data;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_mon_dump
		(mach_port_t proc_port, mach_port_seqno_t seqno, char_array *mon_data, mach_msg_type_number_t *mon_dataCnt);

	static const mach_msg_type_long_t mon_dataType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		FALSE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		TRUE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	0,
	};

	mach_msg_type_number_t mon_dataCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 24) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_mon_dump(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, &OutP->mon_data, &mon_dataCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 48;

	OutP->mon_dataType = mon_dataType;

	OutP->mon_dataType.msgtl_number = mon_dataCnt;
}

/* Routine bsd_getattr */
mig_internal void _Xbsd_getattr
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t vattrType;
		vattr_t vattr;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_getattr
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, vattr_t *vattr);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t vattrType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		24,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_getattr(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, &OutP->vattr);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 132;

	OutP->vattrType = vattrType;
}

/* Routine bsd_setattr */
mig_internal void _Xbsd_setattr
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t vattrType;
		vattr_t vattr;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_setattr
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, vattr_t vattr);

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t vattrCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		24,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	if ((In0P->Head.msgh_size != 132) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->vattrType != * (int *) &vattrCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_setattr(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->vattr);
}

/* Routine bsd_path_getattr */
mig_internal void _Xbsd_path_getattr
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t followType;
		boolean_t follow;
		mach_msg_type_t pathType;
		char path[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t vattrType;
		vattr_t vattr;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_path_getattr
		(mach_port_t proc_port, mach_port_seqno_t seqno, boolean_t follow, path_name_t path, mach_msg_type_number_t pathCnt, vattr_t *vattr);

	unsigned int msgh_size;

	static const mach_msg_type_t followCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t vattrType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		24,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 36) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->followType != * (int *) &followCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->pathType.msgt_inline != TRUE) ||
	    (In0P->pathType.msgt_longform != FALSE) ||
	    (In0P->pathType.msgt_name != 8) ||
	    (In0P->pathType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 36 + ((In0P->pathType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_path_getattr(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->follow, In0P->path, In0P->pathType.msgt_number, &OutP->vattr);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 132;

	OutP->vattrType = vattrType;
}

/* Routine bsd_path_setattr */
mig_internal void _Xbsd_path_setattr
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t followType;
		boolean_t follow;
		mach_msg_type_t pathType;
		char path[1024];
		mach_msg_type_t vattrType;
		vattr_t vattr;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_path_setattr
		(mach_port_t proc_port, mach_port_seqno_t seqno, boolean_t follow, path_name_t path, mach_msg_type_number_t pathCnt, vattr_t vattr);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t followCheck = {
		/* msgt_name = */		0,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t vattrCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		24,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 136) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->followType != * (int *) &followCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->pathType.msgt_inline != TRUE) ||
	    (In0P->pathType.msgt_longform != FALSE) ||
	    (In0P->pathType.msgt_name != 8) ||
	    (In0P->pathType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->pathType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size != 136 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 1024);

#if	TypeCheck
	if (* (int *) &In1P->vattrType != * (int *) &vattrCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_path_setattr(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->follow, In0P->path, In0P->pathType.msgt_number, In1P->vattr);
}

/* Routine bsd_sysctl */
mig_internal void _Xbsd_sysctl
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t mibType;
		int mib[12];
		mach_msg_type_t miblenType;
		int miblen;
		mach_msg_type_t oldlenType;
		size_t oldlen;
		mach_msg_type_long_t newType;
		char_array new;
		mach_msg_type_t newlenType;
		size_t newlen;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_long_t oldType;
		char_array old;
		mach_msg_type_t oldlenType;
		size_t oldlen;
		mach_msg_type_t retlenType;
		int retlen;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_sysctl
		(mach_port_t proc_port, mach_port_seqno_t seqno, mib_t mib, mach_msg_type_number_t mibCnt, int miblen, char_array *old, mach_msg_type_number_t *oldCnt, size_t *oldlen, char_array new, mach_msg_type_number_t newCnt, size_t newlen, int *retlen);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t miblenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t oldlenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t newlenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t oldType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		FALSE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		TRUE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	0,
	};

	static const mach_msg_type_t oldlenType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t retlenType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	mach_msg_type_number_t oldCnt;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 68) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->mibType.msgt_inline != TRUE) ||
	    (In0P->mibType.msgt_longform != FALSE) ||
	    (In0P->mibType.msgt_name != 2) ||
	    (In0P->mibType.msgt_size != 32))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = 4 * In0P->mibType.msgt_number;
#if	TypeCheck
	if (msgh_size != 68 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 48);

#if	TypeCheck
	if (* (int *) &In1P->miblenType != * (int *) &miblenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In1P->oldlenType != * (int *) &oldlenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In1P->newType.msgtl_header.msgt_inline != FALSE) ||
	    (In1P->newType.msgtl_header.msgt_longform != TRUE) ||
	    (In1P->newType.msgtl_name != 8) ||
	    (In1P->newType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In1P->newlenType != * (int *) &newlenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_sysctl(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->mib, In0P->mibType.msgt_number, In1P->miblen, &OutP->old, &oldCnt, &In1P->oldlen, In1P->new, In1P->newType.msgtl_number, In1P->newlen, &OutP->retlen);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	OutP->Head.msgh_size = 64;

	OutP->oldType = oldType;

	OutP->oldType.msgtl_number = oldCnt;

	OutP->oldlenType = oldlenType;

	OutP->oldlen = In1P->oldlen;

	OutP->retlenType = retlenType;
}

/* Routine bsd_set_atexpansion */
mig_internal void _Xbsd_set_atexpansion
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t what_to_expandType;
		char what_to_expand[1024];
		mach_msg_type_t expansionType;
		char expansion[1024];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_set_atexpansion
		(mach_port_t proc_port, mach_port_seqno_t seqno, path_name_t what_to_expand, mach_msg_type_number_t what_to_expandCnt, path_name_t expansion, mach_msg_type_number_t expansionCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 32) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->what_to_expandType.msgt_inline != TRUE) ||
	    (In0P->what_to_expandType.msgt_longform != FALSE) ||
	    (In0P->what_to_expandType.msgt_name != 8) ||
	    (In0P->what_to_expandType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->what_to_expandType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 32 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 1024);

#if	TypeCheck
	if ((In1P->expansionType.msgt_inline != TRUE) ||
	    (In1P->expansionType.msgt_longform != FALSE) ||
	    (In1P->expansionType.msgt_name != 8) ||
	    (In1P->expansionType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 32 + ((In1P->expansionType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_set_atexpansion(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->what_to_expand, In0P->what_to_expandType.msgt_number, In1P->expansion, In1P->expansionType.msgt_number);
}

/* Routine bsd_sendmsg_short */
mig_internal void _Xbsd_sendmsg_short
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t flagsType;
		int flags;
		mach_msg_type_long_t dataType;
		char data[8192];
		mach_msg_type_t toType;
		char to[128];
		mach_msg_type_t cmsgType;
		char cmsg[128];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t nsentType;
		size_t nsent;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Request *In2P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_sendmsg_short
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int flags, small_char_array data, mach_msg_type_number_t dataCnt, sockarg_t to, mach_msg_type_number_t toCnt, sockarg_t cmsg, mach_msg_type_number_t cmsgCnt, size_t *nsent);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t flagsCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nsentType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 60) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->flagsType != * (int *) &flagsCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->dataType.msgtl_header.msgt_inline != TRUE) ||
	    (In0P->dataType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->dataType.msgtl_name != 8) ||
	    (In0P->dataType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->dataType.msgtl_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 60 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 8192);

#if	TypeCheck
	if ((In1P->toType.msgt_inline != TRUE) ||
	    (In1P->toType.msgt_longform != FALSE) ||
	    (In1P->toType.msgt_name != 8) ||
	    (In1P->toType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In1P->toType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 60 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In2P = (Request *) ((char *) In1P + msgh_size_delta - 128);

#if	TypeCheck
	if ((In2P->cmsgType.msgt_inline != TRUE) ||
	    (In2P->cmsgType.msgt_longform != FALSE) ||
	    (In2P->cmsgType.msgt_name != 8) ||
	    (In2P->cmsgType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 60 + ((In2P->cmsgType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_sendmsg_short(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->flags, In0P->data, In0P->dataType.msgtl_number, In1P->to, In1P->toType.msgt_number, In2P->cmsg, In2P->cmsgType.msgt_number, &OutP->nsent);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->nsentType = nsentType;
}

/* Routine bsd_sendmsg_long */
mig_internal void _Xbsd_sendmsg_long
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t flagsType;
		int flags;
		mach_msg_type_long_t dataType;
		char_array data;
		mach_msg_type_t toType;
		char to[128];
		mach_msg_type_t cmsgType;
		char cmsg[128];
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t nsentType;
		size_t nsent;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Request *In1P;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_sendmsg_long
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int flags, char_array data, mach_msg_type_number_t dataCnt, sockarg_t to, mach_msg_type_number_t toCnt, sockarg_t cmsg, mach_msg_type_number_t cmsgCnt, size_t *nsent);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t flagsCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nsentType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

#if	TypeCheck
	msgh_size = In0P->Head.msgh_size;
	if ((msgh_size < 64) ||
	    !(In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->flagsType != * (int *) &flagsCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->dataType.msgtl_header.msgt_inline != FALSE) ||
	    (In0P->dataType.msgtl_header.msgt_longform != TRUE) ||
	    (In0P->dataType.msgtl_name != 8) ||
	    (In0P->dataType.msgtl_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if ((In0P->toType.msgt_inline != TRUE) ||
	    (In0P->toType.msgt_longform != FALSE) ||
	    (In0P->toType.msgt_name != 8) ||
	    (In0P->toType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	msgh_size_delta = (In0P->toType.msgt_number + 3) & ~3;
#if	TypeCheck
	if (msgh_size < 64 + msgh_size_delta)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
	msgh_size -= msgh_size_delta;
#endif	/* TypeCheck */

	In1P = (Request *) ((char *) In0P + msgh_size_delta - 128);

#if	TypeCheck
	if ((In1P->cmsgType.msgt_inline != TRUE) ||
	    (In1P->cmsgType.msgt_longform != FALSE) ||
	    (In1P->cmsgType.msgt_name != 8) ||
	    (In1P->cmsgType.msgt_size != 8))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (msgh_size != 64 + ((In1P->cmsgType.msgt_number + 3) & ~3))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_sendmsg_long(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->flags, In0P->data, In0P->dataType.msgtl_number, In0P->to, In0P->toType.msgt_number, In1P->cmsg, In1P->cmsgType.msgt_number, &OutP->nsent);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_size = 40;

	OutP->nsentType = nsentType;
}

/* Routine bsd_recvmsg_short */
mig_internal void _Xbsd_recvmsg_short
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t flagsType;
		int flags;
		mach_msg_type_t fromlenType;
		int fromlen;
		mach_msg_type_t cmsglenType;
		int cmsglen;
		mach_msg_type_t datalenType;
		int datalen;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t outflagsType;
		int outflags;
		mach_msg_type_t nreceivedType;
		size_t nreceived;
		mach_msg_type_t fromType;
		char from[128];
		mach_msg_type_t cmsgType;
		char cmsg[128];
		mach_msg_type_long_t dataType;
		char data[8192];
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_recvmsg_short
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int flags, int *outflags, int fromlen, size_t *nreceived, sockarg_t from, mach_msg_type_number_t *fromCnt, int cmsglen, sockarg_t cmsg, mach_msg_type_number_t *cmsgCnt, int datalen, small_char_array data, mach_msg_type_number_t *dataCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t flagsCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t fromlenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cmsglenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t datalenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t outflagsType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nreceivedType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t fromType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		128,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cmsgType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		128,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t dataType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	8192,
	};

	mach_msg_type_number_t fromCnt;
	char cmsg[128];
	mach_msg_type_number_t cmsgCnt;
	char data[8192];
	mach_msg_type_number_t dataCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 64) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->flagsType != * (int *) &flagsCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->fromlenType != * (int *) &fromlenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->cmsglenType != * (int *) &cmsglenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->datalenType != * (int *) &datalenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	fromCnt = 128;

	cmsgCnt = 128;

	dataCnt = 8192;

	OutP->RetCode = bsd_recvmsg_short(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->flags, &OutP->outflags, In0P->fromlen, &OutP->nreceived, OutP->from, &fromCnt, In0P->cmsglen, cmsg, &cmsgCnt, In0P->datalen, data, &dataCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->outflagsType = outflagsType;

	OutP->nreceivedType = nreceivedType;

	OutP->fromType = fromType;

	OutP->fromType.msgt_number = fromCnt;
	msgh_size_delta = (fromCnt + 3) & ~3;
	msgh_size = 68 + msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 128);

	OutP->cmsgType = cmsgType;

	memcpy(OutP->cmsg, cmsg, cmsgCnt);

	OutP->cmsgType.msgt_number = cmsgCnt;
	msgh_size_delta = (cmsgCnt + 3) & ~3;
	msgh_size += msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 128);

	OutP->dataType = dataType;

	memcpy(OutP->data, data, dataCnt);

	OutP->dataType.msgtl_number = dataCnt;
	msgh_size += (dataCnt + 3) & ~3;

	OutP = (Reply *) OutHeadP;
	OutP->Head.msgh_size = msgh_size;
}

/* Routine bsd_recvmsg_long */
mig_internal void _Xbsd_recvmsg_long
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t filenoType;
		int fileno;
		mach_msg_type_t flagsType;
		int flags;
		mach_msg_type_t fromlenType;
		int fromlen;
		mach_msg_type_t cmsglenType;
		int cmsglen;
		mach_msg_type_t datalenType;
		int datalen;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
		mach_msg_type_t outflagsType;
		int outflags;
		mach_msg_type_t nreceivedType;
		size_t nreceived;
		mach_msg_type_t fromType;
		char from[128];
		mach_msg_type_t cmsgType;
		char cmsg[128];
		mach_msg_type_long_t dataType;
		char_array data;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_recvmsg_long
		(mach_port_t proc_port, mach_port_seqno_t seqno, int fileno, int flags, int *outflags, size_t *nreceived, int fromlen, sockarg_t from, mach_msg_type_number_t *fromCnt, int cmsglen, sockarg_t cmsg, mach_msg_type_number_t *cmsgCnt, int datalen, char_array *data, mach_msg_type_number_t *dataCnt);

	unsigned int msgh_size;
	unsigned int msgh_size_delta;

	static const mach_msg_type_t filenoCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t flagsCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t fromlenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cmsglenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t datalenCheck = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t outflagsType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t nreceivedType = {
		/* msgt_name = */		2,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t fromType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		128,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_t cmsgType = {
		/* msgt_name = */		8,
		/* msgt_size = */		8,
		/* msgt_number = */		128,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	static const mach_msg_type_long_t dataType = {
	{
		/* msgt_name = */		0,
		/* msgt_size = */		0,
		/* msgt_number = */		0,
		/* msgt_inline = */		FALSE,
		/* msgt_longform = */		TRUE,
		/* msgt_deallocate = */		TRUE,
		/* msgt_unused = */		0
	},
		/* msgtl_name = */	8,
		/* msgtl_size = */	8,
		/* msgtl_number = */	0,
	};

	mach_msg_type_number_t fromCnt;
	char cmsg[128];
	mach_msg_type_number_t cmsgCnt;
	char_array data;
	mach_msg_type_number_t dataCnt;

#if	TypeCheck
	if ((In0P->Head.msgh_size != 64) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->filenoType != * (int *) &filenoCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->flagsType != * (int *) &flagsCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->fromlenType != * (int *) &fromlenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->cmsglenType != * (int *) &cmsglenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

#if	TypeCheck
	if (* (int *) &In0P->datalenType != * (int *) &datalenCheck)
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	fromCnt = 128;

	cmsgCnt = 128;

	OutP->RetCode = bsd_recvmsg_long(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno, In0P->fileno, In0P->flags, &OutP->outflags, &OutP->nreceived, In0P->fromlen, OutP->from, &fromCnt, In0P->cmsglen, cmsg, &cmsgCnt, In0P->datalen, &data, &dataCnt);
	if (OutP->RetCode != KERN_SUCCESS)
		return;

	OutP->Head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;

	OutP->outflagsType = outflagsType;

	OutP->nreceivedType = nreceivedType;

	OutP->fromType = fromType;

	OutP->fromType.msgt_number = fromCnt;
	msgh_size_delta = (fromCnt + 3) & ~3;
	msgh_size = 72 + msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 128);

	OutP->cmsgType = cmsgType;

	memcpy(OutP->cmsg, cmsg, cmsgCnt);

	OutP->cmsgType.msgt_number = cmsgCnt;
	msgh_size_delta = (cmsgCnt + 3) & ~3;
	msgh_size += msgh_size_delta;
	OutP = (Reply *) ((char *) OutP + msgh_size_delta - 128);

	OutP->dataType = dataType;

	OutP->data = data;

	OutP->dataType.msgtl_number = dataCnt;

	OutP = (Reply *) OutHeadP;
	OutP->Head.msgh_size = msgh_size;
}

/* Routine bsd_exec_done */
mig_internal void _Xbsd_exec_done
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	typedef struct {
		mach_msg_header_t Head;
	} Request;

	typedef struct {
		mach_msg_header_t Head;
		mach_msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	register Request *In0P = (Request *) InHeadP;
	register Reply *OutP = (Reply *) OutHeadP;
	mig_external kern_return_t bsd_exec_done
		(mach_port_t proc_port, mach_port_seqno_t seqno);

#if	TypeCheck
	if ((In0P->Head.msgh_size != 24) ||
	    (In0P->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		{ OutP->RetCode = MIG_BAD_ARGUMENTS; return; }
#endif	/* TypeCheck */

	OutP->RetCode = bsd_exec_done(In0P->Head.msgh_request_port, In0P->Head.msgh_seqno);
}

static mig_routine_t bsd_1_server_routines[] = {
		_Xbsd_execve,
		_Xbsd_after_exec,
		_Xbsd_vm_map,
		_Xbsd_fd_to_file_port,
		_Xbsd_file_port_open,
		_Xbsd_zone_info,
		_Xbsd_signal_port_register,
		_Xbsd_fork,
		_Xbsd_vfork,
		_Xbsd_take_signal,
		_Xbsd_sigreturn,
		_Xbsd_getrusage,
		_Xbsd_chdir,
		_Xbsd_chroot,
		_Xbsd_open,
		_Xbsd_mknod,
		_Xbsd_link,
		_Xbsd_symlink,
		_Xbsd_unlink,
		_Xbsd_access,
		_Xbsd_readlink,
		_Xbsd_utimes,
		_Xbsd_rename,
		_Xbsd_mkdir,
		_Xbsd_rmdir,
		_Xbsd_acct,
		_Xbsd_write_short,
		_Xbsd_write_long,
		_Xbsd_read_short,
		_Xbsd_read_long,
		0,
		0,
		_Xbsd_select,
		_Xbsd_task_by_pid,
		0,
		0,
		_Xbsd_setgroups,
		_Xbsd_setrlimit,
		0,
		_Xbsd_sigstack,
		_Xbsd_settimeofday,
		_Xbsd_adjtime,
		_Xbsd_setitimer,
		0,
		_Xbsd_bind,
		0,
		_Xbsd_connect,
		_Xbsd_setsockopt,
		_Xbsd_getsockopt,
		0,
		0,
		_Xbsd_init_process,
		_Xbsd_table_set,
		_Xbsd_table_get,
		_Xbsd_emulator_error,
		0,
		_Xbsd_share_wakeup,
		_Xbsd_maprw_request_it,
		_Xbsd_maprw_release_it,
		_Xbsd_maprw_remap,
		_Xbsd_pid_by_task,
		_Xbsd_mon_switch,
		_Xbsd_mon_dump,
		_Xbsd_getattr,
		_Xbsd_setattr,
		_Xbsd_path_getattr,
		_Xbsd_path_setattr,
		_Xbsd_sysctl,
		_Xbsd_set_atexpansion,
		_Xbsd_sendmsg_short,
		_Xbsd_sendmsg_long,
		_Xbsd_recvmsg_short,
		_Xbsd_recvmsg_long,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		_Xbsd_exec_done,
};

mig_external boolean_t bsd_1_server
	(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
	register mach_msg_header_t *InP =  InHeadP;
	register mig_reply_header_t *OutP = (mig_reply_header_t *) OutHeadP;

	static const mach_msg_type_t RetCodeType = {
		/* msgt_name = */		MACH_MSG_TYPE_INTEGER_32,
		/* msgt_size = */		32,
		/* msgt_number = */		1,
		/* msgt_inline = */		TRUE,
		/* msgt_longform = */		FALSE,
		/* msgt_deallocate = */		FALSE,
		/* msgt_unused = */		0
	};

	register mig_routine_t routine;

	OutP->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REPLY(InP->msgh_bits), 0);
	OutP->Head.msgh_size = sizeof *OutP;
	OutP->Head.msgh_remote_port = InP->msgh_reply_port;
	OutP->Head.msgh_local_port = MACH_PORT_NULL;
	OutP->Head.msgh_seqno = 0;
	OutP->Head.msgh_id = InP->msgh_id + 100;

	OutP->RetCodeType = RetCodeType;

	if ((InP->msgh_id > 101099) || (InP->msgh_id < 101000) ||
	    ((routine = bsd_1_server_routines[InP->msgh_id - 101000]) == 0)) {
		OutP->RetCode = MIG_BAD_ID;
		return FALSE;
	}
	(*routine) (InP, &OutP->Head);
	return TRUE;
}

mig_external mig_routine_t bsd_1_server_routine
	(const mach_msg_header_t *InHeadP)
{
	register int msgh_id;

	msgh_id = InHeadP->msgh_id - 101000;

	if ((msgh_id > 99) || (msgh_id < 0))
		return 0;

	return bsd_1_server_routines[msgh_id];
}

