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
	    (*func)(file, arg);
	    sawone = TRUE;
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
    /* Just output the string - identifiers should be clean */
    fputs(s, file);
}

/*
 * How many bytes one element of an out-of-line region occupies (#416).
 *
 * Out-of-line data does not travel in the message: the message carries a
 * descriptor holding an address and a byte count, and that count is the
 * caller's element count multiplied by this.  So this number is used on every
 * call, and no assertion about the message's own layout reaches it — none of
 * those bytes is in the message.
 *
 * ⚠️ It lives here because it was computed in two places with two different
 * conditions.  The user stub multiplied when `argMultiplier > 1 || howbig >
 * 8`; the server stub multiplied only when `howbig > 8`.  An element of two
 * or more components each a byte wide — `array[] of struct[2] of char` — is
 * therefore counted twice by the sender and once by the receiver: the same
 * region, two lengths, and nothing in either stub that could notice.  No
 * .defs in this tree declares such a type, which is why it has never fired;
 * one function is how it stops being able to.
 *
 * A multiplier of zero reaches the same fate as one.  Zero bytes per element
 * is not a size any declaration means, and the arithmetic that produced it
 * would silently transfer nothing.
 */
u_int
OolElementUnit(register argument_t *arg)
{
    register ipc_type_t *it = arg->argType;
    register argument_t *count;
    u_int howbig, mult;

    if (IS_MULTIPLE_KPD(it)) {
	count = arg->argSubCount;
	howbig = it->itElement->itSize;
    } else {
	count = arg->argCount;
	howbig = it->itSize;
    }

    mult = (count != argNULL && count->argMultiplier > 0)
	   ? count->argMultiplier : 1;

    if (mult > 1 || howbig > 8)
	return mult * howbig / 8;

    return 1;
}

/*
 * And the name of the C type that unit is the size of, when there is one.
 *
 * There is one exactly when the unit is wider than a byte.  A region measured
 * in bytes is measured in bytes on every target and by every declaration —
 * `pointer_t` is `^array[] of MACH_MSG_TYPE_BYTE`, and its element has no C
 * type of its own to disagree about.  Above a byte the count means elements,
 * the element is a real type, and its width is a claim worth settling.
 */
identifier_t
OolElementType(register argument_t *arg, boolean_t isuser)
{
    register ipc_type_t *it = arg->argType;
    register ipc_type_t *element;
    register const char *c;

    /*
     * Out-of-line *data* only.  A port argument has no region behind it, and
     * an out-of-line array of ports is measured in ports — migcom fills in a
     * count, not a byte size, so there is no multiplier here to settle.  An
     * assertion on either would be true and would be about nothing, which
     * teaches a reader to skim them.
     */
    if (it->itInLine || it->itPortType)
	return strNULL;
    if (OolElementUnit(arg) <= 1)
	return strNULL;

    element = IS_MULTIPLE_KPD(it) ? it->itElement->itElement : it->itElement;
    if (element == itNULL)
	return strNULL;

    c = isuser ? element->itUserType : element->itServerType;
    if (c == strNULL)
	return strNULL;

    /* A name with a `*` or a space in it is a derived type whose spelling is
       not guaranteed to be usable where an assertion needs it. */
    for (; *c != '\0'; c++)
	if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
	      || (*c >= '0' && *c <= '9') || *c == '_'))
	    return strNULL;

    return isuser ? element->itUserType : element->itServerType;
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


/*
 * What migcom believes about a message, checked by the only thing that knows
 * (#416).
 *
 * migcom sizes a message by adding up its fields.  The compiler lays out the
 * same fields by adding up their sizes *and the padding between them*, and it
 * knows something migcom cannot: the alignment of each field's C type.  All
 * migcom has is the wire tag and the type's name as a string, and
 * `unsigned64` is `unsigned long` in one declaration and
 * `struct { unsigned int val[2]; }` in another — eight-aligned and
 * four-aligned, with no rule leading from the tag to the answer.
 *
 * The failure is silent in the worst way.  Both ends of an RPC use migcom's
 * numbers, so a message goes out with a length that stops before its last
 * field and arrives at a server that finds one field where the next begins.
 *
 * ── Everything migcom claims, and nothing less ────────────────────────
 *
 * Its beliefs are not only about the whole message.  A field's size is used
 * as the length of a memcpy into the message, as the multiplier in
 * `msgh_size = base + n * <size>`, as the constant subtracted when the
 * structure pointer is slid back over a short array, and as the size of the
 * buffer the caller allocates.  Every one of those is the same number —
 * `sizeof` of a C type migcom has never seen — so every one of them is
 * asserted:
 *
 *	each field	its size, and for an array its element's size too
 *	each padding	the size migcom declared it
 *	the message	the largest it can be, and the smallest
 *
 * With every field pinned, the derived uses are founded rather than hoped
 * for; and with the message pinned, so is every msgh_size constant and the
 * subsystem-wide maximum, which is a maximum over exactly these.
 *
 * ⚠️ Both message sizes are measured to `msgh_end`, a zero-length marker at
 * the end of the message part of every structure.  Not to `sizeof`, which
 * includes the padding a compiler adds at the end and which the sender never
 * transmits; not to the trailer, which only the receiving side declares.  The
 * sender and the receiver must arrive at the *same* number, and a marker both
 * of them carry is what guarantees it.
 */
/*
 * And that unit, settled against the compiler (#416).
 *
 * This is the last number migcom emits about a message that no other
 * assertion reaches, because the bytes it measures are not in the message:
 * the descriptor holds an address, and this is what the caller's element
 * count is multiplied by to say how many bytes to transfer.  Getting it wrong
 * transfers the wrong amount of somebody else's memory, on every call, and
 * nothing in either stub is in a position to notice.
 *
 * It is checked against the element type's own name, and it is the *shared*
 * derivation that is checked — the same call the two stubs use to emit the
 * multiplier.  Asserting a separately computed copy would compare migcom with
 * migcom and prove nothing.
 */
static void
WriteOolUnitAssert(FILE *file, register argument_t *arg, boolean_t isuser)
{
    identifier_t type = OolElementType(arg, isuser);

    if (type == strNULL)
	return;

    fprintf(file, "\t_Static_assert(sizeof(%s) == %d,\n",
	    type, OolElementUnit(arg));
    fprintf(file, "\t\t\"MIG measures the out-of-line %s in the wrong unit\");\n",
	    arg->argMsgField);
}

static void
WriteFieldAssert(FILE *file, char *name, register argument_t *arg)
{
    register ipc_type_t *it = arg->argType;
    boolean_t is_array = IS_VARIABLE_SIZED_UNTYPED(it) || it->itNoOptArray
			 || IS_MULTIPLE_KPD(it);

    fprintf(file, "\t_Static_assert(sizeof(((%s *) 0)->%s) == %d,\n",
	    name, arg->argMsgField, it->itTypeSize);
    fprintf(file, "\t\t\"MIG and the compiler disagree on the size of %s\");\n",
	    arg->argMsgField);

    /*
     * The element's size is the multiplier in the run-time length arithmetic,
     * so it has to be pinned separately: sixteen bytes of array is sixteen
     * ones or four fours, and only the second is right if the count means
     * elements.
     */
    if (is_array && it->itElement != itNULL) {
	fprintf(file, "\t_Static_assert(sizeof(((%s *) 0)->%s[0]) == %d,\n",
		name, arg->argMsgField, it->itElement->itTypeSize);
	fprintf(file, "\t\t\"MIG counts %s in units the compiler disagrees with\");\n",
		arg->argMsgField);
    }

    if (it->itPadSize != 0) {
	fprintf(file, "\t_Static_assert(sizeof(((%s *) 0)->%s) == %d,\n",
		name, arg->argPadName, it->itPadSize);
	fprintf(file, "\t\t\"MIG and the compiler disagree on the padding after %s\");\n",
		arg->argMsgField);
    }
}

static void
WriteStructAssert(FILE *file, char *name, argument_t *args, u_int mask,
		  boolean_t simple, boolean_t isuser,
		  u_int minsize, u_int maxsize)
{
    register argument_t *arg;
    u_int kpd  = mask | (mask == akbRequest ? akbSendKPD  : akbReturnKPD);
    u_int body = mask | (mask == akbRequest ? akbSendBody : akbReturnBody);

    /*
     * ⚠️ Selected exactly as WriteFieldDeclPrim was selected to declare them
     * — same predicate, same two groups, same order.  An assertion about a
     * field that was never declared does not fail, it fails to compile, and
     * the two lists must be the same list.
     */
    if (!simple)
	for (arg = args; arg != argNULL; arg = arg->argNext)
	    if (akCheckAll(arg->argKind, kpd)) {
		WriteFieldAssert(file, name, arg);
		WriteOolUnitAssert(file, arg, isuser);
	    }

    for (arg = args; arg != argNULL; arg = arg->argNext)
	if (akCheckAll(arg->argKind, body))
	    WriteFieldAssert(file, name, arg);

    fprintf(file, "\n\t_Static_assert(__builtin_offsetof(%s, msgh_end) == %d,\n",
	    name, maxsize);
    fprintf(file, "\t\t\"MIG sized %s differently from the compiler\");\n\n",
	    name);

    if (minsize == maxsize)
	return;

    /*
     * The smallest the message can be is the largest less every variable
     * array, and that is not an estimate: the array is declared at its
     * maximum, so subtracting it *is* the message with that array empty.
     */
    fprintf(file, "\t_Static_assert(__builtin_offsetof(%s, msgh_end)", name);
    for (arg = args; arg != argNULL; arg = arg->argNext) {
	register ipc_type_t *it = arg->argType;

	if (!akCheckAll(arg->argKind, body))
	    continue;
	if (!IS_VARIABLE_SIZED_UNTYPED(it))
	    continue;

	fprintf(file, "\n\t\t- sizeof(((%s *) 0)->%s)", name, arg->argMsgField);
	if (it->itPadSize != 0)
	    fprintf(file, " - sizeof(((%s *) 0)->%s)", name, arg->argPadName);
    }
    fprintf(file, "\n\t\t== %d,\n", minsize);
    fprintf(file, "\t\t\"MIG sized an empty %s differently from the compiler\");\n\n",
	    name);
}

void
WriteStructDecl(FILE *file, argument_t *args,
                void (*func)(FILE *file, argument_t *arg),
                u_int mask, char *name,
                boolean_t simple, boolean_t trailer,
                boolean_t isuser, boolean_t template_only,
                u_int minsize, u_int maxsize)
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
    /*
     * Where the message ends, marked (#416).
     *
     * A zero-length member: no size, no alignment, so it moves nothing and
     * costs nothing — and `offsetof` of it is exactly the number both the
     * sender and the receiver need, on a structure where only one of them
     * declares a trailer and neither should be counting the padding the
     * compiler puts at the end.
     *
     * The trailer belongs to whoever receives the message: the server sees it
     * on a request, the caller sees it on a reply.  So it goes after the
     * marker, outside the message proper, which is what it is.
     */
    if (!template_only)
	if (mask == akbRequest) {
	    WriteList(file, args, func, mask | akbSendBody, "\n", "\n");
	    fprintf(file, "\t\tchar msgh_end[0];\n");
	    if (!isuser)
		WriteTrailerDecl(file, trailer);
	} else {
	    WriteList(file, args, func, mask | akbReturnBody, "\n", "\n");
	    fprintf(file, "\t\tchar msgh_end[0];\n");
	    if (isuser)
		WriteTrailerDecl(file, trailer);
	}
    fprintf(file, "\t} %s;\n", name);
    fprintf(file, "\n");

    if (!template_only)
	WriteStructAssert(file, name, args, mask, simple, isuser, minsize, maxsize);
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
    /*
     * pad1 is an array as of #413 — one word on i386, two on x86-64, derived
     * from the width of an address so that every descriptor is one size.  A
     * braced zero initialises it whatever that count turns out to be, which
     * is what a generator wants: the template stops naming a width it cannot
     * see from here.
     */
    fprintf(file, "\t\t/* pad1 = */\t\t{0},\n");
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
 *
 * Note: On x86_64, va_list is an array type, so passing it to a function
 * passes by reference. This means that va_arg advances in this function
 * will be visible to the caller.
 */
static void
SkipVFPrintf(FILE *file, register char *fmt, va_list pvar)
{
    if (*fmt == 0)
        return; /* degenerate case */

    if (fmt[0] == '/' && fmt[1] == '*') {
        /* Format string begins with C comment.  Scan format
           string until end-comment delimiter, skipping the
           items in pvar that the enclosed format items would
           print. */

        register int c;

        fmt += 2;
        for (;;) {
            c = *fmt++;
            if (c == 0)
                return; /* nothing to format */
            if (c == '*') {
                if (*fmt == '/') {
                    break;
                }
            }
            else if (c == '%') {
                /* Field to skip */
                c = *fmt++;
                switch (c) {
                    case 's':
                        (void) va_arg(pvar, char *);
                        break;
                    case 'd':
                        (void) va_arg(pvar, int);
                        break;
                    case 'f':
                        (void) va_arg(pvar, double);
                        break;
                    case '\0':
                        return; /* error - fmt ends with '%' */
                    default:
                        break;
                }
            }
        }
        /* End of comment.  To be pretty, skip
           the space that follows. */
        fmt++;
        if (*fmt == ' ')
            fmt++;
    }

    /* Now format the string. */
    (void) vfprintf(file, fmt, pvar);
}

static void
vWriteCopyType(FILE *file, ipc_type_t *it, char *left, char *right, va_list pvar)
{
    if (it->itStruct)
    {
	fprintf(file, "\t");
	(void) SkipVFPrintf(file, left, pvar);
	fprintf(file, " = ");
	(void) SkipVFPrintf(file, right, pvar);
	fprintf(file, ";\n");
    }
    else if (it->itString)
    {
	fprintf(file, "\t(void) mig_strncpy(");
	(void) SkipVFPrintf(file, left, pvar);
	fprintf(file, ", ");
	(void) SkipVFPrintf(file, right, pvar);
	fprintf(file, ", %d);\n", it->itTypeSize);
    }
    else
    {
	fprintf(file, "\t{   typedef struct { char data[%d]; } *sp;\n",
		it->itTypeSize);
	fprintf(file, "\t    * (sp) ");
	(void) SkipVFPrintf(file, left, pvar);
	fprintf(file, " = * (sp) ");
	(void) SkipVFPrintf(file, right, pvar);
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
	    (void) SkipVFPrintf(file, left, pvar);
	    fprintf(file, ", ");
	    (void) SkipVFPrintf(file, right, pvar);
	    if (arg->argCount != argNULL && arg->argCount->argVarName != strNULL)
	        fprintf(file, ", %s);\n", arg->argCount->argVarName);
	    else
	        fprintf(file, ", 0 /* missing argCount */);\n");
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
    else {
	/*
	 * #443: say WHICH routine refused, and why.
	 *
	 * Every TypeCheck failure in a server stub funnels through here, and
	 * what it used to hand back was a bare MIG_BAD_ARGUMENTS: an error
	 * that surfaces far away, inside whichever task made the call, with
	 * nothing saying which check fired or on what.  A check nobody can
	 * diagnose is a check that gets switched off again the first time it
	 * fires -- which is how the kernel's came to be off in the first
	 * place.
	 *
	 * Userland servers get it as well.  They are the majority of the
	 * stubs and the half where these checks have always been on, so a
	 * silent rejection there has had far longer to hide than in the
	 * kernel; there is no argument for diagnosing the smaller half only.
	 *
	 * Costs nothing until it fires, and rate-limits itself so one bad
	 * sender in a loop cannot drown the console it is reporting to.
	 */
	fprintf(file, "MIG_CHECK_FAILED(\"%s\", \"%s\"); ",
		rt->rtName,
		(arg != argNULL) ? arg->argVarName : error);
        fprintf(file, "%sMIG_RETURN_ERROR(OutP, %s); }\n", string, error);
    }
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
