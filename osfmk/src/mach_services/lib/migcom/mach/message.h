/*
 * Minimal mach/message.h for building host tools (mig/migcom) on a
 * modern system without relying on the full exported Mach headers.
 *
 * Only the types and constants actually used by migcom are defined.
 */

#ifndef MIGCOM_LOCAL_MESSAGE_H_
#define MIGCOM_LOCAL_MESSAGE_H_

#include "boolean.h"

typedef unsigned int    mach_msg_bits_t;
typedef unsigned int    mach_msg_size_t;
typedef int             mach_msg_id_t;
typedef unsigned int    mach_port_t;

typedef unsigned int    mach_msg_type_name_t;

/* Some basic constants mig relies on (values are not critical for host tools). */
#define MACH_MSG_TYPE_MOVE_RECEIVE    16
#define MACH_MSG_TYPE_MOVE_SEND       17
#define MACH_MSG_TYPE_MOVE_SEND_ONCE  18
#define MACH_MSG_TYPE_COPY_SEND       19
#define MACH_MSG_TYPE_MAKE_SEND       20
#define MACH_MSG_TYPE_MAKE_SEND_ONCE  21
#define MACH_MSG_TYPE_COPY_RECEIVE    22

typedef unsigned int mach_msg_descriptor_type_t;

/* Descriptor types used by mig */
#define MACH_MSG_PORT_DESCRIPTOR		0
#define MACH_MSG_OOL_DESCRIPTOR		1
#define MACH_MSG_OOL_PORTS_DESCRIPTOR	2

/* Basic descriptor and body structures (simplified for host tools) */
typedef struct {
    mach_port_t name;
    mach_msg_size_t pad1;
    unsigned int pad2 : 16;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
} mach_msg_port_descriptor_t;

typedef struct {
    void *address;
    mach_msg_size_t size;
    unsigned int deallocate : 8;
    unsigned int copy : 8;
    unsigned int pad1 : 8;
    mach_msg_descriptor_type_t type : 8;
} mach_msg_ool_descriptor_t;

typedef struct {
    void *address;
    mach_msg_size_t count;
    unsigned int deallocate : 8;
    unsigned int copy : 8;
    mach_msg_type_name_t disposition : 8;
    mach_msg_descriptor_type_t type : 8;
} mach_msg_ool_ports_descriptor_t;

typedef union {
    mach_msg_port_descriptor_t port;
    mach_msg_ool_descriptor_t out_of_line;
    mach_msg_ool_ports_descriptor_t ool_ports;
    /* minimal type descriptor */
} mach_msg_descriptor_t;

typedef struct {
    mach_msg_size_t msgh_descriptor_count;
} mach_msg_body_t;

/* Message header used by mig */
typedef struct {
    mach_msg_bits_t msgh_bits;
    mach_msg_size_t msgh_size;
    mach_port_t msgh_remote_port;
    mach_port_t msgh_local_port;
    mach_msg_size_t msgh_reserved;
    mach_msg_id_t msgh_id;
} mach_msg_header_t;

/* Polymorphic message type */
#define MACH_MSG_TYPE_POLYMORPHIC	((mach_msg_type_name_t) -1)

/* Port type names */
#define MACH_MSG_TYPE_PORT_RECEIVE	0
#define MACH_MSG_TYPE_PORT_SEND		1
#define MACH_MSG_TYPE_PORT_SEND_ONCE	2
#define MACH_MSG_TYPE_PORT_NAME		15

/* Helper macros used by mig */
#define MACH_MSG_TYPE_PORT_ANY(x) \
    (((x) == MACH_MSG_TYPE_PORT_RECEIVE) || \
     ((x) == MACH_MSG_TYPE_PORT_SEND) || \
     ((x) == MACH_MSG_TYPE_PORT_SEND_ONCE) || \
     ((x) == MACH_MSG_TYPE_PORT_NAME))

#define MACH_MSG_TYPE_PORT_ANY_SEND(x) \
    (((x) == MACH_MSG_TYPE_PORT_SEND) || \
     ((x) == MACH_MSG_TYPE_PORT_SEND_ONCE))

#define MACH_MSG_TYPE_PORT_ANY_RIGHT(x) \
    (MACH_MSG_TYPE_PORT_ANY_SEND(x) || \
     ((x) == MACH_MSG_TYPE_PORT_RECEIVE))

#endif /* MIGCOM_LOCAL_MESSAGE_H_ */


