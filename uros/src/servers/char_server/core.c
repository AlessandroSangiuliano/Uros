/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/core.c — resource registry + module orchestration.
 *
 * Mirror gpu_server/core.c.  Single-threaded (mach_msg_server is the
 * only writer); modules' IRQ paths run on threads they spawn
 * themselves and only call back via mach_msg sends to subscribers.
 */

#include <mach.h>
#include <mach/cap_types.h>
#include <stdio.h>
#include <string.h>
#include <servers/netname.h>	/* netname_look_up, name_server_port (#365) */
#include <vt/vt_ipc.h>		/* vt_switch_msg_t, VT_SWITCH_MSGH_ID (#365) */
#include "char_server.h"
#include "device_master.h"	/* MIG: device_intr_{register,unregister} */

#include <mach/mach_traps.h>	/* the traps, declared once (#426) */

/* ============================================================
 * Device table
 * ============================================================ */

static struct char_device_entry devices[CHAR_MAX_DEVICES];
static unsigned int             n_devices;

static struct char_device_entry *
alloc_dev_slot(void)
{
	unsigned int i;
	for (i = 1; i < CHAR_MAX_DEVICES; i++) {	/* slot 0 = invalid */
		if (!devices[i].in_use) {
			memset(&devices[i], 0, sizeof(devices[i]));
			devices[i].in_use = 1;
			devices[i].id     = (char_dev_id_t)i;
			return &devices[i];
		}
	}
	return NULL;
}

struct char_device_entry *
char_core_dev_lookup(char_dev_id_t id)
{
	if (id == 0 || id >= CHAR_MAX_DEVICES)
		return NULL;
	if (!devices[id].in_use)
		return NULL;
	return &devices[id];
}

/*
 * Reverse lookup: find the device a module instance belongs to.  Module
 * ops only ever receive their own priv pointer, but the line discipline
 * (#397) needs the device entry to reach its controlling-terminal owner.
 */
struct char_device_entry *
char_core_dev_by_priv(void *priv)
{
	char_dev_id_t i;

	if (priv == NULL)
		return NULL;
	for (i = 1; i < CHAR_MAX_DEVICES; i++)
		if (devices[i].in_use && devices[i].priv == priv)
			return &devices[i];
	return NULL;
}

unsigned int
char_core_dev_count(void)
{
	return n_devices;
}

int
char_core_dev_copy_all(struct char_device_info *out, unsigned int max)
{
	unsigned int i, n = 0;
	for (i = 1; i < CHAR_MAX_DEVICES && n < max; i++) {
		if (devices[i].in_use)
			out[n++] = devices[i].info;
	}
	return (int)n;
}

/* ============================================================
 * Discovery
 * ============================================================ */

void
char_core_run_discovery(const char_module_ops_t * const *modules,
			unsigned int n_modules,
			mach_port_t hal_port)
{
	unsigned int m;

	(void)hal_port;	/* future modules (usb_hid_kbd) will probe via HAL */

	if (modules == NULL || n_modules == 0) {
		printf("char_server: no back-end modules loaded — discovery "
		       "skipped\n");
		return;
	}

	for (m = 0; m < n_modules; m++) {
		const char_module_ops_t *ops = modules[m];

		if (ops == NULL)
			continue;
		if (ops->abi_version != CHAR_MODULE_ABI_VERSION) {
			printf("char_server: rejecting module \"%s\" — ABI "
			       "%u != %u\n",
			       ops->name ? ops->name : "(unnamed)",
			       (unsigned)ops->abi_version,
			       (unsigned)CHAR_MODULE_ABI_VERSION);
			continue;
		}
		if (ops->probe == NULL)
			continue;

		/*
		 * Probe repeatedly: a module may expose more than one device.
		 * The console TTY hands out one instance per virtual terminal
		 * (#365).  Single-instance modules (ps2, uart, ...) return NULL
		 * on the second probe — their attach flips an "attached" guard —
		 * so the loop ends after one device for them.
		 */
		for (;;) {
			void *priv;
			struct char_device_entry *dev;

			priv = ops->probe(NULL);
			if (priv == NULL)
				break;		/* no (more) instances */

			dev = alloc_dev_slot();
			if (dev == NULL) {
				printf("char_server: device table full, dropping "
				       "\"%s\"\n", ops->name);
				if (ops->detach)
					ops->detach(priv);
				break;
			}

			dev->module = ops;
			dev->priv   = priv;
			dev->info.id    = dev->id;
			dev->info.class = ops->device_class;
			dev->info.flags = 0;
			strncpy(dev->info.module_name, ops->name,
				CHAR_DEV_NAME_LEN - 1);
			dev->info.module_name[CHAR_DEV_NAME_LEN - 1] = '\0';

			if (ops->attach != NULL && ops->attach(priv) < 0) {
				printf("char_server: module \"%s\" attach "
				       "failed\n", ops->name);
				dev->in_use = 0;
				break;
			}

			n_devices++;
			printf("char_server: device %u attached "
			       "(module=\"%s\", class=%u)\n",
			       (unsigned)dev->id, ops->name,
			       (unsigned)dev->info.class);
		}
	}
}

/* ============================================================
 * Capability check.  Empty tokens rejected — every char_server entry
 * point is cap-gated, no early-boot exception path (cap_server is
 * always up before any client could plausibly want to talk to us:
 * char_server boots after cap_server in bootstrap.conf).
 * ============================================================ */

int
char_core_cap_check(const char *token, unsigned int token_count,
		    uint64_t op, uint64_t resource_id)
{
	struct uros_cap cap;
	kern_return_t kr;

	if (token == NULL || token_count != sizeof(struct uros_cap))
		return -1;

	memcpy(&cap, token, sizeof(cap));
	kr = urmach_cap_verify(&cap, (uint32_t)op, resource_id);
	if (kr != KERN_SUCCESS) {
		printf("char_server: cap_verify FAIL "
		       "(op=0x%llx res=0x%llx kr=%d)\n",
		       (unsigned long long)op,
		       (unsigned long long)resource_id, (int)kr);
		return -1;
	}
	return 0;
}

/* ============================================================
 * Init
 * ============================================================ */

int
char_core_init(void)
{
	memset(devices, 0, sizeof(devices));
	n_devices = 0;
	return 0;
}

/* ============================================================
 * IRQ table — wires module handlers to char_server's port set.
 *
 * Same convention block_device_server uses: an IRQ notification
 * arrives as a Mach message with msgh_id >= IRQ_NOTIFY_MSGH_BASE.
 * We allocate one receive port per registered IRQ, drop it into the
 * shared port_set, and on dispatch route by msgh_local_port.
 *
 * Single-threaded: install + dispatch both run on char_server's main
 * thread, no locking needed.
 * ============================================================ */

#define IRQ_NOTIFY_MSGH_BASE	3000	/* matches kernel's irq notif id */
#define CHAR_MAX_IRQ		16

struct char_irq_slot {
	int		in_use;
	uint32_t	irq;
	mach_port_t	port;
	void		(*handler)(void *);
	void		*arg;
};

static struct char_irq_slot	irq_slots[CHAR_MAX_IRQ];
static unsigned int		n_irq_slots;
static mach_port_t		irq_master_device;
static mach_port_t		irq_port_set;
static int			irq_initialised;

int
char_core_irq_init(mach_port_t master_device, mach_port_t port_set)
{
	if (irq_initialised)
		return 0;
	irq_master_device = master_device;
	irq_port_set      = port_set;
	memset(irq_slots, 0, sizeof(irq_slots));
	n_irq_slots = 0;
	irq_initialised = 1;
	return 0;
}

/*
 * #382: forward a console break (Ctrl+D spotted by uart.so / ps2.so) to
 * the kernel debugger.  The RPC blocks this dispatch thread for the whole
 * DDB session — intended: nothing char_server-side should move while the
 * operator pokes around.  Returns 0 when the kernel took the break
 * (byte consumed), -1 when the -K flag isn't armed (deliver the byte as
 * ordinary input).
 */
int
char_core_ddb_break(void)
{
	if (!irq_initialised || irq_master_device == MACH_PORT_NULL)
		return -1;
	if (device_ddb_break(irq_master_device) != KERN_SUCCESS)
		return -1;
	return 0;
}

int
char_core_irq_register(uint32_t irq, void (*handler)(void *), void *arg)
{
	struct char_irq_slot *slot = NULL;
	mach_port_t port = MACH_PORT_NULL;
	kern_return_t kr;
	unsigned int i;

	if (!irq_initialised || handler == NULL)
		return -1;

	for (i = 0; i < CHAR_MAX_IRQ; i++) {
		if (irq_slots[i].in_use && irq_slots[i].irq == irq) {
			printf("char_server: IRQ %u already registered\n", irq);
			return -1;
		}
		if (slot == NULL && !irq_slots[i].in_use)
			slot = &irq_slots[i];
	}
	if (slot == NULL)
		return -1;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS) {
		printf("char_server: IRQ %u port alloc failed (kr=%d)\n",
		       irq, kr);
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(), port, port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		(void)mach_port_destroy(mach_task_self(), port);
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(), port, irq_port_set);

	kr = device_intr_register(irq_master_device, irq, port,
				  MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("char_server: device_intr_register(%u) failed "
		       "(kr=%d)\n", irq, kr);
		(void)mach_port_destroy(mach_task_self(), port);
		return -1;
	}

	slot->in_use  = 1;
	slot->irq     = irq;
	slot->port    = port;
	slot->handler = handler;
	slot->arg     = arg;
	n_irq_slots++;

	printf("char_server: IRQ %u registered (port=0x%x)\n",
	       irq, (unsigned)port);
	return 0;
}

int
char_core_irq_unregister(uint32_t irq)
{
	unsigned int i;

	for (i = 0; i < CHAR_MAX_IRQ; i++) {
		if (!irq_slots[i].in_use || irq_slots[i].irq != irq)
			continue;
		(void)device_intr_unregister(irq_master_device, irq);
		(void)mach_port_destroy(mach_task_self(), irq_slots[i].port);
		memset(&irq_slots[i], 0, sizeof(irq_slots[i]));
		n_irq_slots--;
		return 0;
	}
	return -1;
}

boolean_t
char_core_dispatch_irq(mach_msg_header_t *in)
{
	unsigned int i;

	/* IRQ notifications carry msgh_id == IRQ_NOTIFY_MSGH_BASE + irq.
	 * Kernel uses irq in 0..15, so anything outside this narrow range
	 * is a MIG RPC (char_server.defs starts at 4100) and must fall
	 * through to the regular demux. */
	if (in->msgh_id < IRQ_NOTIFY_MSGH_BASE ||
	    in->msgh_id >= IRQ_NOTIFY_MSGH_BASE + 16)
		return FALSE;

	for (i = 0; i < CHAR_MAX_IRQ; i++) {
		if (!irq_slots[i].in_use)
			continue;
		if (irq_slots[i].port != in->msgh_local_port)
			continue;
		(*irq_slots[i].handler)(irq_slots[i].arg);
		/*
		 * Re-unmask the line at the PIC after the module-side
		 * handler has cleared the device status (#222).  No-op for
		 * edge-triggered lines (PS/2, COM) but uniform across
		 * trigger modes.
		 */
		(void)device_intr_enable(irq_master_device, irq_slots[i].irq);
		return TRUE;
	}
	/* Stray IRQ notification (slot torn down between the kernel
	 * sending it and us draining it).  Swallow silently. */
	return TRUE;
}

/* ============================================================
 * Keyboard → console loopback (#363).
 *
 * The on-screen console is a CHAR_CLASS_TTY module that lives in this
 * very process, so it cannot be an ordinary external keyboard
 * subscriber.  Instead core allocates one receive port, drops it on
 * the shared port set, and hands a send right to every keyboard
 * module through its kbd_subscribe op.  ps2.so then fans each key
 * event to that port exactly as it would to a remote client; the
 * events arrive on our own port set, char_demux spots msgh_id
 * CHAR_KBD_EVENT_MSGH_ID and calls the sink the console registered.
 *
 * Human typing is low-frequency, so the kernel round-trip per
 * keystroke is free; the win is that ps2.so stays entirely unaware it
 * is talking to an in-process consumer.
 * ============================================================ */

static void		(*kbd_sink)(void *arg, const char_kbd_event_t *ev);
static void		*kbd_sink_arg;
static mach_port_t	kbd_loopback_port;

void
char_core_register_kbd_sink(void (*fn)(void *, const char_kbd_event_t *),
			    void *arg)
{
	kbd_sink     = fn;
	kbd_sink_arg = arg;
}

boolean_t
char_core_dispatch_kbd_event(mach_msg_header_t *in)
{
	const char_kbd_event_msg_t *msg;

	if (in->msgh_id != CHAR_KBD_EVENT_MSGH_ID)
		return FALSE;

	/* Route to the console sink if one registered; drop otherwise
	 * (no console module loaded → nobody to hand the event to). */
	if (kbd_sink != NULL) {
		msg = (const char_kbd_event_msg_t *)in;
		kbd_sink(kbd_sink_arg, &msg->event);
	}
	return TRUE;
}

/*
 * Drop the controlling-tty binding for a session that has exited (#365).
 * A ctty is bound at tty_acquire_ctty and, until now, never cleared: a
 * respawned shell trying to re-claim the same VT hit the bound-tty cap
 * gate (KERN_PROTECTION_FAILURE).  proc_server tells us the session is
 * gone (see char_core_dispatch_ctty_release), and we free every device it
 * owned so the VT is claimable again.
 */
void
char_core_release_ctty(int sid)
{
	unsigned int i;

	if (sid == 0)
		return;
	for (i = 1; i < CHAR_MAX_DEVICES; i++) {
		if (devices[i].in_use && devices[i].ctty_sid == sid) {
			devices[i].ctty_sid = 0;
			printf("char_server: ctty released for sid=%d "
			       "(dev %u free)\n", sid, i);
		}
	}
}

boolean_t
char_core_dispatch_ctty_release(mach_msg_header_t *in)
{
	const char_ctty_release_msg_t *msg;

	if (in->msgh_id != CHAR_CTTY_RELEASE_MSGH_ID)
		return FALSE;

	msg = (const char_ctty_release_msg_t *)in;
	char_core_release_ctty(msg->sid);
	return TRUE;
}

/*
 * Tell the virtual_terminal_server the on-screen VT changed, so it can
 * start a shell there lazily if none runs yet (#365 phase 3).  The console
 * module calls this from its key sink on Ctrl+Alt+Fn.  The server port is
 * looked up once and cached; a send failure (server restarted) drops the
 * cache so the next switch re-resolves.  One-way, best-effort — on a
 * headless / no-console build the lookup just fails and switches are moot.
 */
static mach_port_t vt_server_port = MACH_PORT_NULL;

void
char_core_notify_vt_switch(uint32_t surface)
{
	vt_switch_msg_t   m;
	kern_return_t     kr;

	if (vt_server_port == MACH_PORT_NULL) {
		char host[80] = "";
		char serv[80] = VT_SERVICE_NAME;

		kr = netname_look_up(name_server_port, host, serv,
				     &vt_server_port);
		if (kr != NETNAME_SUCCESS) {
			vt_server_port = MACH_PORT_NULL;
			return;
		}
	}

	memset(&m, 0, sizeof m);
	m.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.head.msgh_size        = sizeof m;
	m.head.msgh_remote_port = vt_server_port;
	m.head.msgh_id          = VT_SWITCH_MSGH_ID;
	m.surface               = (int32_t)surface;

	kr = mach_msg(&m.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m,
		      0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS && kr != MACH_SEND_TIMED_OUT) {
		(void)mach_port_deallocate(mach_task_self(), vt_server_port);
		vt_server_port = MACH_PORT_NULL;
	}
}

int
char_core_kbd_loopback_wire(void)
{
	kern_return_t kr;
	mach_port_t   send;
	unsigned int  i, wired = 0;

	if (!irq_initialised)		/* port set lives in the IRQ block */
		return -1;
	if (kbd_sink == NULL)		/* no console attached → nothing to do */
		return 0;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &kbd_loopback_port);
	if (kr != KERN_SUCCESS) {
		printf("char_server: kbd loopback port alloc failed (kr=%d)\n",
		       (int)kr);
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(), kbd_loopback_port,
				    kbd_loopback_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		(void)mach_port_destroy(mach_task_self(), kbd_loopback_port);
		kbd_loopback_port = MACH_PORT_NULL;
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(), kbd_loopback_port,
				    irq_port_set);
	send = kbd_loopback_port;

	for (i = 1; i < CHAR_MAX_DEVICES; i++) {
		struct char_device_entry *dev = &devices[i];

		if (!dev->in_use || dev->module == NULL)
			continue;
		if (dev->info.class != CHAR_CLASS_KEYBOARD)
			continue;
		if (dev->module->kbd_subscribe == NULL)
			continue;
		if (dev->module->kbd_subscribe(dev->priv, send) == 0)
			wired++;
	}

	printf("char_server: keyboard->console loopback wired "
	       "(%u keyboard%s)\n", wired, wired == 1 ? "" : "s");
	return (wired > 0) ? 0 : -1;
}
