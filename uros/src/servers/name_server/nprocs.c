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
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS 
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
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mach_traps.h>	/* #716: urmach_cap_verify */
#include <mach/cap_types.h>	/* #716: a mount's capability */
#include <sa_mach.h>
#include <mach_error.h>

#include <servers/netname_defs.h>
#include "netname_server.h"
#include "notify_server.h"

extern void netname_init(void);

extern boolean_t Debug;
extern char *program;

extern mach_port_t notify_port;
extern mach_port_t netname_port;

typedef struct port_record {
    struct port_record *next;
    mach_port_t port;
    mach_port_urefs_t urefs;
} *port_record_t;

static port_record_t ports;

#define	MAX_UREFS	1000	/* max urefs we will hold for a port */
#define MORE_UREFS	1000	/* when we need urefs, how many to make */

static void
add_reference(mach_port_t port)
{
    port_record_t this, *last;
    mach_port_t previous;
    kern_return_t kr;

    if (!MACH_PORT_VALID(port))
	return;

    for (last = &ports, this = ports;
	 this != NULL;
	 last = &this->next, this = *last)
	if (this->port == port) {
	    /* we already know about this port */

	    if (++this->urefs > MAX_UREFS) {
		kr = mach_port_mod_refs(mach_task_self(), port,
					MACH_PORT_RIGHT_SEND, 1 - this->urefs);
		if (kr == KERN_SUCCESS)
		    this->urefs = 1;
		else if (kr == KERN_INVALID_RIGHT)
		    ; /* port is a dead name now */
		else {
		    panic("%s: add_reference: mach_port_mod_refs: %s\n",
			    program, mach_error_string(kr));
		}
	    }

	    /* remove the record and move it to the front */

	    *last = this->next;
	    goto insert;
	}

    /* we haven't seen this port before */

    this = (port_record_t) malloc(sizeof *this);
    this->port = port;
    this->urefs = 1;

    kr = mach_port_request_notification(mach_task_self(), port,
					MACH_NOTIFY_DEAD_NAME, TRUE,
					notify_port,
					MACH_MSG_TYPE_MAKE_SEND_ONCE,
					&previous);
    if ((kr != KERN_SUCCESS) || (previous != MACH_PORT_NULL)) {
	panic("%s: mach_port_request_notification: %s\n",
	      program, mach_error_string(kr));
    }

  insert:
    this->next = ports;
    ports = this;
}

static void
sub_reference(mach_port_t port)
{
    port_record_t this, *last;
    kern_return_t kr;

    if (!MACH_PORT_VALID(port))
	return;

    /* we must know about this port */

    for (last = &ports, this = ports;
	 this != NULL;
	 last = &this->next, this = *last)
	if (this->port == port)
	    break;

    /*
     * #574: and if it does not, it says so.  It was a mount whose server had
     * died: the dead-name notification took the port record and left the
     * mount, and the next look-up of the mount read this->next at address 0.
     * The mount goes with its server now; a record that outlives its port is
     * still a defect, and it is named here rather than read through.
     */
    if (this == NULL)
	panic("%s: sub_reference: port %x is in a record and not in the "
	      "port list (#574)\n", program, port);

    /* move it to the front */

    *last = this->next;
    this->next = ports;
    ports = this;

    /* make more user-references if we don't have enough */

    if (this->urefs == 1) {
	kr = mach_port_mod_refs(mach_task_self(), port,
				MACH_PORT_RIGHT_SEND, MORE_UREFS);
	if (kr == KERN_INVALID_RIGHT)
	    kr = mach_port_mod_refs(mach_task_self(), port,
				    MACH_PORT_RIGHT_DEAD_NAME, MORE_UREFS);
	if (kr != KERN_SUCCESS) {
	    panic("%s: mach_port_mod_refs: %s\n",
		  program, mach_error_string(kr));
	}

	this->urefs += MORE_UREFS;
    }

    this->urefs--;
}

static void
remove_port(mach_port_t port)
{
    port_record_t this, *last;
    kern_return_t kr;

    /* we must know about this port */

    for (last = &ports, this = ports;
	 this != NULL;
	 last = &this->next, this = *last)
	if (this->port == port)
	    break;

    /* deallocate the dead name */

    kr = mach_port_mod_refs(mach_task_self(), port,
			    MACH_PORT_RIGHT_DEAD_NAME, - this->urefs - 1);
    if (kr != KERN_SUCCESS) {
	panic("%s: mach_port_mod_refs: %s\n",
	      program, mach_error_string(kr));
    }

    /* remove the record */

    *last = this->next;
    free((char *) this);
}

/*
 *  We use a simple self-organizing list to keep track of
 *  registered ports.
 */

typedef struct name_record {
    struct name_record *next;
    netname_name_t key;
    mach_port_t name, sig;
} *name_record_t;

static name_record_t list;

/*
 * Watchers waiting for a service name to appear.
 * One-shot: removed after notification is sent.
 */
typedef struct watcher_record {
    struct watcher_record *next;
    netname_name_t key;
    mach_port_t notify;		/* send-once right */
} *watcher_record_t;

static watcher_record_t watchers;

static boolean_t
netname_find(netname_name_in_t key, name_record_t *thisp, name_record_t **prevp)
{
    register name_record_t *prev, this;

    for (this = *(prev = &list);
	 this != NULL;
	 this = *(prev = &this->next))
	if (strcmp(this->key, key) == 0) {
	    *thisp = this;
	    *prevp = prev;
	    return TRUE;
	}

    return FALSE;
}

void
netname_init(void)
{
    list = NULL;
    ports = NULL;
}

static void
netname_remove(mach_port_t name)
{
    register name_record_t *prev, this;

    if (Debug)
	printf("%s: netname_remove(%x)\n",
	       program, name);

    prev = &list;
    this = list;

    while (this != NULL)
	if ((this->name == name) ||
	    (this->sig == name)) {
	    register name_record_t last = this;

	    /* #574: what a server's death takes out of the registry, said. */
	    printf("%s: \"%.80s\" is gone with the port it named or was "
		   "signed by\n", program, last->key);
	    this = *prev = this->next;
	    free(last);
	} else
	    this = *(prev = &this->next);
}

/*
 * Send a one-shot notification to a watcher with the service port.
 * The notify port is a send-once right; it is consumed by the send.
 */
static void
notify_watcher(mach_port_t notify, mach_port_t service_port)
{
    netname_notify_msg_t msg;
    kern_return_t kr;

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) |
			 MACH_MSGH_BITS_COMPLEX;
    msg.head.msgh_size = sizeof(msg) - sizeof(mach_msg_trailer_t);
    msg.head.msgh_remote_port = notify;
    msg.head.msgh_local_port = MACH_PORT_NULL;
    msg.head.msgh_id = NETNAME_NOTIFY_MSID;

    msg.body.msgh_descriptor_count = 1;

    msg.service.name = service_port;
    msg.service.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.service.type = MACH_MSG_PORT_DESCRIPTOR;

    kr = mach_msg(&msg.head, MACH_SEND_MSG,
		  sizeof(msg) - sizeof(mach_msg_trailer_t),
		  0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS && Debug)
	printf("%s: notify_watcher: mach_msg: %s\n",
	       program, mach_error_string(kr));
}

/*
 * Notify all watchers waiting for a given service name.
 * Watchers are one-shot: removed from the list after notification.
 */
static void
notify_watchers_for(netname_name_in_t key, mach_port_t service_port)
{
    watcher_record_t *prev, this;

    prev = &watchers;
    this = watchers;

    while (this != NULL) {
	if (strcmp(this->key, key) == 0) {
	    watcher_record_t w = this;
	    this = *prev = this->next;

	    if (Debug)
		printf("%s: notifying watcher for '%.80s' port=%x\n",
		       program, key, w->notify);

	    notify_watcher(w->notify, service_port);
	    free(w);
	} else {
	    prev = &this->next;
	    this = this->next;
	}
    }
}

/*
 * Whether `sig' can sign for `port' (#744), for a name and for a mount (#716).
 *
 * A name is its server's because nobody else can present the signature it was
 * checked in with, and this is the check of the half of that the name server
 * can see.  MACH_PORT_NULL is what every task can present: four servers passed
 * it, so their names were anyone's to check in again or out, and ext_server
 * and proc_server passed it for every mount, "/" among them.  A dead name is
 * the same for everyone too.  And the port being registered cannot sign for
 * itself, since everyone who looks the name up holds a send right to it.
 *
 * ⚠️ The other half is the caller's: a port nobody else holds a send right to.
 * The name server cannot tell one from a port the caller shares, and a task
 * port signs as well as a private one while handing the name server the task --
 * urmach_netname_signature() is that half.
 */
static boolean_t
signature_can_sign(mach_port_t sig, mach_port_t port)
{
    return MACH_PORT_VALID(sig) && sig != port;
}

kern_return_t
do_netname_check_in(mach_port_t server, netname_name_in_t key,
		    mach_port_t sig, mach_port_t name)
{
    name_record_t *prev, this;

    if (Debug)
	printf("%s: netname_check_in(%.80s, name=%x, sig=%x)\n",
	       program, key, name, sig);

    /*
     * Refused before the name is looked for, so that a name nobody holds yet
     * cannot be taken without a signature either.  The rights in the request
     * go back with it: mach_msg_server() destroys a request answered with an
     * error.
     */
    if (!signature_can_sign(sig, name))
	return NETNAME_INVALID_PORT;

    if (netname_find(key, &this, &prev)) {
	if (this->sig == sig) {
	    this->name = name;
	    *prev = this->next;
	} else
	    return NETNAME_NOT_YOURS;
    } else {
	this = (name_record_t) malloc(sizeof *this);
	if (this == NULL)
	    return KERN_RESOURCE_SHORTAGE;

	this->name = name;
	this->sig = sig;
	/* ⚠️ strncpy does NOT terminate when the source fills the bound, so
	 * a bound equal to the destination leaves an unterminated key --
	 * and the strcmp that runs off it runs for the NEXT client, not
	 * for the one that sent the long name.  Same shape as the
	 * mount-prefix copy below, which already had it right. */
	(void) strncpy(this->key, key, sizeof(netname_name_t) - 1);
	this->key[sizeof(netname_name_t) - 1] = '\0';
    }

    this->next = list;
    list = this;

    /* we take responsibility for the refs in the request */

    add_reference(sig);
    add_reference(name);

    /* notify any watchers waiting for this service */
    notify_watchers_for(key, name);

    return NETNAME_SUCCESS;
}

kern_return_t
do_netname_register_send_right(mach_port_t server, netname_name_in_t key,
			       mach_port_t sig, mach_port_t name)
{
    return do_netname_check_in(server, key, sig, name);
}

kern_return_t
do_netname_look_up(mach_port_t server, netname_name_in_t host,
		   netname_name_in_t key, mach_port_t *namep)
{
    name_record_t *prev, this;

    if (netname_find(key, &this, &prev)) {
	mach_port_t name = this->name;

	*prev = this->next;
	this->next = list;
	list = this;

	if (Debug)
	    printf("%s: netname_look_up(%.80s) => %x\n",
		   program, key, name);

	/* return a ref for the port */

	sub_reference(*namep = name);
	return NETNAME_SUCCESS;
    } else {
	if (Debug)
	    printf("%s: netname_look_up(%.80s) failed\n",
		   program, key);

	return NETNAME_NOT_CHECKED_IN;
    }
}

kern_return_t
do_netname_check_out(mach_port_t server, netname_name_in_t key, mach_port_t sig)
{
    name_record_t *prev, this;

    if (Debug)
	printf("%s: netname_check_out(%.80s, sig=%x)\n",
	       program, key, sig);

    if (!signature_can_sign(sig, MACH_PORT_NULL))
	return NETNAME_INVALID_PORT;		/* #744 */

    if (netname_find(key, &this, &prev)) {
	if (this->sig == sig) {
	    *prev = this->next;
	    free((char *) this);

	    /* we take responsibility for the ref in the request */

	    add_reference(sig);
	    return NETNAME_SUCCESS;
	} else
	    return NETNAME_NOT_YOURS;
    } else
	return NETNAME_NOT_CHECKED_IN;
}

kern_return_t
do_netname_version(mach_port_t server, netname_name_t version)
{
    if (Debug)
	printf("%s: netname_version()\n", program);

    (void) strcpy(version, "Simple Name Service 3.0");
    return KERN_SUCCESS;
}

/* ================================================================
 * Mount registry (#220 sub-step C).  Separate keyspace from the
 * service-name table: keys are absolute path prefixes and lookup is
 * longest-segment-match instead of exact.  Used by libvfs to route
 * filesystem RPCs to the right fs_server.
 * ================================================================ */

typedef struct mount_record {
    struct mount_record *next;
    netname_name_t       prefix;
    mach_port_t          port;
    mach_port_t          sig;
} *mount_record_t;

static mount_record_t mounts;

/*
 * Segment-aware prefix match: prefix must end at a '/' boundary in
 * path so that "/mnt/d" does not match "/mnt/disk1".  Returns the
 * length of the matched prefix, or 0 if no match.
 */
static size_t
mount_prefix_match(const char *path, const char *prefix)
{
    size_t pl = strlen(prefix);
    size_t patl = strlen(path);

    if (pl > patl)
	return 0;
    if (strncmp(path, prefix, pl) != 0)
	return 0;
    /* Root prefix "/" matches every absolute path. */
    if (pl == 1 && prefix[0] == '/')
	return 1;
    /* Otherwise the next char in path must be either end-of-string
     * or the path separator, so /mnt/d does not match /mnt/disk1. */
    if (path[pl] == '\0' || path[pl] == '/')
	return pl;
    return 0;
}

/*
 * Whether `token' lets its bearer check a mount in under `prefix' (#716).
 *
 * The kernel verifies it: urmach_cap_verify() checks the MAC with the key
 * cap_server installed, the type, the id, the operation, and whether it was
 * revoked.  The id is the prefix's as the caller spelled it, so a capability
 * for "/mnt/disk2" is not one for "/mnt" or for "/etc".  The copy is because
 * the token arrives as bytes inside the message, which promises nothing of
 * the alignment a struct uros_cap wants.
 */
static kern_return_t
mount_capability(netname_name_in_t prefix, const char *token,
		 mach_msg_type_number_t count)
{
    struct uros_cap t;

    if (count != sizeof t)
	return CAP_ERR_INVALID_TOKEN;
    memcpy(&t, token, sizeof t);
    return urmach_cap_verify(&t, RESOURCE_MOUNT_POINT, CAP_OP_MOUNT_REGISTER,
			     cap_name_hash(prefix));
}

/*
 * The mounts of a port that died (#574): every record whose port or signature
 * is `name'.
 *
 * The dead-name notification removed a server's names and its port record,
 * and left its mounts: the next look-up of one handed sub_reference() a port
 * it no longer knew, and the name server died on a NULL.  And since #716 a
 * left record could not be checked out by anyone -- its signature was dead --
 * so a server started again could never mount its prefix either.
 */
static void
mount_remove(mach_port_t name)
{
    mount_record_t *prev = &mounts, this;

    while ((this = *prev) != NULL)
	if (this->port == name || this->sig == name) {
	    printf("%s: mount \"%.80s\" is gone with the port it named or "
		   "was signed by\n", program, this->prefix);
	    *prev = this->next;
	    free(this);
	} else
	    prev = &this->next;
}

kern_return_t
do_netname_check_in_mount(mach_port_t server, netname_name_in_t prefix,
			  mach_port_t sig, mach_port_t port,
			  cap_token_t token, mach_msg_type_number_t tokenCnt)
{
    mount_record_t this;
    kern_return_t kr;

    if (Debug)
	printf("%s: netname_check_in_mount('%.80s', port=%x, sig=%x)\n",
	       program, prefix, port, sig);

    /*
     * #716: a mount is its server's on the same terms as a name, and the
     * signature is asked before the prefix is looked at, as do_netname_check_in
     * asks it.  The rights in the request go back with the refusal.
     */
    if (!signature_can_sign(sig, port))
	return NETNAME_INVALID_PORT;

    /* Validate: must be absolute. */
    if (prefix[0] != '/')
	return NETNAME_NOT_YOURS;

    /*
     * #716: and the prefix is the caller's to mount under only with a
     * capability for it.  Asked before the registry is, so that a refusal
     * says nothing of whether the prefix is taken; the kernel's answer goes
     * back as it is, a CAP_ERR_* that names why.
     */
    kr = mount_capability(prefix, token, tokenCnt);
    if (kr != KERN_SUCCESS)
	return kr;

    /* Reject duplicates (caller should check_out_mount first). */
    for (this = mounts; this != NULL; this = this->next)
	if (strncmp(this->prefix, prefix, sizeof(this->prefix)) == 0)
	    return NETNAME_MOUNT_EXISTS;

    this = (mount_record_t) malloc(sizeof *this);
    if (this == NULL)
	return KERN_RESOURCE_SHORTAGE;
    this->port = port;
    this->sig  = sig;
    (void) strncpy(this->prefix, prefix, sizeof(this->prefix) - 1);
    this->prefix[sizeof(this->prefix) - 1] = '\0';
    this->next = mounts;
    mounts = this;

    add_reference(sig);
    add_reference(port);
    return NETNAME_SUCCESS;
}

kern_return_t
do_netname_check_out_mount(mach_port_t server, netname_name_in_t prefix,
			   mach_port_t sig)
{
    mount_record_t *prev, this;

    if (Debug)
	printf("%s: netname_check_out_mount('%.80s', sig=%x)\n",
	       program, prefix, sig);

    if (!signature_can_sign(sig, MACH_PORT_NULL))
	return NETNAME_INVALID_PORT;		/* #716 */

    for (prev = &mounts, this = mounts;
	 this != NULL;
	 prev = &this->next, this = *prev) {
	if (strncmp(this->prefix, prefix, sizeof(this->prefix)) != 0)
	    continue;
	if (this->sig != sig)
	    return NETNAME_NOT_YOURS;
	*prev = this->next;
	free((char *) this);
	add_reference(sig);
	return NETNAME_SUCCESS;
    }
    return NETNAME_NOT_CHECKED_IN;
}

kern_return_t
do_netname_look_up_mount(mach_port_t server, netname_path_in_t path,
			 mach_port_t *out_port, netname_name_t out_matched)
{
    mount_record_t this, best = NULL;
    size_t best_len = 0;

    /* Longest-prefix wins; first hit on equal length is fine since we
     * reject duplicates at check_in time. */
    for (this = mounts; this != NULL; this = this->next) {
	size_t mlen = mount_prefix_match(path, this->prefix);
	if (mlen > best_len) {
	    best     = this;
	    best_len = mlen;
	}
    }

    if (best == NULL) {
	if (Debug)
	    printf("%s: netname_look_up_mount('%.80s'): no match\n",
		   program, path);
	out_matched[0] = '\0';
	return NETNAME_NO_MOUNT;
    }

    if (Debug)
	printf("%s: netname_look_up_mount('%.80s') => '%.80s' port=%x\n",
	       program, path, best->prefix, best->port);

    sub_reference(*out_port = best->port);
    (void) strncpy(out_matched, best->prefix, sizeof(netname_name_t) - 1);
    out_matched[sizeof(netname_name_t) - 1] = '\0';
    return NETNAME_SUCCESS;
}

/*
 * #284: enumerate every mount prefix into a single newline-separated
 * buffer.  proc_server's shutdown drain splits the result and re-resolves
 * each prefix via netname_look_up_mount to fs_sync it.  If the registry
 * outgrows the buffer we stop early and report the count we managed to
 * emit — a partial sync is still better than none on the way down.
 */
kern_return_t
do_netname_list_mounts(mach_port_t server, netname_path_t out_prefixes,
		       int *out_count)
{
    mount_record_t this;
    size_t off = 0;
    int n = 0;
    const size_t cap = sizeof(netname_path_t);

    out_prefixes[0] = '\0';
    for (this = mounts; this != NULL; this = this->next) {
	size_t pl = strlen(this->prefix);
	/* prefix + '\n' + terminating NUL must fit. */
	if (off + pl + 2 > cap)
	    break;
	memcpy(out_prefixes + off, this->prefix, pl);
	off += pl;
	out_prefixes[off++] = '\n';
	n++;
    }
    out_prefixes[off] = '\0';
    *out_count = n;

    if (Debug)
	printf("%s: netname_list_mounts => %d mount(s)\n", program, n);
    return NETNAME_SUCCESS;
}

kern_return_t
do_mach_notify_port_deleted(mach_port_t notify, mach_port_t name)
{
    panic("%s: do_mach_notify_port_deleted\n", program);
    return KERN_FAILURE;
}

kern_return_t
do_mach_notify_port_destroyed(mach_port_t notify, mach_port_t port)
{
    panic("%s: do_mach_notify_port_destroyed\n", program);
    return KERN_FAILURE;
}

kern_return_t
do_mach_notify_no_senders(mach_port_t notify, mach_port_mscount_t mscount)
{
    panic("%s: do_mach_notify_no_senders\n", program);
    return KERN_FAILURE;
}

kern_return_t
do_mach_notify_send_once(mach_port_t notify)
{
    panic("%s: do_mach_notify_send_once\n", program);
    return KERN_FAILURE;
}

kern_return_t
do_mach_notify_dead_name(mach_port_t notify, mach_port_t name)
{
    if (Debug)
	printf("%s: do_mach_notify_dead_name: %x\n", program, name);

    netname_remove(name);
    mount_remove(name);		/* #574 */
    remove_port(name);
    return KERN_SUCCESS;
}

kern_return_t
do_netname_notify(mach_port_t server, netname_name_in_t key,
		  mach_port_t notify)
{
    name_record_t *prev, this;

    if (Debug)
	printf("%s: netname_notify(%.80s, notify=%x)\n",
	       program, key, notify);

    /* If the service is already registered, notify immediately */
    if (netname_find(key, &this, &prev)) {
	mach_port_t service = this->name;

	*prev = this->next;
	this->next = list;
	list = this;

	notify_watcher(notify, service);
	return NETNAME_SUCCESS;
    }

    /* Service not yet available — add to watcher list */
    {
	watcher_record_t w;

	w = (watcher_record_t) malloc(sizeof *w);
	if (w == NULL)
	    return KERN_RESOURCE_SHORTAGE;

	(void) strncpy(w->key, key, sizeof(netname_name_t) - 1);
	w->key[sizeof(netname_name_t) - 1] = '\0';
	w->notify = notify;
	w->next = watchers;
	watchers = w;
    }

    return NETNAME_SUCCESS;
}

kern_return_t
do_netname_debug_on(mach_port_t server)
{
    printf("%s: do_netname_debug_on\n", program);
    Debug = TRUE;
    return KERN_SUCCESS;
}

kern_return_t
do_netname_debug_off(mach_port_t server)
{
    printf("%s: do_netname_debug_off\n", program);
    Debug = FALSE;
    return KERN_SUCCESS;
}

