/* Minimal compatibility header for machid library */
#ifndef _MACHID_LIB_H_
#define _MACHID_LIB_H_

#include <servers/machid_types.h>
#include <mach.h>

extern mach_port_t machid_server_port;
extern mach_port_t machid_auth_port;

#endif /* _MACHID_LIB_H_ */
