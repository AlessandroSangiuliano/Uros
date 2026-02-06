/*
 * Stub header for device_server.h
 * The real device_server.h should be generated from device.defs by MIG.
 * This is a temporary placeholder until the MIG parsing issues are resolved.
 */

#ifndef _DEVICE_DEVICE_SERVER_STUB_H_
#define _DEVICE_DEVICE_SERVER_STUB_H_

#include <mach/kern_return.h>
#include <mach/message.h>

/* Stub: device server demux function */
static inline boolean_t
device_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP)
{
    /* Not implemented - device subsystem disabled */
    return FALSE;
}

#endif /* _DEVICE_DEVICE_SERVER_STUB_H_ */
