/*
 * Stub header for device_server.h
 * 
 * The device.defs file has multi-line type definitions that are not
 * compatible with the MIG preprocessor. Until this is fixed, we provide
 * stub declarations for the required symbols.
 *
 * TODO: Fix std_types.defs multi-line type definitions and regenerate
 * device_server.h properly from device.defs
 */

#ifndef _DEVICE_DEVICE_SERVER_H_
#define _DEVICE_DEVICE_SERVER_H_

#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/mig_errors.h>

/*
 * Stub subsystem declaration - this will be filled in when device.defs
 * is properly processed by MIG.
 */
extern struct mig_subsystem device_subsystem;

/*
 * Stub server routine - required by ipc_kobject.c
 */
extern boolean_t device_server(
    mach_msg_header_t *InHeadP,
    mach_msg_header_t *OutHeadP);

#endif /* _DEVICE_DEVICE_SERVER_H_ */
