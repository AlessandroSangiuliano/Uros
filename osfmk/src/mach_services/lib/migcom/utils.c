/*
 * Copyright (c) 1995, 1994, 1993, 1992, 1991, 1990  
 * Open Software Foundation, Inc. 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation, and that the name of ("OSF") or Open Software 
 * Foundation not be used in advertising or publicity pertaining to 
 * distribution of the software without specific, written prior permission. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL OSF BE LIABLE FOR ANY 
 * SPECIAL, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES 
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN 
 * ACTION OF CONTRACT, NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING 
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE 
 */
/*
 * OSF Research Institute MK6.1 (unencumbered) 1/31/1995
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 * 92/03/03  16:25:39  jeffreyh
 * 	Changes from TRUNK
 * 	[92/02/26  12:33:02  jeffreyh]
 * 
 * 92/01/14  16:47:08  rpd
 * 	Modified WriteTypeDeclIn and WriteTypeDeclOut to disable
 * 	the deallocate flag on Indefinite arguments.
 * 	[92/01/09            rpd]
 * 
 * 92/01/03  20:30:51  dbg
 * 	Change argByReferenceUser and argByReferenceServer to fields in
 * 	argument_t.
 * 	[91/08/29            dbg]
 * 
 * 91/07/31  18:11:45  dbg
 * 	Accept new dealloc_t argument type in WriteStaticDecl,
 * 	WritePackMsgType.
 * 
 * 	Don't need to zero last character of C string.  Mig_strncpy does
 * 	the proper work.
 * 
 * 	Add SkipVFPrintf, so that WriteCopyType doesn't print fields in
 * 	comments.
 * 	[91/07/17            dbg]
 * 
 * 91/06/25  10:32:36  rpd
 * 	Changed WriteVarDecl to WriteUserVarDecl.
 * 	Added WriteServerVarDecl.
 * 	[91/05/23            rpd]
 * 
 * 91/02/05  17:56:28  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:56:39  mrt]
 * 
 * 90/06/02  15:06:11  rpd
 * 	Created for new IPC.
 * 	[90/03/26  21:14:54  rpd]
 * 
 * 07-Apr-89  Richard Draves (rpd) at Carnegie-Mellon University
 *	Extensive revamping.  Added polymorphic arguments.
 *	Allow multiple variable-sized inline arguments in messages.
 *
 * 21-Aug-87  Mary Thompson (mrt) at Carnegie-Mellon University
 *	Added deallocflag to the WritePackMsg routines.
 *
 * 29-Jul-87  Mary Thompson (mrt) at Carnegie-Mellon University
 *	Changed WriteVarDecl to not automatically write
 *	semi-colons between items, so that it can be
 *	used to write C++ argument lists.
 *
 * 27-May-87  Richard Draves (rpd) at Carnegie-Mellon University
 *	Created.
 */

#include "type.h"
#include <mach/message.h>
#include <stdarg.h>
#include <stdlib.h>
#include "routine.h"
#include "write.h"
#include "global.h"
#include "utils.h"
#include "safestr.h" /* SafeSnprintf */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <execinfo.h>

/* Forward declaration for SafeString to avoid implicit-declaration warnings */
void SafeString(FILE *file, const char *s);

extern char *MessFreeRoutine;

void
WriteIdentificationString(FILE *file)
{
    extern char * GenerationDate;
    extern char * MigGenerationDate;
    extern char * MigMoreData;
    extern IsKernelUser, IsKernelServer, UseMsgRPC;

    fprintf(file, "/*\n");
    fprintf(file, " * IDENTIFICATION:\n");
    fprintf(file, " * stub generated %s", GenerationDate);
    fprintf(file, " * with a MiG generated %s by %s\n", MigGenerationDate, MigMoreData);
    fprintf(file, " * OPTIONS: \n");
    if (IsKernelUser)
	fprintf(file, " *\tKernelUser\n");
    if (IsKernelServer)
	fprintf(file, " *\tKernelServer\n");
    if (!UseMsgRPC)
	fprintf(file, " *\t-R (no RPC calls)\n");
    fprintf(file, " */\n");
}

void
WriteImport(FILE *file, string_t filename)
{
    fprintf(file, "#include %s\n", filename);
}

void
WriteRCSDecl(FILE *file, identifier_t name, string_t rcs)
{
    fprintf(file, "#ifndef\tlint\n");
    fprintf(file, "#if\tUseExternRCSId\n");
    fprintf(file, "%s char %s_rcsid[] = %s;\n", (BeAnsiC) ? "const" : "", name, rcs);
    fprintf(file, "#else\t/* UseExternRCSId */\n");
    fprintf(file, "static %s char rcsid[] = %s;\n", (BeAnsiC) ? "const" : "", rcs);
    fprintf(file, "#endif\t/* UseExternRCSId */\n");
    fprintf(file, "#endif\t/* lint */\n");
    fprintf(file, "\n");
}

void
WriteBogusDefines(FILE *file)
{
    fprintf(file, "#ifndef\tmig_internal\n");
    fprintf(file, "#define\tmig_internal\tstatic\n");
    fprintf(file, "#endif\t/* mig_internal */\n");
    fprintf(file, "\n");

    fprintf(file, "#ifndef\tmig_external\n");
    fprintf(file, "#define mig_external\n");
    fprintf(file, "#endif\t/* mig_external */\n");
    fprintf(file, "\n");

    fprintf(file, "#ifndef\tTypeCheck\n");
    fprintf(file, "#define\tTypeCheck 1\n");
    fprintf(file, "#endif\t/* TypeCheck */\n");
    fprintf(file, "\n");

    fprintf(file, "#ifndef\tmin\n");
    fprintf(file, "#define\tmin(a,b)  ( ((a) < (b))? (a): (b) )\n");
    fprintf(file, "#endif\t/* min */\n");
    fprintf(file, "\n");

    fprintf(file, "#ifndef\tUseStaticTemplates\n");
    if (BeAnsiC) {
        fprintf(file, "#define\tUseStaticTemplates\t1\n");
    } else {
        fprintf(file, "#if\t%s\n", NewCDecl);
        fprintf(file, "#define\tUseStaticTemplates\t1\n");
        fprintf(file, "#endif\t/* %s */\n", NewCDecl);
    }    
    fprintf(file, "#endif\t/* UseStaticTemplates */\n");
    fprintf(file, "\n");
}

void
WriteList(FILE *file, argument_t *args,
          void (*func)(FILE *file, argument_t *arg),
          u_int mask, char *between, char *after)
{
    register argument_t *arg;
    register boolean_t sawone = FALSE;

    for (arg = args; arg != argNULL; arg = arg->argNext)
	if (akCheckAll(arg->argKind, mask))
	{
	    if (sawone)
            SafeString(file, between);
	}

    if (sawone)
	SafeString(file, after);
}

static boolean_t
WriteReverseListPrim(FILE *file, register argument_t *arg,
                     void (*func)(FILE *file, argument_t *arg),
                     u_int mask, char *between)
{
    boolean_t sawone = FALSE;

    if (arg != argNULL)
    {
	sawone = WriteReverseListPrim(file, arg->argNext, func, mask, between);

	if (akCheckAll(arg->argKind, mask))
	{
	    if (sawone)
		SafeString(file, between);
	    sawone = TRUE;

	    (*func)(file, arg);
	}
    }

    return sawone;
}

void
WriteReverseList(FILE *file, argument_t *args,
                 void (*func)(FILE *file, argument_t *arg),
                 u_int mask, char *between, char *after)
{
    boolean_t sawone;

    sawone = WriteReverseListPrim(file, args, func, mask, between);

    if (sawone)
	SafeString(file, after);
}

void
WriteNameDecl(FILE *file, argument_t *arg)
{
    SafeString(file, arg->argVarName);
}

void
WriteUserVarDecl(FILE *file, argument_t *arg)
{
    char *ref = arg->argByReferenceUser ? "*" : "";

    fprintf(file, "\t%s %s", arg->argType->itUserType, ref);
	SafeString(file, arg->argVarName);
}

void
WriteServerVarDecl(FILE *file, argument_t *arg)
{
    char *ref = arg->argByReferenceServer ? "*" : "";
  
    fprintf(file, "\t%s %s", arg->argType->itTransType, ref);
	SafeString(file, arg->argVarName);
}

char *
ReturnTypeStr(routine_t *rt)
{
    return rt->rtRetCode->argType->itUserType;
} 

char *
FetchUserType(ipc_type_t *it)
{
    return it->itUserType;
} 

char *
FetchUserKPDType(ipc_type_t *it)
{
    return it->itUserKPDType;
} 

char *
FetchServerType(ipc_type_t *it)
{
    return it->itServerType;
} 

char *
FetchServerKPDType(ipc_type_t *it)
{
    /* do we really need to differentiate User and Server ?? */
    return it->itServerKPDType;
} 

void
WriteTrailerDecl(FILE *file, boolean_t trailer)
{
    if (trailer)
	fprintf(file, "\t\tmach_msg_format_0_trailer_t trailer;\n");
    else
	fprintf(file, "\t\tmach_msg_trailer_t trailer;\n");
}

void
SafeString(FILE *file, const char *s)
{
    if (s == NULL) {
        fputs("(null)", file);
        return;
    }
    const char *p = s;
    for (; *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (c < 32 || c > 126) {
            fputs("/*<non-printable>*/", file);
            fprintf(stderr, "SafeString: non-printable character detected in string: 0x%02x at position %ld\n", c, (long)(p - s));
            return;
        }
    }
    fputs(s, file);
}

void
WriteFieldDeclPrim(FILE *file, argument_t *arg,
                   char *(*tfunc)(ipc_type_t *it))
{
    register ipc_type_t *it = arg->argType;

    if (IS_VARIABLE_SIZED_UNTYPED(it) || it->itNoOptArray) {
	register argument_t *count = arg->argCount;
	register ipc_type_t *btype = it->itElement;

	/*
	 *	Build our own declaration for a varying array:
	 *	use the element type and maximum size specified.
	 *	Note arg->argCount->argMultiplier == btype->itNumber.
	 */
	/*
	 * NDR encoded VarStrings requires the offset field.
	 * Since it is not used, it wasn't worthwhile to create an extra 
	 * parameter
	 */
	if (it->itString)
	    fprintf(file, "\t\t%s %sOffset; /* MiG doesn't use it */\n", 
		(*tfunc)(count->argType), arg->argName);

	if (!(arg->argFlags & flSameCount) && !it->itNoOptArray)
	        /* in these cases we would have a count, which we don't want */
		fprintf(file, "\t\t%s %s;\n", (*tfunc)(count->argType), 
		    count->argMsgField);
	fprintf(file, "\t\t%s %s[%d];",
			(*tfunc)(btype),
			arg->argMsgField,
			it->itNumber/btype->itNumber);
    }
    else if (IS_MULTIPLE_KPD(it))  
	fprintf(file, "\t\t%s %s[%d];", (*tfunc)(it), arg->argMsgField,
			it->itKPD_Number);
    else  {
	/* either simple KPD or simple in-line */
	fprintf(file, "\t\t%s %s;", (*tfunc)(it), arg->argMsgField);
    }

    /* Kernel Processed Data has always PadSize = 0 */
    if (it->itPadSize != 0)
	fprintf(file, "\n\t\tchar %s[%d];", arg->argPadName, it->itPadSize);
}


void
WriteStructDecl(FILE *file, argument_t *args,
                void (*func)(FILE *file, argument_t *arg),
                u_int mask, char *name,
                boolean_t simple, boolean_t trailer,
                boolean_t isuser, boolean_t template_only)
{
    fprintf(file, "\ttypedef struct {\n");
    fprintf(file, "\t\tmach_msg_header_t Head;\n");
    if (simple == FALSE) {
	fprintf(file, "\t\t/* start of the kernel processed data */\n");
	fprintf(file, "\t\tmach_msg_body_t msgh_body;\n");
	if (mask == akbRequest) 
    	    WriteList(file, args, func, mask | akbSendKPD, "\n", "\n");
	else 
    	    WriteList(file, args, func, mask | akbReturnKPD, "\n", "\n");
	fprintf(file, "\t\t/* end of the kernel processed data */\n");
    }
    if (!template_only) 
	if (mask == akbRequest) {
	    WriteList(file, args, func, mask | akbSendBody, "\n", "\n");
	    if (!isuser)
		WriteTrailerDecl(file, trailer);
	} else {
	    WriteList(file, args, func, mask | akbReturnBody, "\n", "\n");
	    if (isuser)
		WriteTrailerDecl(file, trailer);
	}
    fprintf(file, "\t} %s;\n", name);
    fprintf(file, "\n");
}

void
WriteTemplateDeclIn(FILE *file, register argument_t *arg)
{
    (*arg->argKPD_Template)(file, arg, TRUE);
}

void
WriteTemplateDeclOut(FILE *file, register argument_t *arg)
{
    (*arg->argKPD_Template)(file, arg, FALSE);
}

void
WriteTemplateKPD_port(FILE *file, register argument_t *arg, boolean_t in)
{
    register ipc_type_t *it = arg->argType;

    fprintf(file, "#if\tUseStaticTemplates\n");
    fprintf(file, "\tstatic %s %s = {\n", it->itUserKPDType, arg->argTTName);

    fprintf(file, "\t\t/* name = */\t\tMACH_PORT_NULL,\n");
    fprintf(file, "\t\t/* pad1 = */\t\t0,\n");
    fprintf(file, "\t\t/* pad2 = */\t\t0,\n");
    fprintf(file, "\t\t/* disp = */\t\t%s,\n",
	in ? it->itInNameStr: it->itOutNameStr);
    fprintf(file, "\t\t/* type = */\t\tMACH_MSG_PORT_DESCRIPTOR,\n");

    fprintf(file, "\t};\n");
    fprintf(file, "#endif\t/* UseStaticTemplates */\n");
}

void
WriteTemplateKPD_ool(FILE *file, argument_t *arg, boolean_t in)
{
    register ipc_type_t *it = arg->argType;

    fprintf(file, "#if\tUseStaticTemplates\n");
    fprintf(file, "\tstatic %s %s = {\n", it->itUserKPDType, arg->argTTName);

    if (IS_MULTIPLE_KPD(it))
	it = it->itElement;

    fprintf(file, "\t\t/* addr = */\t\t(void *)0,\n");
    if (it->itVarArray)
	fprintf(file, "\t\t/* size = */\t\t0,\n");
    else
	fprintf(file, "\t\t/* size = */\t\t%d,\n",
	    (it->itNumber * it->itSize + 7)/8);
    fprintf(file, "\t\t/* deal = */\t\t%s,\n",
	(arg->argDeallocate == d_YES) ? "TRUE" : "FALSE");
    /* the d_MAYBE case will be fixed runtime */
    fprintf(file, "\t\t/* copy = */\t\t%s,\n",
	(arg->argFlags & flPhysicalCopy) ? "MACH_MSG_PHYSICAL_COPY" : "MACH_MSG_VIRTUAL_COPY");
    /* the PHYSICAL COPY flag has not been established yet */
    fprintf(file, "\t\t/* pad2 = */\t\t0,\n");
    fprintf(file, "\t\t/* type = */\t\tMACH_MSG_OOL_DESCRIPTOR,\n");

    fprintf(file, "\t};\n");
    fprintf(file, "#endif\t/* UseStaticTemplates */\n");
}

void
WriteTemplateKPD_oolport(FILE *file, argument_t *arg, boolean_t in)
{
    register ipc_type_t *it = arg->argType;

    fprintf(file, "#if\tUseStaticTemplates\n");
    fprintf(file, "\tstatic %s %s = {\n", it->itUserKPDType, arg->argTTName);

    if (IS_MULTIPLE_KPD(it))
	it = it->itElement;

    fprintf(file, "\t\t/* addr = */\t\t(void *)0,\n");
    if (!it->itVarArray)
	fprintf(file, "\t\t/* coun = */\t\t%d,\n",
	    it->itNumber);
    else
	fprintf(file, "\t\t/* coun = */\t\t0,\n");
    fprintf(file, "\t\t/* deal = */\t\t%s,\n",
        (arg->argDeallocate == d_YES) ? "TRUE" : "FALSE");
    fprintf(file, "\t\t/* copy is meaningful only in overwrite mode */\n");
    fprintf(file, "\t\t/* copy = */\t\tMACH_MSG_PHYSICAL_COPY,\n");
    fprintf(file, "\t\t/* disp = */\t\t%s,\n",
	in ? it->itInNameStr: it->itOutNameStr);
    fprintf(file, "\t\t/* type = */\t\tMACH_MSG_OOL_PORTS_DESCRIPTOR,\n");

    fprintf(file, "\t};\n");
    fprintf(file, "#endif\t/* UseStaticTemplates */\n");
}

/*
 * Like vfprintf, but omits a leading comment in the format string
 * and skips the items that would be printed by it.  Only %s, %d,
 * and %f are recognized.
 */
/* Like vfprintf, but omits a leading comment in the format string
 * and skips the items that would be printed by it.  Only %s, %d,
 * and %f are recognized.
 *
 * Returns: the number of fields (and their types in types_out) skipped
 * by the leading comment.  The caller should then advance its va_list
 * by consuming the same types before further use.
 */
int
SkipVFPrintf(FILE *file, register char *fmt, va_list pvar, char *types_out, int max_types)
{
    if (*fmt == 0) {
        /* nothing to print */
        return 0;
    }

    if (fmt[0] == '/' && fmt[1] == '*') {
        const char *p = fmt + 2;
        int tcount = 0;
        int c;

        while (*p) {
            c = *p++;
            if (c == '*' && *p == '/') {
                p++; /* skip '/' */
                break;
            }
            if (c == '%') {
                char t = *p++;
                if ((t == 's' || t == 'd' || t == 'f') && tcount < max_types) {
                    types_out[tcount++] = t;
                }
            }
        }

        /* Skip optional space after comment */
        if (*p == ' ')
            p++;

        /* Call vfprintf on the remainder using a copy of the incoming va_list */
        {
            va_list ptmp;
            va_copy(ptmp, pvar);
            /* advance ptmp by the number/types of fields we recorded */
            {
                int i;
                for (i = 0; i < tcount; ++i) {
                    switch (types_out[i]) {
                        case 's': (void) va_arg(ptmp, char *); break;
                        case 'd': (void) va_arg(ptmp, int); break;
                        case 'f': (void) va_arg(ptmp, double); break;
                        default: break;
                    }
                }
            }
            /* Optional debug: dump the arguments that will be consumed by vfprintf */
            if (getenv("MIG_DEBUG_SKIPVF")) {
                va_list pdump;
                va_copy(pdump, ptmp);
                {
                    int i;
                    for (i = 0; i < tcount; ++i) {
                        switch (types_out[i]) {
                            case 's': {
                                char *s = va_arg(pdump, char *);
                                fprintf(stderr, "[SkipVFPrintf-debug] arg %d type %%s -> %p\n", i, (void*)s);
                                break;
                            }
                            case 'd': {
                                int d = va_arg(pdump, int);
                                fprintf(stderr, "[SkipVFPrintf-debug] arg %d type %%d -> %d\n", i, d);
                                break;
                            }
                            case 'f': {
                                double f = va_arg(pdump, double);
                                fprintf(stderr, "[SkipVFPrintf-debug] arg %d type %%f -> %g\n", i, f);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                va_end(pdump);
            }
            /* Optional in-process check: capture the exact bytes written by this vfprintf */
            if (getenv("MIG_CHECK_WRITEBUF")) {
                int fd = fileno(file);
                off_t before = lseek(fd, 0, SEEK_END);
                (void) vfprintf(file, (char *)p, ptmp);
                fflush(file);
                off_t after = lseek(fd, 0, SEEK_END);
                if (after > before) {
                    off_t sz = after - before;
                    char *buf = malloc(sz);
                    if (buf) {
                        if (pread(fd, buf, sz, before) == sz) {
                            /* scan for non-ASCII */
                            off_t i;
                            for (i = 0; i < sz; ++i) {
                                unsigned char c = (unsigned char)buf[i];
                                if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                                    fprintf(stderr, "[SkipVFPrintf-Check] non-ASCII byte 0x%02x at offset %lld (in emission)\n", c, (long long)i);
                                    {
                                        void *bt[32]; int n = backtrace(bt, 32);
                                        backtrace_symbols_fd(bt, n, fileno(stderr));
                                    }
                                    /* dump context */
                                    {
                                        long start = (i > 64) ? i - 64 : 0;
                                        long end = (i + 192 < sz) ? i + 192 : sz;
                                        long j;
                                        fprintf(stderr, "context hex:");
                                        for (j = start; j < end; ++j) fprintf(stderr, " %02x", (unsigned char)buf[j]);
                                        fprintf(stderr, "\n");
                                    }
                                    raise(SIGTRAP);
                                    break;
                                }
                            }
                        }
                        free(buf);
                    }
                }
                va_end(ptmp);
            } else {
                (void) vfprintf(file, (char *)p, ptmp);
                va_end(ptmp);
            }
        }
        return tcount;
    }

    /* No leading comment - just print */
    if (getenv("MIG_CHECK_WRITEBUF")) {
        int fd = fileno(file);
        off_t before = lseek(fd, 0, SEEK_END);
        (void) vfprintf(file, fmt, pvar);
        fflush(file);
        off_t after = lseek(fd, 0, SEEK_END);
        if (after > before) {
            off_t sz = after - before;
            char *buf = malloc(sz);
            if (buf) {
                if (pread(fd, buf, sz, before) == sz) {
                    off_t i;
                    for (i = 0; i < sz; ++i) {
                        unsigned char c = (unsigned char)buf[i];
                        if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                            fprintf(stderr, "[SkipVFPrintf-Check] non-ASCII byte 0x%02x at offset %lld (in emission)\n", c, (long long)i);
                            {
                                void *bt[32]; int n = backtrace(bt, 32);
                                backtrace_symbols_fd(bt, n, fileno(stderr));
                            }
                            {
                                long start = (i > 64) ? i - 64 : 0;
                                long end = (i + 192 < sz) ? i + 192 : sz;
                                long j;
                                fprintf(stderr, "context hex:");
                                for (j = start; j < end; ++j) fprintf(stderr, " %02x", (unsigned char)buf[j]);
                                fprintf(stderr, "\n");
                            }
                            raise(SIGTRAP);
                            break;
                        }
                    }
                }
                free(buf);
            }
        }
    } else {
        (void) vfprintf(file, fmt, pvar);
    }
    return 0;
}

static void
vWriteCopyType(FILE *file, ipc_type_t *it, char *left, char *right, va_list pvar)
{
    if (it->itStruct)
    {
	fprintf(file, "\t");
	{
	    char __types[16];
	    int __tcnt = SkipVFPrintf(file, left, pvar, __types, sizeof(__types));
	    int __i;
	    for (__i = 0; __i < __tcnt; ++__i) {
		switch (__types[__i]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, " = ");
	{
	    char __types2[16];
	    int __tcnt2 = SkipVFPrintf(file, right, pvar, __types2, sizeof(__types2));
	    int __i2;
	    for (__i2 = 0; __i2 < __tcnt2; ++__i2) {
		switch (__types2[__i2]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, ";\n");
    }
    else if (it->itString)
    {
	fprintf(file, "\t(void) mig_strncpy(");
	{
	    char __types[16];
	    int __tcnt = SkipVFPrintf(file, left, pvar, __types, sizeof(__types));
	    int __i;
	    for (__i = 0; __i < __tcnt; ++__i) {
		switch (__types[__i]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, ", ");
	{
	    char __types2[16];
	    int __tcnt2 = SkipVFPrintf(file, right, pvar, __types2, sizeof(__types2));
	    int __i2;
	    for (__i2 = 0; __i2 < __tcnt2; ++__i2) {
		switch (__types2[__i2]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, ", %d);\n", it->itTypeSize);
    }
    else
    {
	fprintf(file, "\t{   typedef struct { char data[%d]; } *sp;\n",
		it->itTypeSize);
	fprintf(file, "\t    * (sp) ");
	{
	    char __types[16];
	    int __tcnt = SkipVFPrintf(file, left, pvar, __types, sizeof(__types));
	    int __i;
	    for (__i = 0; __i < __tcnt; ++__i) {
		switch (__types[__i]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, " = * (sp) ");
	{
	    char __types2[16];
	    int __tcnt2 = SkipVFPrintf(file, right, pvar, __types2, sizeof(__types2));
	    int __i2;
	    for (__i2 = 0; __i2 < __tcnt2; ++__i2) {
		switch (__types2[__i2]) {
		    case 's': (void) va_arg(pvar, char *); break;
		    case 'd': (void) va_arg(pvar, int); break;
		    case 'f': (void) va_arg(pvar, double); break;
		    default: break;
		}
	    }
	}
	fprintf(file, ";\n\t}\n");
    }
}

/* Simple, safe WriteCopyType that accepts fully-expanded left/right
 * strings (no printf-style varargs).  This avoids varargs propagation
 * and the associated uninitialized-memory risks when arguments are
 * assembled across multiple layers.
 */
void
WriteCopyTypeSimple(FILE *file, ipc_type_t *it, const char *left, const char *right)
{
    /* Optional check: ensure left/right strings are ASCII (detects embedded garbage) */
    if (getenv("MIG_CHECK_WRITEBUF")) {
        const unsigned char *l = (const unsigned char *)left;
        const unsigned char *r = (const unsigned char *)right;
        size_t i;
        for (i = 0; l && l[i]; ++i) {
            unsigned char c = l[i];
            if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                fprintf(stderr, "[WriteCopyTypeSimple-Check] non-ASCII byte 0x%02x in left at offset %zu\n", c, i);
                {
                    void *bt[32]; int n = backtrace(bt, 32);
                    backtrace_symbols_fd(bt, n, fileno(stderr));
                }
                raise(SIGTRAP);
                break;
            }
        }
        for (i = 0; r && r[i]; ++i) {
            unsigned char c = r[i];
            if ((c < 32 && c != 9 && c != 10 && c != 13) || c > 126) {
                fprintf(stderr, "[WriteCopyTypeSimple-Check] non-ASCII byte 0x%02x in right at offset %zu\n", c, i);
                {
                    void *bt[32]; int n = backtrace(bt, 32);
                    backtrace_symbols_fd(bt, n, fileno(stderr));
                }
                raise(SIGTRAP);
                break;
            }
        }
    }

    if (it->itStruct) {
        fprintf(file, "\t%s = %s;\n", left, right);
    } else if (it->itString) {
        fprintf(file, "\t(void) mig_strncpy(%s, %s, %d);\n", left, right, it->itTypeSize);
    } else {
        fprintf(file, "\t{   typedef struct { char data[%d]; } *sp;\n", it->itTypeSize);
        fprintf(file, "\t    * (sp) %s = * (sp) %s;\n\t}\n", left, right);
    }
}


/*ARGSUSED*/
/*VARARGS4*/
void
WriteCopyType(FILE *file, ipc_type_t *it, char *left, char *right, ...)
{
    va_list pvar;
    va_start(pvar, right);

    vWriteCopyType(file, it, left, right, pvar);

    va_end(pvar);
}


/*ARGSUSED*/
/*VARARGS4*/
void
WriteCopyArg(FILE *file, argument_t *arg, char *left, char *right, ...)
{
    va_list pvar;
    va_start(pvar, right);

    {
	ipc_type_t *it = arg->argType;
	if (it->itVarArray && !it->itString) {
	    fprintf(file, "\t    (void)memcpy(");
	    {
	        char __types[16];
	        int __tcnt = SkipVFPrintf(file, left, pvar, __types, sizeof(__types));
	        int __i;
	        for (__i = 0; __i < __tcnt; ++__i) {
	            switch (__types[__i]) {
	            case 's': (void) va_arg(pvar, char *); break;
	            case 'd': (void) va_arg(pvar, int); break;
	            case 'f': (void) va_arg(pvar, double); break;
	            default: break;
	            }
	        }
	    }
	    fprintf(file, ", ");
	    {
	        char __types2[16];
	        int __tcnt2 = SkipVFPrintf(file, right, pvar, __types2, sizeof(__types2));
	        int __i2;
	        for (__i2 = 0; __i2 < __tcnt2; ++__i2) {
	            switch (__types2[__i2]) {
	            case 's': (void) va_arg(pvar, char *); break;
	            case 'd': (void) va_arg(pvar, int); break;
	            case 'f': (void) va_arg(pvar, double); break;
	            default: break;
	            }
	        }
	    }
	    fprintf(file, ", %s);\n", arg->argCount->argVarName);
	} else
	    vWriteCopyType(file, it, left, right, pvar);
    }

    va_end(pvar);
}


/*
 * Global KPD disciplines 
 */
void
KPD_error(FILE *file, argument_t *arg)
{
    printf("MiG internal error: argument is %s\n", arg->argVarName);
    exit(1);
}

void
KPD_noop(FILE *file, argument_t *arg)
{
}

/* Wrapper for KPD_error matching the Template signature (takes 'in' flag). */
void
KPD_error_template(FILE *file, argument_t *arg, boolean_t in)
{
    /* 'in' parameter is ignored; forward to the common KPD_error handler */
    KPD_error(file, arg);
}

static void
WriteStringDynArgs(argument_t *args, u_int mask, string_t InPOutP, string_t *str_oolports, string_t *str_ool)
{
    argument_t *arg;
    char loc[100], sub[20];
    string_t tmp_str1 = ""; 
    string_t tmp_str2 = "";
    int cnt, multiplier = 1;
    boolean_t test, complex = FALSE;

    for (arg = args; arg != argNULL; arg = arg->argNext) {
	ipc_type_t *it = arg->argType;

	if (IS_MULTIPLE_KPD(it)) {
	    test = it->itVarArray || it->itElement->itVarArray;
	    if (test) {
		multiplier = it->itKPD_Number;
	        it = it->itElement;
	        complex = TRUE;
	    }
	} else
	    test = it->itVarArray;

	cnt = multiplier;
	while (cnt) {
	    if (complex)
		SafeSnprintf(sub, sizeof(sub), "[%d]", multiplier - cnt);
	    if (akCheck(arg->argKind, mask) && 
		it->itPortType && !it->itInLine && test) {
		    SafeSnprintf(loc, sizeof(loc), " + %s->%s%s.count", InPOutP, arg->argMsgField,
		        complex ? sub : "");
		    tmp_str1 = strconcat(tmp_str1, loc);
	    }
	    if (akCheck(arg->argKind, mask) && 
		!it->itInLine && !it->itPortType && test) {
	 	    SafeSnprintf(loc, sizeof(loc), " + %s->%s%s.size", InPOutP, arg->argMsgField,
		        complex ? sub : "");
		    tmp_str2 = strconcat(tmp_str2, loc);
	    }
	    cnt--;
	}
    }
    /* Debugging checks: detect non-ASCII bytes in the constructed strings and print backtrace for diagnostics. */
    if (tmp_str1 && tmp_str1[0]) {
        size_t __len1 = strlen(tmp_str1);
        size_t __i1;
        for (__i1 = 0; __i1 < __len1; ++__i1) {
            unsigned char __c = (unsigned char)tmp_str1[__i1];
            if ((__c < 32 && __c != 9 && __c != 10 && __c != 13) || __c > 126) {
                fprintf(stderr, "[DEBUG-WriteStringDynArgs] non-ASCII byte 0x%02x in tmp_str1 at offset %zu (len=%zu)\n", __c, __i1, __len1);
                void *__bt[32]; int __n = backtrace(__bt, 32); backtrace_symbols_fd(__bt, __n, fileno(stderr));
                /* also dump a small hex context */
                size_t __start = (__i1 > 32) ? (__i1 - 32) : 0;
                size_t __end = (__i1 + 128 < __len1) ? (__i1 + 128) : __len1;
                fprintf(stderr, "context hex (tmp_str1):");
                size_t __j;
                for (__j = __start; __j < __end; ++__j) fprintf(stderr, " %02x", (unsigned char)tmp_str1[__j]);
                fprintf(stderr, "\n");
                break;
            }
        }
        if (__len1 > 1024)
            fprintf(stderr, "[DEBUG-WriteStringDynArgs] tmp_str1 length = %zu\n", __len1);
    }
    if (tmp_str2 && tmp_str2[0]) {
        size_t __len2 = strlen(tmp_str2);
        size_t __i2;
        for (__i2 = 0; __i2 < __len2; ++__i2) {
            unsigned char __c = (unsigned char)tmp_str2[__i2];
            if ((__c < 32 && __c != 9 && __c != 10 && __c != 13) || __c > 126) {
                fprintf(stderr, "[DEBUG-WriteStringDynArgs] non-ASCII byte 0x%02x in tmp_str2 at offset %zu (len=%zu)\n", __c, __i2, __len2);
                void *__bt2[32]; int __n2 = backtrace(__bt2, 32); backtrace_symbols_fd(__bt2, __n2, fileno(stderr));
                size_t __start2 = (__i2 > 32) ? (__i2 - 32) : 0;
                size_t __end2 = (__i2 + 128 < __len2) ? (__i2 + 128) : __len2;
                fprintf(stderr, "context hex (tmp_str2):");
                size_t __j;
                for (__j = __start2; __j < __end2; ++__j) fprintf(stderr, " %02x", (unsigned char)tmp_str2[__j]);
                fprintf(stderr, "\n");
                break;
            }
        }
        if (__len2 > 1024)
            fprintf(stderr, "[DEBUG-WriteStringDynArgs] tmp_str2 length = %zu\n", __len2);
    }

    *str_oolports = tmp_str1;
    *str_ool = tmp_str2;  
}

/*
 * Utilities for Logging Events that happen at the stub level
 */
void
WriteLogMsg(FILE *file, routine_t *rt, int where, int what)
{
    string_t ptr_str;
    string_t StringOolPorts = strNULL;
    string_t StringOOL = strNULL;
    u_int ports, oolports, ool;
    string_t event;

    fprintf(file, "\n#if  MIG_DEBUG\n");
    if (where == LOG_USER)
	fprintf(file, "\tLOG_TRACE(MACH_MSG_LOG_USER,\n");
    else
	fprintf(file, "\tLOG_TRACE(MACH_MSG_LOG_SERVER,\n");
    if (where == LOG_USER && what == LOG_REQUEST) {
	ptr_str = "InP";
	event = "MACH_MSG_REQUEST_BEING_SENT";
    } else if (where == LOG_USER && what == LOG_REPLY) {
	ptr_str = "Out0P";
	event = "MACH_MSG_REPLY_BEING_RCVD";
    } else if (where == LOG_SERVER && what == LOG_REQUEST) {
	ptr_str = "In0P";
	event = "MACH_MSG_REQUEST_BEING_RCVD";
    } else {
	ptr_str = "OutP";
	event = "MACH_MSG_REPLY_BEING_SENT";
    }
    WriteStringDynArgs(rt->rtArgs, 
	(what == LOG_REQUEST) ? akbSendKPD : akbReturnKPD, 
	ptr_str, &StringOolPorts, &StringOOL);
    fprintf(file, "\t\t%s,\n", event);
    fprintf(file, "\t\t%s->Head.msgh_id,\n", ptr_str);
    if (where == LOG_USER && what == LOG_REQUEST) {
	if (rt->rtNumRequestVar)
	    fprintf(file, "\t\tmsgh_size,\n");
	else
	    fprintf(file, "\t\tsizeof(Request),\n");
    } else 
	fprintf(file, "\t\t%s->Head.msgh_size,\n", ptr_str);
    if ((what == LOG_REQUEST && rt->rtSimpleRequest == FALSE) ||
	(what == LOG_REPLY && rt->rtSimpleReply == FALSE))
	    fprintf(file, "\t\t%s->msgh_body.msgh_descriptor_count,\n", ptr_str);
    else
	    fprintf(file, "\t\t0, /* Kernel Proc. Data entries */\n");
    if (what == LOG_REQUEST) {
	fprintf(file, "\t\t0, /* RetCode */\n");
	ports = rt->rtCountPortsIn;
        oolports = rt->rtCountOolPortsIn;
	ool = rt->rtCountOolIn;
    } else {
	if (akCheck(rt->rtRetCode->argKind, akbReply))
	    fprintf(file, "\t\t%s->RetCode,\n", ptr_str);
	else
	    fprintf(file, "\t\t0, /* RetCode */\n");
	ports = rt->rtCountPortsOut;
        oolports = rt->rtCountOolPortsOut;
	ool = rt->rtCountOolOut;
    }
    fprintf(file, "\t\t/* Ports */\n");
    fprintf(file, "\t\t%d,\n", ports);
    fprintf(file, "\t\t/* Out-of-Line Ports */\n");
    fprintf(file, "\t\t%d", oolports);
    if (StringOolPorts != strNULL)
	fprintf(file, "%s,\n", StringOolPorts);
    else
	fprintf(file, ",\n");
    fprintf(file, "\t\t/* Out-of-Line Bytes */\n");
    fprintf(file, "\t\t%d", ool);
    if (StringOOL != strNULL)
	fprintf(file, "%s,\n", StringOOL);
    else
	fprintf(file, ",\n");
    fprintf(file, "\t\t__FILE__, __LINE__);\n");
    fprintf(file, "#endif /* MIG_DEBUG */\n\n");
}

void
WriteLogDefines(FILE *file, string_t who)
{
    fprintf(file, "#if  MIG_DEBUG\n");
    fprintf(file, "#define LOG_W_E(X)\tLOG_ERRORS(%s, \\\n", who);
    fprintf(file, "\t\t\tMACH_MSG_ERROR_WHILE_PARSING, (void *)(X), __FILE__, __LINE__)\n");
    fprintf(file, "#else  /* MIG_DEBUG */\n");
    fprintf(file, "#define LOG_W_E(X)\n");
    fprintf(file, "#endif /* MIG_DEBUG */\n");
    fprintf(file, "\n");
}

/* common utility to report errors */
void
WriteReturnMsgError(FILE *file, routine_t *rt, boolean_t isuser, argument_t *arg, string_t error)
{
    char space[MAX_STR_LEN];
    string_t string = &space[0];

    if (UseEventLogger && arg != argNULL) 
	SafeSnprintf(string, MAX_STR_LEN, "LOG_W_E(\"%s\"); ", arg->argVarName);
    else
	string = "";

    fprintf(file, "\t\t{ ");

    if (isuser) {
   	if (! rt->rtMessOnStack)
		fprintf(file, "%s((char *) Mess, sizeof(*Mess)); ", MessFreeRoutine);

        fprintf(file, "%sreturn %s; }\n", string, error);
    }
    else
        fprintf(file, "%sMIG_RETURN_ERROR(OutP, %s); }\n", string, error);
}

/* executed iff elements are defined */
void
WriteCheckTrailerHead(FILE *file, routine_t *rt, boolean_t isuser)
{
    string_t who = (isuser) ? "Out0P" : "In0P";

    fprintf(file, "\tTrailerP = (mach_msg_format_0_trailer_t *)((vm_offset_t)%s +\n", who);
    fprintf(file, "\t\tround_msg(%s->Head.msgh_size));\n", who);
    fprintf(file, "\tif (TrailerP->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0)\n");

    WriteReturnMsgError(file, rt, isuser, argNULL, "MIG_TRAILER_ERROR");
    
    fprintf(file, "#if\tTypeCheck\n");
    fprintf(file, "\ttrailer_size = TrailerP->msgh_trailer_size -\n");
    fprintf(file, "\t\tsizeof(mach_msg_trailer_type_t) - sizeof(mach_msg_trailer_size_t);\n");
    fprintf(file, "#endif\t/* TypeCheck */\n");
}

/* executed iff elements are defined */
void
WriteCheckTrailerSize(FILE *file, boolean_t isuser, argument_t *arg)
{
    fprintf(file, "#if\tTypeCheck\n");
    if (akIdent(arg->argKind) == akeMsgSeqno) {
	fprintf(file, "\tif (trailer_size < sizeof(mach_port_seqno_t))\n");
	WriteReturnMsgError(file, arg->argRoutine, isuser, arg, "MIG_TRAILER_ERROR");
	fprintf(file, "\ttrailer_size -= sizeof(mach_port_seqno_t);\n");
    } else if (akIdent(arg->argKind) == akeSecToken) {
	fprintf(file, "\tif (trailer_size < sizeof(security_token_t))\n");
	WriteReturnMsgError(file, arg->argRoutine, isuser, arg, "MIG_TRAILER_ERROR");
	fprintf(file, "\ttrailer_size -= sizeof(security_token_t);\n");
    }
    fprintf(file, "#endif\t/* TypeCheck */\n");
}
