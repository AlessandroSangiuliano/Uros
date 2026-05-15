#ifndef	_netname_server_
#define	_netname_server_

/* Module netname */

#include <string.h>
#include <mach/ndr.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/notify.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/rpc.h>
#include <mach/port.h>

#include <mach/std_types.h>
#include <servers/netname_defs.h>

/* Routine netname_check_in */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_check_in
#if	defined(LINTLIBRARY)
    (server_port, port_name, signature, port_id)
	mach_port_t server_port;
	netname_name_t port_name;
	mach_port_t signature;
	mach_port_t port_id;
{ return do_netname_check_in(server_port, port_name, signature, port_id); }
#else
(
	mach_port_t server_port,
	netname_name_t port_name,
	mach_port_t signature,
	mach_port_t port_id
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_look_up */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_look_up
#if	defined(LINTLIBRARY)
    (server_port, host_name, port_name, port_id)
	mach_port_t server_port;
	netname_name_t host_name;
	netname_name_t port_name;
	mach_port_t *port_id;
{ return do_netname_look_up(server_port, host_name, port_name, port_id); }
#else
(
	mach_port_t server_port,
	netname_name_t host_name,
	netname_name_t port_name,
	mach_port_t *port_id
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_check_out */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_check_out
#if	defined(LINTLIBRARY)
    (server_port, port_name, signature)
	mach_port_t server_port;
	netname_name_t port_name;
	mach_port_t signature;
{ return do_netname_check_out(server_port, port_name, signature); }
#else
(
	mach_port_t server_port,
	netname_name_t port_name,
	mach_port_t signature
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_version */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_version
#if	defined(LINTLIBRARY)
    (server_port, version)
	mach_port_t server_port;
	netname_name_t version;
{ return do_netname_version(server_port, version); }
#else
(
	mach_port_t server_port,
	netname_name_t version
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_register_send_right */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_register_send_right
#if	defined(LINTLIBRARY)
    (server_port, port_name, signature, port_id)
	mach_port_t server_port;
	netname_name_t port_name;
	mach_port_t signature;
	mach_port_t port_id;
{ return do_netname_register_send_right(server_port, port_name, signature, port_id); }
#else
(
	mach_port_t server_port,
	netname_name_t port_name,
	mach_port_t signature,
	mach_port_t port_id
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_debug_on */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_debug_on
#if	defined(LINTLIBRARY)
    (server_port)
	mach_port_t server_port;
{ return do_netname_debug_on(server_port); }
#else
(
	mach_port_t server_port
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_debug_off */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_debug_off
#if	defined(LINTLIBRARY)
    (server_port)
	mach_port_t server_port;
{ return do_netname_debug_off(server_port); }
#else
(
	mach_port_t server_port
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_notify */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_notify
#if	defined(LINTLIBRARY)
    (server_port, port_name, notify_port)
	mach_port_t server_port;
	netname_name_t port_name;
	mach_port_t notify_port;
{ return do_netname_notify(server_port, port_name, notify_port); }
#else
(
	mach_port_t server_port,
	netname_name_t port_name,
	mach_port_t notify_port
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_check_in_mount */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_check_in_mount
#if	defined(LINTLIBRARY)
    (server_port, prefix, signature, port_id)
	mach_port_t server_port;
	netname_name_t prefix;
	mach_port_t signature;
	mach_port_t port_id;
{ return do_netname_check_in_mount(server_port, prefix, signature, port_id); }
#else
(
	mach_port_t server_port,
	netname_name_t prefix,
	mach_port_t signature,
	mach_port_t port_id
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_check_out_mount */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_check_out_mount
#if	defined(LINTLIBRARY)
    (server_port, prefix, signature)
	mach_port_t server_port;
	netname_name_t prefix;
	mach_port_t signature;
{ return do_netname_check_out_mount(server_port, prefix, signature); }
#else
(
	mach_port_t server_port,
	netname_name_t prefix,
	mach_port_t signature
);
#endif	/* defined(LINTLIBRARY) */

/* Routine netname_look_up_mount */
#ifdef	mig_external
mig_external
#else
extern
#endif	/* mig_external */
kern_return_t do_netname_look_up_mount
#if	defined(LINTLIBRARY)
    (server_port, path, port_id, matched)
	mach_port_t server_port;
	netname_path_t path;
	mach_port_t *port_id;
	netname_name_t matched;
{ return do_netname_look_up_mount(server_port, path, port_id, matched); }
#else
(
	mach_port_t server_port,
	netname_path_t path,
	mach_port_t *port_id,
	netname_name_t matched
);
#endif	/* defined(LINTLIBRARY) */

extern boolean_t netname_server(
		mach_msg_header_t *InHeadP,
		mach_msg_header_t *OutHeadP);

extern mig_routine_t netname_server_routine(
		mach_msg_header_t *InHeadP);


/* Description of this subsystem, for use in direct RPC */
extern struct do_netname_subsystem {
	struct subsystem *	subsystem;	/* Reserved for system use */
	mach_msg_id_t	start;	/* Min routine number */
	mach_msg_id_t	end;	/* Max routine number + 1 */
	unsigned int	maxsize;	/* Max msg size */
	vm_address_t	base_addr;	/* Base ddress */
	struct routine_descriptor	/*Array of routine descriptors */
		routine[11];
	struct routine_arg_descriptor	/*Array of arg descriptors */
		arg_descriptor[33];
} do_netname_subsystem;


#endif	 /* _netname_server_ */
