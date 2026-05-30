/* Minimal header to declare shared machid globals used by libmachid sources */
#ifndef _MACHID_LIB_H_
#define _MACHID_LIB_H_

#include <mach.h>

extern mach_port_t machid_server_port;
extern mach_port_t machid_auth_port;

#endif /* _MACHID_LIB_H_ */
