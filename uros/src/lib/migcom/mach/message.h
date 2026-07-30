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

/*
 * There used to be host-side definitions of the descriptors, the body and
 * the header here, with the address fields declared uint32_t so that the
 * host would measure them and get the i386 answer.  A comment explained the
 * trick and pointed at #416 as the place where migcom would learn what it
 * was generating for.
 *
 * It has learned: every width now comes from the target model in target.c,
 * and nothing in this tool measures a host type to describe a message.  So
 * the definitions are gone rather than merely unused.  A structure shaped
 * like the target, sitting in a header named message.h, answers sizeof()
 * with a plausible number for whoever asks next -- and plausible is exactly
 * how the i386 width survived thirty years without being questioned.
 *
 * What is left is what migcom actually reads: the names of the descriptor
 * kinds, which are wire constants and the same on every target.
 */

/* Polymorphic message type */
#define MACH_MSG_TYPE_POLYMORPHIC	((mach_msg_type_name_t) -1)

/* Port type names - aliases to MOVE_* equivalents (as in original Mach) */
#define MACH_MSG_TYPE_PORT_RECEIVE	MACH_MSG_TYPE_MOVE_RECEIVE
#define MACH_MSG_TYPE_PORT_SEND		MACH_MSG_TYPE_MOVE_SEND
#define MACH_MSG_TYPE_PORT_SEND_ONCE	MACH_MSG_TYPE_MOVE_SEND_ONCE
#define MACH_MSG_TYPE_PORT_NAME		15

/* Helper macros used by mig - same as kernel's mach/message.h */
/* Uses range check: MOVE_RECEIVE(16) to MAKE_SEND_ONCE(21) */
/* PORT_NAME(15) is NOT included - it's just a name, not a right */
#define MACH_MSG_TYPE_PORT_ANY(x)			\
	(((x) >= MACH_MSG_TYPE_MOVE_RECEIVE) &&		\
	 ((x) <= MACH_MSG_TYPE_MAKE_SEND_ONCE))

#define MACH_MSG_TYPE_PORT_ANY_SEND(x)			\
	(((x) >= MACH_MSG_TYPE_MOVE_SEND) &&		\
	 ((x) <= MACH_MSG_TYPE_MAKE_SEND_ONCE))

#define MACH_MSG_TYPE_PORT_ANY_RIGHT(x) \
    (MACH_MSG_TYPE_PORT_ANY_SEND(x) || \
     ((x) == MACH_MSG_TYPE_PORT_RECEIVE))

#endif /* MIGCOM_LOCAL_MESSAGE_H_ */


