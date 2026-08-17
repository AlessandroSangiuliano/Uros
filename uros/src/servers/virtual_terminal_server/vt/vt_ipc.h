/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * vt/vt_ipc.h — char_server ↔ virtual_terminal_server protocol (#365).
 *
 * The virtual_terminal_server owns the shells on the on-screen virtual
 * terminals; char_server owns the keyboard and consoles and therefore is
 * the one that sees a VT switch (Ctrl+Alt+Fn).  When the user switches to
 * a virtual terminal, char_server sends this one-way message to the
 * virtual_terminal_server, which lazily starts a shell there if none is
 * running yet — a terminal only gets a shell the first time it is looked
 * at, the way a lever engages a gear only when thrown.
 *
 * A single hand-built Mach message, no MIG: the sender drops it and moves
 * on, the server multiplexes it against the shell-exit notifications on
 * one port set.
 */

#ifndef _VT_VT_IPC_H_
#define _VT_VT_IPC_H_

#include <mach/message.h>

/* name_server registration the virtual_terminal_server checks in under and
 * char_server looks up to reach it. */
#define VT_SERVICE_NAME		"virtual_terminal_server"

/*
 * msgh_id char_server stamps on the VT-switch notification.  Chosen clear
 * of the gpu (4000) / char (4100) MIG subsystems, the kbd fan-out (4200)
 * and the ctty-release id (4201), so the server can route by a single id
 * compare against the shell-exit message it also receives.
 */
#define VT_SWITCH_MSGH_ID	4300

typedef struct vt_switch_msg {
	mach_msg_header_t	head;
	NDR_record_t		ndr;
	int32_t			surface;	/* VT surface switched to */
} vt_switch_msg_t;

#endif /* _VT_VT_IPC_H_ */
