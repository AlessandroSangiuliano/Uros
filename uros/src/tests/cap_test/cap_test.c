/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 * cap_test — negative tests for the UrMach capability fast path.
 *
 * Issue B installs a trust-on-first-use (TOFU) identity in the kernel:
 * the first task that calls urmach_cap_register with a setup token
 * (cap_id == 0) becomes the trusted cap_server, and all later register
 * calls from other tasks must fail with CAP_ERR_UNAUTHORIZED.
 *
 * The bootstrap launches tasks in parallel, so this test must wait
 * until cap_server has already registered before calling
 * urmach_cap_register itself — otherwise we'd win the TOFU race and
 * wrongly brick the whole capability subsystem for the boot.  We use
 * cap_server's netname registration as the synchronization barrier:
 * cap_server only calls netname_check_in *after* its own successful
 * urmach_cap_register, so seeing the port in the name server means the
 * key slot is taken.
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/mig_errors.h>	/* #443: MIG_BAD_ARGUMENTS, mig_reply_error_t */
#include <mach/mach_syscalls.h>	/* #472: trap 100, called directly */
#include <mach/rpc.h>		/* #478: struct rpc_subsystem */
#include <mach/bootstrap.h>
#include <mach/mach_host.h>	/* #546: vm_wire, vm_remap */
#include <mach/sync.h>		/* #552: semaphore_create/signal/wait */
#include <mach/sync_policy.h>	/* #552: SYNC_POLICY_FIFO */
#include <mach/clock.h>		/* #708: host_get_clock_service, clock_get_time */
#include <mach/urmach_futex.h>	/* #708: the futex's three timed waits */
#include <pthread.h>		/* #708: [31]'s sender */
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <device/device.h>
#include <device/device_types.h>
#include "device_master.h"	/* #432: a device has one driver */
#include "ahci_batch.h"	/* #432: a disk reading into a page it does not own */
#include <libcap.h>
#include "sha256.h"     /* Issue #180: SHA-NI dispatch query + KAT */

/*
 * The cap traps come from <mach/mach_traps.h>, included above (#426).
 *
 * ⚠️ This file used to declare two of them here as well -- with the header
 * already on the include list, four lines above -- and one of the two was
 * WRONG: `struct uros_cap *' where the kernel takes a const pointer.  A local
 * extern does not merely duplicate a header, it OVERRIDES the question: the
 * other half of a trap is an assembly stub, which carries no types, so nothing
 * compared either declaration with the kernel.  One of twenty such private
 * declarations in the tree.
 */

/*
 * #395 probe — XMM survival across the kernel cap-verify path.
 *
 * The kernel computes the capability HMAC with SHA-NI, which executes XMM
 * instructions, in a kernel built -mno-sse under a lazy-FPU (CR0.TS)
 * discipline with no save/restore around that use.  Suspicion: a cap-verify
 * syscall silently clobbers the calling thread's XMM registers.
 *
 * These helpers move a 128-byte pattern into/out of xmm0..xmm7 around a
 * single trap.  They are the only userland XMM between fill and read: the
 * trap stubs are libmach assembly (no compiler codegen), so any change to
 * the registers across the call happened in the kernel.  noinline keeps the
 * compiler from scheduling its own code inside the window.
 */
static void __attribute__((noinline))
xmm_fill(const uint8_t p[128])
{
    __asm__ volatile(
        "movdqu   0(%0), %%xmm0\n\t"
        "movdqu  16(%0), %%xmm1\n\t"
        "movdqu  32(%0), %%xmm2\n\t"
        "movdqu  48(%0), %%xmm3\n\t"
        "movdqu  64(%0), %%xmm4\n\t"
        "movdqu  80(%0), %%xmm5\n\t"
        "movdqu  96(%0), %%xmm6\n\t"
        "movdqu 112(%0), %%xmm7\n\t"
        : : "r"(p)
        : "xmm0", "xmm1", "xmm2", "xmm3",
          "xmm4", "xmm5", "xmm6", "xmm7");
}

static void __attribute__((noinline))
xmm_read(uint8_t p[128])
{
    __asm__ volatile(
        "movdqu %%xmm0,   0(%0)\n\t"
        "movdqu %%xmm1,  16(%0)\n\t"
        "movdqu %%xmm2,  32(%0)\n\t"
        "movdqu %%xmm3,  48(%0)\n\t"
        "movdqu %%xmm4,  64(%0)\n\t"
        "movdqu %%xmm5,  80(%0)\n\t"
        "movdqu %%xmm6,  96(%0)\n\t"
        "movdqu %%xmm7, 112(%0)\n\t"
        : : "r"(p) : "memory");
}

extern kern_return_t bootstrap_ports(mach_port_t bootstrap,
                                     mach_port_t *host_port,
                                     mach_port_t *device_port,
                                     mach_port_t *root_ledger_wired,
                                     mach_port_t *root_ledger_paged,
                                     mach_port_t *security_port);

extern kern_return_t netname_look_up(mach_port_t server_port,
                                     char *host_name,
                                     char *service_name,
                                     mach_port_t *service_port);

extern void printf_init(mach_port_t device_server_port);

/*
 * Spin on netname_look_up until cap_server appears, yielding the CPU
 * between tries so the scheduler can actually run cap_server.  No
 * upper bound here — a missing cap_server means boot is broken and the
 * QEMU timeout will surface the failure clearly.
 */
static mach_port_t
wait_for_cap_server(void)
{
    mach_port_t port = MACH_PORT_NULL;
    for (;;) {
        kern_return_t kr = netname_look_up(name_server_port, "",
                                           (char *)"cap_server", &port);
        if (kr == KERN_SUCCESS)
            return port;
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
    }
}

/*
 * Whether the partition behind `part_port` is the boot disk: the one the
 * block server published as "disk0a", which bootstrap stage 2 loads servers
 * from.  1 if it is, 0 if disk0a is another partition, -1 if disk0a is not
 * registered.
 *
 * 🔴 Asked, not read off the candidate list.  Which controller the block
 * server numbers first depends on which controllers are attached and in which
 * PCI slots (#592): virtio-blk under run-x86_64.sh, AHCI under run-qemu.sh,
 * virtio-blk again under run-qemu.sh --virtio-first.  This file
 * used to take the first candidate, 'virtio_blk0a', for the boot disk, so on
 * i386 arm [14] wrote its scratch block into the real boot disk, ahci0a,
 * which the arm exists to leave alone.
 *
 * The block server checks both names in with the same receive right, and a
 * task holds one name for all its send rights to one port, so equal names
 * mean the same partition.
 */
static int
is_the_boot_disk(mach_port_t part_port)
{
    mach_port_t boot = MACH_PORT_NULL;
    int         same;

    if (netname_look_up(name_server_port, "", (char *)"disk0a", &boot)
        != KERN_SUCCESS)
        return -1;
    same = boot == part_port;
    (void)mach_port_deallocate(mach_task_self(), boot);
    return same;
}

/*
 * Test [6]: a complex message the kernel must refuse halfway (#442).
 *
 * ipc_kmsg_copyin_body translates the descriptors one at a time and calls
 * ipc_kmsg_clean_partial when one of them is bad, to give back the rights
 * it has already taken.  Nothing in this tree exercised that path: a boot
 * plus the whole benchmark suite reaches it zero times, so any claim about
 * it was a claim about code that had never run.
 *
 * The message below carries two port descriptors.  The first names a port
 * this task really owns, so the kernel acquires a right for it; the second
 * names nothing, so the translation fails with the first already done.
 *
 * What is being checked is not the error code alone -- it is that the port
 * survives.  If the unwind released the first right twice the port would be
 * gone, and if it released it not at all the right would leak; sending on
 * the same port afterwards and receiving that message back says the count
 * came home.
 */
static int
bad_descriptor_unwind(void)
{
    typedef struct {
        mach_msg_header_t          head;
        mach_msg_body_t            body;
        mach_msg_port_descriptor_t good;
        mach_msg_port_descriptor_t bad;
    } bad_msg_t;

    typedef struct {
        mach_msg_header_t   head;
        mach_msg_trailer_t  trailer;
    } plain_recv_t;

    mach_port_t   port = MACH_PORT_NULL;
    bad_msg_t     msg;
    plain_recv_t  back;
    kern_return_t kr;
    int           ok = 1;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [6] port allocate FAIL kr=%d\n", (int)kr);
        return 0;
    }
    kr = mach_port_insert_right(mach_task_self(), port, port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [6] insert right FAIL kr=%d\n", (int)kr);
        return 0;
    }

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
                         MACH_MSGH_BITS_COMPLEX;
    msg.head.msgh_size        = sizeof(msg);
    msg.head.msgh_remote_port = port;
    msg.head.msgh_local_port  = MACH_PORT_NULL;
    msg.head.msgh_id          = 442;
    msg.body.msgh_descriptor_count = 2;

    msg.good.name        = port;
    msg.good.disposition = MACH_MSG_TYPE_MAKE_SEND;
    msg.good.type        = MACH_MSG_PORT_DESCRIPTOR;

    /* A name no task holds: the generation bits alone make it impossible. */
    msg.bad.name         = (mach_port_t) 0xdeadbeefu;
    msg.bad.disposition  = MACH_MSG_TYPE_COPY_SEND;
    msg.bad.type         = MACH_MSG_PORT_DESCRIPTOR;

    kr = mach_msg(&msg.head, MACH_SEND_MSG, sizeof(msg), 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr == MACH_MSG_SUCCESS) {
        printf("cap_test: [6] bad descriptor was ACCEPTED — FAIL\n");
        return 0;
    }
    printf("cap_test: [6] bad descriptor rejected kr=0x%x\n", (unsigned)kr);

    /* The port must still be usable, exactly once. */
    memset(&back, 0, sizeof(back));
    back.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    back.head.msgh_size        = sizeof(back.head);
    back.head.msgh_remote_port = port;
    back.head.msgh_local_port  = MACH_PORT_NULL;
    back.head.msgh_id          = 443;

    kr = mach_msg(&back.head, MACH_SEND_MSG, sizeof(back.head), 0,
                  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != MACH_MSG_SUCCESS) {
        printf("cap_test: [6] port did not survive the unwind kr=0x%x\n",
               (unsigned)kr);
        ok = 0;
    } else {
        kr = mach_msg(&back.head, MACH_RCV_MSG, 0, sizeof(back),
                      port, 2000, MACH_PORT_NULL);
        if (kr != MACH_MSG_SUCCESS || back.head.msgh_id != 443) {
            printf("cap_test: [6] follow-up receive FAIL kr=0x%x id=%d\n",
                   (unsigned)kr, (int)back.head.msgh_id);
            ok = 0;
        } else {
            printf("cap_test: [6] port survived the unwind: OK\n");
        }
    }

    (void)mach_port_destroy(mach_task_self(), port);
    return ok;
}

/*
 * [7]/[8] (#443): the MIG argument checks fire, and say who refused.
 *
 * The suites have been reporting "zero checks fired" ever since these were
 * compiled back in, and that number was worth nothing: an observation that
 * has never been shown able to see a firing cannot be read as evidence that
 * none happened.  Nothing in the tree sends a malformed message, so the
 * whole diagnostic path -- the check, the rate-limited counter, the printf
 * -- had never once executed.  These two cases execute it deliberately.
 *
 * The trick is which size the stub actually reads.  Not the msgh_size the
 * caller writes into the header: ipc_kmsg_get overwrites that with the
 * send_size argument of mach_msg (ipc_kmsg.c, `kmsg->ikm_header.msgh_size =
 * size`).  So the malformed message is made by lying to mach_msg about how
 * much to send, not by lying in the header.
 *
 * Both sides of the tree are covered on purpose, because they are compiled
 * differently: the kernel's 18 server stubs had TypeCheck off from the port
 * until this issue, userland's 179 have always had it on and until now
 * rejected in silence.
 */
/*
 * #472.  Trap 100 took a port NAME from ring 3 and used it as a pointer.
 *
 * `syscall_mach_port_allocate_subsystem' handed its second argument straight
 * to convert_port_to_subsystem() as an ipc_port_t, which locks it and reads
 * ip_kotype() out of it -- so the kernel wrote to and read from an address
 * the caller chose, and only afterwards decided whether it liked what it
 * found.  IP_VALID(), the only guard on the road, rejects 0 and ~0.
 *
 * ⚠️ Tested HERE, on i386, and that is the point rather than convenience:
 * the routine is machine-independent and the defect is exactly as present on
 * the mature target.  What the 64-bit build added was only the report -- a
 * name is 32 bits and a pointer is 64, so the cast finally warned.
 *
 * ⚠️ And the trap is called directly rather than through libmach, because
 * libmach does not call it: mach_port_allocate_subsystem() goes to the
 * neighbouring trap instead.  Nothing in userland reaches this one, which is
 * precisely the situation in which a hole stays open -- an attacker does not
 * use libmach either.
 *
 * Two values, because they fail two different checks and only both together
 * say the argument is being TRANSLATED rather than merely screened:
 *
 *   - a number that is not a name in this space at all.  The lookup must
 *     refuse it at the table bounds or the generation.
 *   - a name that IS ours and is emphatically not a subsystem.  The lookup
 *     must get as far as the port and refuse it on its kind.  A guard that
 *     only rejected implausible numbers would pass the first and fail this.
 */
static int
subsystem_name_is_not_a_pointer(void)
{
    /*
     * Not zero and not ~0, which IP_VALID() already rejected, so a pass here
     * cannot be the old guard doing its old job.  Past any plausible port
     * table, so the lookup refuses it on the index.
     */
    const mach_port_t   not_a_name = (mach_port_t) 0x0FFFFFF0u;
    mach_port_t         name = MACH_PORT_NULL;
    kern_return_t       kr;
    int                 ok = 1;

    kr = syscall_mach_port_allocate_subsystem(mach_task_self(), not_a_name,
                                              &name);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [9] trap 100 accepted 0x%x as a subsystem — WRONG\n",
               (unsigned) not_a_name);
        (void) mach_port_destroy(mach_task_self(), name);
        ok = 0;
    } else {
        printf("cap_test: [9] trap 100 refused a number that is not a name: "
               "kr=%d\n", (int) kr);
    }

    name = MACH_PORT_NULL;
    kr = syscall_mach_port_allocate_subsystem(mach_task_self(),
                                              mach_task_self(), &name);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [9] trap 100 accepted the task port as a "
               "subsystem — WRONG\n");
        (void) mach_port_destroy(mach_task_self(), name);
        ok = 0;
    } else {
        printf("cap_test: [9] trap 100 refused a real name that is not a "
               "subsystem: kr=%d\n", (int) kr);
    }

    return ok;
}

/*
 * #478.  A subsystem must survive an allocation that fails.
 *
 * `syscall_mach_port_allocate_full' released a subsystem reference on every
 * path, and mach_port_allocate_full() takes the matching one only INSIDE its
 * `if (kr == KERN_SUCCESS)'.  So a failing allocation decremented a count
 * that had never been incremented.  A subsystem is created with a count of
 * one, which means one call: it reached zero, the subsystem was freed, and
 * the port that names it was left pointing into the hole.
 *
 * Nothing privileged is needed to make the allocation fail -- `qos.name'
 * with a name that already denotes a right returns KERN_NAME_EXISTS, and
 * the name is chosen in ring 3.
 *
 * ⚠️ The trap is called directly rather than through libmach, and here that
 * is not only the #472 reason.  libmach's mach_port_allocate_full() falls
 * back to the MIG route on MACH_SEND_INTERRUPTED, which is exactly what a
 * destroyed subsystem produces -- and the MIG route would then allocate
 * happily with no subsystem at all and report success.  The fallback would
 * convert the finding into a pass.
 *
 * ⚠️ The subsystem is probed BEFORE the failing call as well as after it.
 * The whole test rests on reading "the subsystem is gone" out of a refusal
 * from trap 100, and a refusal that was never shown able to be an
 * acceptance is not evidence of anything.  The first probe is what makes
 * the second one mean something.
 *
 * The port from the first probe is destroyed straight away, and that is
 * load-bearing rather than tidiness: destroying the receive right releases
 * the reference mach_port_allocate_full() took for it, which puts the count
 * back to one on both kernels.  Leave it alive and the broken kernel goes
 * from two to one instead of from one to zero, and passes.
 */

/*
 * ── A device has one driver, and this task is not it (#432) ──────────
 *
 * 🔴 THE MASTER DEVICE PORT USED TO BE THE WHOLE ANSWER.  Any task holding it
 * could name any bus/device/function and have memory mapped into THAT device's
 * IOMMU domain -- and a page inside somebody else's device's domain is a page
 * that DEVICE can reach, which is the one direction nothing else checks.  This
 * program holds the master port and is not the block server, so it is the task
 * that can prove the refusal.
 *
 * 🔴 AND IT ASKS BEFORE IT TRIES.  A device is claimed by the FIRST task that
 * maps for it, so "try it and see" would STEAL any device nobody had claimed
 * yet, and the driver that was going to claim it would be refused afterwards
 * -- a self-test breaking the thing it checks.  device_dma_owned() answers the
 * question without claiming, and exists for this.
 *
 * ⚠️ A boot where nothing is claimed yet says NOT ASKED (#591; it said DID NOT
 * RUN, which no harness counted).  The order between this task and the block
 * server is an interleaving and not an order (#425); silence would be
 * indistinguishable from a pass.
 */

/*
 * ── A disk reads into a page belonging to a task with no device ──────
 *
 * 🔴 THIS IS THE `DEVICE_DMA_NO_BDF' HOLE, EXERCISED.  ext_server allocates a
 * page cache and hands the block server the PHYSICAL addresses of it, because
 * it owns no device and has no bus/device/function to name.  Once the disk is
 * confined to what it was granted, those pages are in nobody's domain and the
 * transfer is refused -- correct, diagnosable, and the filesystem stops.
 *
 * So this does exactly what ext_server does, in a test: allocate scatter-
 * gather pages with DEVICE_DMA_NO_BDF, hand their addresses to a disk, and
 * require the bytes to arrive.  What makes it work is the block server
 * translating them through the kernel first -- the server that OWNS the device
 * doing the mapping, which is the shape #432 asked for.
 *
 * 🔑 THE ORACLE IS NOT OURS.  0xEF53 at offset 1080 is the ext2 superblock
 * magic, put there by mke2fs on the host when the image was built.  A test
 * that checked a pattern it wrote itself would pass on a read that returned
 * the buffer unchanged.
 */

/*
 * ── The manifest refuses what it does not declare (#432) ─────────────
 *
 * 🔴 THE FIRST TIME THIS SYSTEM'S POLICY FILE HAS EVER BEEN READ.  The
 * manifest machinery has existed since #216 -- a compiler, a validator, a
 * per-task table, bootstrap's lookup of "<symtab>.cmf" in the bundle -- and
 * not one .cmf was ever compiled or shipped, so cap_manifest_allows()
 * answered "permissive, no manifest" on every boot of this system's life and
 * the enforcement branch in cap_server called itself dead in its own comment.
 *
 * cap_test.manifest declares RESOURCE_BLK_DEVICE and nothing else.  This
 * program legitimately opens partitions with block-device capabilities --
 * arms [3] and [5] do -- and has no business driving PCI hardware: it is a
 * test, not a driver.  So asking for a PCI capability must come back DENIED,
 * and the denial is the policy being enforced rather than described.
 *
 * 🔑 WHICH IS THE OTHER HALF OF ARM [11].  That one shows the kernel refusing
 * a device somebody else drives; this one shows cap_server refusing the
 * capability that would have let this task claim one at all.  Together they
 * answer "who may drive this device": the manifest says what KIND, the claim
 * says which INSTANCE, and neither is a policy alone.
 *
 * ⚠️ A boot where this task has no manifest is WRONG, by the branch below
 * that names the PCI capability: cap_server is permissive without one, so
 * that token comes back too.  And the declared capability refused is WRONG
 * as well -- a server that refuses everything is not a policy.  This comment
 * said both ended in DID NOT RUN; the second did, and returned success
 * (#591).
 */
/*
 * ── [15] The master device port is a way around the manifest (#511) ──────
 *
 * 🔴 THIS TASK'S OWN POLICY FILE SAYS IT MAY NOT DRIVE PCI HARDWARE, and
 * says so on purpose: cap_test.manifest carries the line
 *
 *	# required 5 0x010601 0x7    RESOURCE_PCI_DEVICE, SATA AHCI, ...
 *
 * commented out, with "its absence here is the policy" written beside it.
 * Arm [13] above reads that refusal back from cap_server and passes.
 *
 * 🔑 And then this arm does the thing anyway, because holding the master
 * device port is enough.  A device is addressed by three integers -- bus,
 * slot, function -- which is a NAME and not a right, and `device_mmio_map'
 * takes a physical address with nothing to establish that it names a device
 * rather than somebody's memory.  The policy is not wrong; it is bypassed.
 *
 * 🔥 THE VALUE IS THE ORACLE.  Scanning configuration space for a class this
 * task is forbidden and then mapping the BAR it finds could still be argued
 * to have landed somewhere harmless.  So the arm reads the controller's
 * version register through the mapping and requires the number the AHCI
 * driver -- another task, which does hold that device -- printed for itself
 * earlier in the same boot.  A matching version says the mapping reached
 * that device's registers and not a plausible-looking page.
 *
 * ⚠️ Read-only, and one page.  What is being shown is that the range can be
 * reached at all; writing another server's controller registers would prove
 * the same thing and break the boot doing it.
 *
 * ⚠️ IT WAS WRITTEN TO BE INVERTED once #511 was fixed, and only its first
 * question was: the refused read of 0:0.0 below passes as CLOSED.  Past it,
 * the rest stayed the demonstration it was before the fix, and returned
 * success when the bypass worked (#511 OPEN) or could not be shown (DID NOT
 * RUN) -- #511 reopened would have passed.  Every way past that read is WRONG
 * now, and the lines after the WRONG one say how far the bypass reaches
 * (#591).
 */
#define PCI_CFG_VENDOR		0x00
#define PCI_CFG_CLASS		0x08
#define PCI_CFG_BAR5		0x24
#define AHCI_CLASS_SATA		0x0106		/* class 01, subclass 06 */
#define AHCI_VS_REG		0x10

static int
the_master_port_bypasses_the_manifest(mach_port_t device_port)
{
    natural_t    bus, slot, func, data = 0;
    natural_t    found_bus = 0, found_slot = 0, found_func = 0;
    natural_t    bar5 = 0;
    vm_address_t uva = 0;
    int          found = 0;
    unsigned     version;

    /*
     * Configuration space, read by name.
     *
     * 🔴 THE REFUSAL HAS TO BE TOLD FROM AN EMPTY BUS.  This arm first
     * reported "no SATA controller in configuration space" when every read
     * was being denied -- a true sentence about what it saw and a useless
     * one about why, and it PASSED while proving nothing.  A guard that
     * cannot fail is worse than none, and this one had quietly become one
     * the moment the thing it tests started working.
     */
    {
        kern_return_t probe = device_pci_config_read(device_port, 0, 0, 0,
                                                     PCI_CFG_VENDOR, &data);

        if (probe == KERN_NO_ACCESS) {
            printf("cap_test: [15] #511 CLOSED (configuration space) — this "
                   "task holds no device and no bus, and reading 0:0.0 was "
                   "refused: a device is addressed by a right and not by "
                   "three integers\n");
            return 1;
        }
        printf("cap_test: [15] WRONG — #511 reopened: reading 0:0.0 was not "
               "refused (kr=%d) to a task that holds no device and no bus\n",
               (int)probe);
    }

    for (bus = 0; bus < 1 && !found; bus++)
        for (slot = 0; slot < 32 && !found; slot++)
            for (func = 0; func < 8 && !found; func++) {
                if (device_pci_config_read(device_port, bus, slot, func,
                                           PCI_CFG_VENDOR, &data)
                    != KERN_SUCCESS)
                    continue;
                if (data == 0xFFFFFFFFu || data == 0)
                    continue;
                if (device_pci_config_read(device_port, bus, slot, func,
                                           PCI_CFG_CLASS, &data)
                    != KERN_SUCCESS)
                    continue;
                if ((data >> 16) != AHCI_CLASS_SATA)
                    continue;
                found_bus = bus; found_slot = slot; found_func = func;
                found = 1;
            }

    if (!found) {
        printf("cap_test: [15] #511 — no SATA controller in configuration "
               "space to show how far the reopened read reaches\n");
        return 0;
    }

    if (device_pci_config_read(device_port, found_bus, found_slot, found_func,
                               PCI_CFG_BAR5, &bar5) != KERN_SUCCESS) {
        printf("cap_test: [15] #511 — BAR5 of %u:%u.%u unreadable\n",
               (unsigned)found_bus, (unsigned)found_slot,
               (unsigned)found_func);
        return 0;
    }
    bar5 &= ~0xFu;

    if (bar5 == 0) {
        printf("cap_test: [15] #511 — BAR5 of %u:%u.%u is zero\n",
               (unsigned)found_bus, (unsigned)found_slot,
               (unsigned)found_func);
        return 0;
    }

    if (device_mmio_map(device_port, (vm_address_t)bar5, 4096,
                        mach_task_self(), &uva) != KERN_SUCCESS || uva == 0) {
        printf("cap_test: [15] #511 — configuration space is open, and the "
               "mapping of %u:%u.%u BAR5 (0x%08X) was refused\n",
               (unsigned)found_bus, (unsigned)found_slot,
               (unsigned)found_func, (unsigned)bar5);
        return 0;
    }

    version = *(volatile unsigned *)(uva + AHCI_VS_REG);

    if (version == 0x00010000u || version == 0x00010100u
        || version == 0x00010200u || version == 0x00010300u) {
        printf("cap_test: [15] #511 OPEN — this task declares no PCI device "
               "and read version 0x%08X out of %u:%u.%u BAR5 (0x%08X), which "
               "another server holds: the master port is a way around the "
               "manifest\n",
               version, (unsigned)found_bus, (unsigned)found_slot,
               (unsigned)found_func, (unsigned)bar5);
    } else {
        printf("cap_test: [15] #511 — the mapping succeeded but 0x%08X is no "
               "AHCI version, so it is NOT evidence about where it landed\n",
               version);
    }

    (void)device_mmio_unmap(device_port, uva, 4096, mach_task_self());
    return 0;
}

static int
the_manifest_refuses_what_it_does_not_declare(void)
{
    struct uros_cap tok_blk, tok_pci;
    kern_return_t   kr_blk, kr_pci;

    memset(&tok_blk, 0, sizeof(tok_blk));
    memset(&tok_pci, 0, sizeof(tok_pci));

    /*
     * ⚠️ THE DECLARED ONE FIRST, and it is not a formality.  A denial proves
     * the policy is enforced only if something is also ALLOWED -- otherwise
     * "cap_server said no" is indistinguishable from cap_server being absent,
     * broken, or refusing everything.  One yes and one no from the same
     * server, in the same breath, is what makes the no mean something.
     */
    kr_blk = cap_request(RESOURCE_BLK_DEVICE, 0,
                         CAP_OP_BLK_READ | CAP_OP_BLK_WRITE, 0, &tok_blk);

    kr_pci = cap_request(RESOURCE_PCI_DEVICE, 0x010601,
                         CAP_OP_PCI_DMA_MAP, 0, &tok_pci);

    if (kr_blk != KERN_SUCCESS) {
        printf("cap_test: [13] WRONG — cap_server would not issue even the "
               "declared block-device capability (kr=%d), so its refusal of "
               "the PCI one proves nothing\n", (int)kr_blk);
        return 0;
    }

    if (kr_pci == KERN_SUCCESS) {
        printf("cap_test: [13] WRONG — cap_server issued a PCI-device "
               "capability to a task whose manifest declares only block "
               "devices\n");
        return 0;
    }

    printf("cap_test: [13] block devices ALLOWED and PCI devices REFUSED "
           "(kr=%d) by the same cap_server in the same breath — the manifest "
           "is a policy and not a description\n", (int)kr_pci);
    return 1;
}

static int
a_disk_reads_into_a_foreign_page(mach_port_t device_port, mach_port_t handle,
                                 const char *name)
{
    kern_return_t          kr;
    vm_address_t           kva = 0, uva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    io_buf_len_t           got = 0;
    uint64_t               region_id = 0;
    unsigned               magic;
    int                    ok = 0;

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (kr != KERN_SUCCESS || pa_cnt != 1) {
        printf("cap_test: [12] a disk reads into a foreign page — DID NOT "
               "RUN, no scatter-gather page (kr=%d)\n", (int)kr);
        if (pa_list != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                                pa_cnt * sizeof(vm_address_t));
        return 1;
    }

    /*
     * ── The consent, which is the whole of #432's second gap ─────────
     *
     * 🔴 KNOWING THE ADDRESS IS NOT BEING GIVEN THE BUFFER.  The block server
     * is about to program this page's physical address into a disk, and the
     * kernel will only map it for that disk if somebody holding a capability
     * for the buffer asks.  This task owns the region, so cap_server will
     * issue it one -- after asking the KERNEL whether this task really is the
     * owner, which is the half a policy file cannot know.
     *
     * ⚠️ And then it is HANDED OVER.  The block server keeps the capability,
     * not the buffer: a delegation of use, revocable by this task without the
     * server being asked.
     */
    {
        struct uros_cap buf_cap;

        memset(&buf_cap, 0, sizeof(buf_cap));
        kr = cap_request(RESOURCE_DMA_BUFFER, region_id,
                         CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE,
                         0, &buf_cap);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [12] WRONG — cap_server would not issue a "
                   "capability for region %llu this task owns (kr=%d)\n",
                   (unsigned long long)region_id, (int)kr);
            goto out;
        }

        kr = device_register_dma(handle, (char *)&buf_cap, sizeof(buf_cap));
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [12] WRONG — the block server would not take "
                   "the capability (kr=%d)\n", (int)kr);
            goto out;
        }
    }

    /*
     * ⚠️ Wiped first, so "the read did nothing" cannot pass.  A page that
     * arrives holding what the allocator left in it is a page nobody wrote.
     */
    memset((void *)uva, 0, 4096);

    kr = device_read_phys(handle, D_READ, 0, 4096, pa_list, pa_cnt, &got);

    magic = (unsigned)(((unsigned char *)uva)[1080])
          | ((unsigned)(((unsigned char *)uva)[1081]) << 8);

    if (kr == KERN_SUCCESS && magic == 0xEF53u) {
        printf("cap_test: [12] %s DMA'd into a page this task owns and no "
               "device was granted — 0x%x at +1080, the superblock magic "
               "mke2fs wrote: the server that owns the disk mapped it\n",
               name, magic);
        ok = 1;
    } else {
        printf("cap_test: [12] WRONG — read of %s into a foreign page "
               "answered kr=%d, %u bytes, and +1080 holds 0x%x rather than "
               "0xef53\n", name, (int)kr, (unsigned)got, magic);
    }

out:
    (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                        pa_cnt * sizeof(vm_address_t));
    return ok;
}

/*
 * ── [14] The pages and the byte count are two arguments, and nobody made
 *    them agree (#532) ──────────────────────────────────────────────────
 *
 * A zero-copy read carries a list of physical pages and a count of bytes.
 * Those are two independent numbers from two independent arguments, and
 * `n * 4096` has to be at least the second of them.  Nothing checked it: a
 * one-page list with eight thousand bytes asked for was accepted, the drive
 * was told to move sixteen sectors, and the descriptor table described four.
 *
 * 🔑 The arm runs on EVERY partition it is given rather than on the first
 * one that answers.  That is not thoroughness for its own sake: the disk
 * arms have always run on the preferred disk, which is virtio-blk, so the
 * AHCI physical entry points have been in the driver's ops table since they
 * were written with no client ever reaching them.  A refusal that exists in
 * one driver and not the other is exactly the drift this is here to catch.
 *
 * ⚠️ The covered read runs FIRST and must succeed.  Without it, a server
 * that refused every physical read for some unrelated reason would pass the
 * second half, and the arm would be reporting a broken path as a working
 * check.
 */
/*
 * A block inside every candidate partition, for the reads a write-only handle
 * must be refused ([27]): the last 4096 bytes of a 30720-sector partition,
 * the smallest candidate (x86-64's AHCI test disks).  The writes no longer
 * use it: they go to the last 4096 bytes of the partition scratch_target()
 * chose, whatever its size (#596).
 */
#define SCRATCH_BLOCK	30712u

/*
 * ── #596: where the writes may go ──────────────────────────────────────
 *
 * 🔴 NOT "IS THIS THE BOOT DISK?" ALONE.  [14]'s write half and [27]'s
 * offered writes asked only that (#592), and on x86-64 the partition they
 * wrote was ahci0a: ext_server's root, mounted by that name on every full
 * boot.  The property that matters is whether anybody else holds the
 * partition, and the block server, which keeps every handle with its
 * partition and mode, answers it (DEV_GET_HOLDERS).  A partition somebody
 * else holds -- a filesystem's, a pager's -- gets no write; the same disk's
 * partition c gets them instead when it is registered and nobody holds it
 * (x86-64's first AHCI disk has one for this, make-disk-x86_64.sh);
 * otherwise nothing does, and the line says why.
 */

/* The other handles on the partition behind `port', and of those the ones
 * opened for writing: 1 when the block server answered, 0 when it did not. */
static int
ct_holders(mach_port_t port, unsigned *open, unsigned *writing)
{
    int                    st[DEV_GET_HOLDERS_COUNT];
    mach_msg_type_number_t cnt = DEV_GET_HOLDERS_COUNT;

    if (device_get_status(port, DEV_GET_HOLDERS, (dev_status_t)st, &cnt)
            != KERN_SUCCESS || cnt < DEV_GET_HOLDERS_COUNT)
        return 0;
    *open = (unsigned)st[DEV_GET_HOLDERS_OPEN];
    *writing = (unsigned)st[DEV_GET_HOLDERS_WRITING];
    return 1;
}

/* The first sector of the partition's last 4096 bytes, asked through `port'
 * (DEV_GET_SIZE): 1, or 0 when the size is unknown or under one page. */
static int
ct_last_block(mach_port_t port, recnum_t *block)
{
    int                    info[DEV_GET_SIZE_COUNT];
    mach_msg_type_number_t cnt = DEV_GET_SIZE_COUNT;

    if (device_get_status(port, DEV_GET_SIZE, (dev_status_t)info, &cnt)
            != KERN_SUCCESS || cnt < DEV_GET_SIZE_COUNT
        || info[DEV_GET_SIZE_RECORD_SIZE] <= 0
        || info[DEV_GET_SIZE_DEVICE_SIZE] < 4096)
        return 0;
    *block = (recnum_t)((info[DEV_GET_SIZE_DEVICE_SIZE] - 4096)
                        / info[DEV_GET_SIZE_RECORD_SIZE]);
    return 1;
}

/* Why a partition gets no write, in the words the lines below use. */
static void
ct_why(char *why, size_t len, int boot, int asked, unsigned open,
       unsigned writing)
{
    if (boot != 0)
        snprintf(why, len, "the boot disk, or cannot be told apart from it");
    else if (!asked)
        snprintf(why, len, "a partition the block server would not say "
                 "who holds");
    else
        snprintf(why, len, "held by %u other handle%s, %u for writing -- "
                 "a mounted filesystem's or a pager's", open,
                 open == 1 ? "" : "s", writing);
}

/*
 * The partition a candidate's writes go to: the candidate itself when it is
 * not the boot disk and nobody else holds it, else its disk's partition c
 * when that is registered and nobody holds it -- its port in *tport and its
 * name in tname, and 1.  Otherwise 0, with the line that says why nothing on
 * this disk is written.  Asked of the partition, before this task opens a
 * handle on it: every handle counted is somebody else's.
 */
static int
scratch_target(mach_port_t part_port, const char *name, mach_port_t *tport,
               char *tname, size_t tlen)
{
    unsigned    open = 0, writing = 0;
    int         boot = is_the_boot_disk(part_port), asked = 0;
    char        why[128], swhy[128], sib[32];
    mach_port_t sp = MACH_PORT_NULL;
    size_t      n = strlen(name);

    if (boot == 0) {
        asked = ct_holders(part_port, &open, &writing);
        if (asked && open == 0) {
            *tport = part_port;
            snprintf(tname, tlen, "%s", name);
            return 1;
        }
    }
    ct_why(why, sizeof(why), boot, asked, open, writing);

    if (n == 0 || n >= sizeof(sib) || name[n - 1] == 'c') {
        printf("cap_test: [14] %s is %s — no write is offered on this disk "
               "(#596)\n", name, why);
        return 0;
    }
    memcpy(sib, name, n + 1);
    sib[n - 1] = 'c';
    if (netname_look_up(name_server_port, "", sib, &sp) != KERN_SUCCESS) {
        printf("cap_test: [14] %s is %s, and this disk has no %s — no write "
               "is offered on it (#596)\n", name, why, sib);
        return 0;
    }
    open = writing = 0;
    asked = ct_holders(sp, &open, &writing);
    if (asked && open == 0) {
        printf("cap_test: [14] %s is %s — the writes go to %s, which nobody "
               "else holds (#596)\n", name, why, sib);
        *tport = sp;
        snprintf(tname, tlen, "%s", sib);
        return 1;
    }
    ct_why(swhy, sizeof(swhy), 0, asked, open, writing);
    printf("cap_test: [14] %s is %s, and %s is %s — no write is offered on "
           "this disk (#596)\n", name, why, sib, swhy);
    (void)mach_port_deallocate(mach_task_self(), sp);
    return 0;
}

/*
 * ── #599: a buffer capability belongs to the handle it was handed on ──────
 *
 * The block server kept the capabilities its clients handed over per
 * controller and tried them for any client, so once one client handed its
 * buffer over, every task with a handle on that controller could have had
 * the disk read or write it.  Now they live in the handle, a full handle
 * refuses rather than evicts, and only a DMA buffer's capability is taken.
 * The helpers below open a handle, allocate one page, hand its capability to
 * a handle, and read the partition's first 4 KiB into it.
 */
static mach_port_t
b2_open(mach_port_t part_port, const char *name)
{
    struct uros_cap  tok;
    security_token_t null_sec = { { 0, 0 } };
    char             blob[CAP_TOKEN_MAX];
    mach_port_t      handle = MACH_PORT_NULL;

    memset(&tok, 0, sizeof(tok));
    if (cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(name),
                    CAP_OP_BLK_READ | CAP_OP_BLK_WRITE, 0, &tok)
        != KERN_SUCCESS)
        return MACH_PORT_NULL;
    memcpy(blob, &tok, sizeof(tok));
    if (device_open_cap(part_port, MACH_PORT_NULL, D_READ | D_WRITE,
                        null_sec, (char *)name, blob,
                        (mach_msg_type_number_t)sizeof(tok), &handle)
        != KERN_SUCCESS)
        return MACH_PORT_NULL;
    return handle;
}

static void
b2_close(mach_port_t handle)
{
    if (handle == MACH_PORT_NULL)
        return;
    (void)device_close(handle);
    (void)mach_port_deallocate(mach_task_self(), handle);
}

struct b2_page {
    vm_address_t kva, uva, pa;
    uint64_t     region;
};

static int
b2_alloc(mach_port_t device_port, struct b2_page *pg)
{
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    kern_return_t          kr;

    memset(pg, 0, sizeof(*pg));
    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), &pg->kva, &pg->uva, &pa_list,
                             &pa_cnt, &pg->region);
    if (kr == KERN_SUCCESS && pa_cnt == 1)
        pg->pa = pa_list[0];
    if (pa_list != NULL)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                            pa_cnt * sizeof(vm_address_t));
    return kr == KERN_SUCCESS && pg->pa != 0;
}

static void
b2_free(mach_port_t device_port, struct b2_page *pg)
{
    if (pg->kva != 0)
        (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, pg->kva, 4096);
    pg->kva = 0;
}

static kern_return_t
b2_hand(mach_port_t handle, uint64_t region, uint32_t type)
{
    struct uros_cap t;
    kern_return_t   kr;

    memset(&t, 0, sizeof(t));
    kr = cap_request(type, region,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE, 0, &t);
    if (kr != KERN_SUCCESS)
        return kr;
    return device_register_dma(handle, (char *)&t, sizeof(t));
}

/* Fill the page with 0xA5, read 4 KiB into it: kr, and what +1080 holds. */
static kern_return_t
b2_read(mach_port_t handle, struct b2_page *pg, unsigned *magic, int *intact)
{
    io_buf_len_t  got = 0;
    vm_address_t  pa = pg->pa;
    kern_return_t kr;
    unsigned      i;

    memset((void *)pg->uva, 0xA5, 4096);
    kr = device_read_phys(handle, D_READ, 0, 4096, &pa, 1, &got);
    *magic = (unsigned)(((unsigned char *)pg->uva)[1080])
           | ((unsigned)(((unsigned char *)pg->uva)[1081]) << 8);
    *intact = 1;
    for (i = 0; i < 4096; i++)
        if (((unsigned char *)pg->uva)[i] != 0xA5)
            *intact = 0;
    return kr;
}

/*
 * [12b] A page no capability covers is refused before any DMA (#599).  A
 * fresh handle, handed nothing, reads into a page of this task's: the block
 * server must refuse it and the page must hold what it held.  Until every
 * client handed its buffers over, such a handle had its addresses passed
 * through untranslated, and the superblock landed in the page -- also under
 * --iommu, where this runs on virtio_blk0a, whose DMA QEMU never puts through
 * the IOMMU (#591).  Which of the server's lines appears is not asked.
 */
static int
a_page_nobody_granted_is_refused(mach_port_t device_port, mach_port_t part_port,
                                 const char *name)
{
    mach_port_t    h;
    struct b2_page r;
    kern_return_t  kr;
    unsigned       magic;
    int            intact, ok;

    h = b2_open(part_port, name);
    if (h == MACH_PORT_NULL || !b2_alloc(device_port, &r)) {
        printf("cap_test: [12b] WRONG — no handle or no page to try\n");
        b2_close(h);
        return 0;
    }
    kr = b2_read(h, &r, &magic, &intact);
    ok = kr != KERN_SUCCESS && intact;
    if (ok)
        printf("cap_test: [12b] a read into a page nobody granted refused "
               "(kr=%d), the page untouched\n", (int)kr);
    else
        printf("cap_test: [12b] WRONG — a read into a page nobody granted "
               "answered kr=%d, page %s, +1080 0x%x\n", (int)kr,
               intact ? "untouched" : "WRITTEN", magic);
    b2_free(device_port, &r);
    b2_close(h);
    return ok;
}

/*
 * [19] A second client cannot spend the first one's capability.  Two handles
 * on one partition, each handed its own page; the second reads into the
 * first's page and must be refused with the page untouched, before and after
 * the first has read into it (a translation kept per controller would have
 * been primed by then); and each still reads its own.
 */
static int
a_capability_is_its_handles(mach_port_t device_port, mach_port_t part_port,
                            const char *name)
{
    mach_port_t    ha, hb;
    struct b2_page r1, r2;
    kern_return_t  k1, k2, k3, k4;
    unsigned       m1, m2, m3, m4;
    int            i1, i2, i3, i4, ok;

    ha = b2_open(part_port, name);
    hb = b2_open(part_port, name);
    if (ha == MACH_PORT_NULL || hb == MACH_PORT_NULL ||
        !b2_alloc(device_port, &r1) || !b2_alloc(device_port, &r2) ||
        b2_hand(ha, r1.region, RESOURCE_DMA_BUFFER) != KERN_SUCCESS ||
        b2_hand(hb, r2.region, RESOURCE_DMA_BUFFER) != KERN_SUCCESS) {
        printf("cap_test: [19] WRONG — %s is registered, and two handles "
               "and two handed pages were not all there (#591)\n", name);
        b2_close(ha);
        b2_close(hb);
        b2_free(device_port, &r1);
        b2_free(device_port, &r2);
        return 0;
    }

    k1 = b2_read(hb, &r1, &m1, &i1);		/* the other's page */
    k2 = b2_read(ha, &r1, &m2, &i2);		/* its own */
    k3 = b2_read(hb, &r1, &m3, &i3);		/* the other's, again */
    k4 = b2_read(hb, &r2, &m4, &i4);		/* its own */
    ok = k1 != KERN_SUCCESS && i1 && k2 == KERN_SUCCESS && m2 == 0xEF53u &&
         k3 != KERN_SUCCESS && i3 && k4 == KERN_SUCCESS && m4 == 0xEF53u;

    b2_close(ha);
    b2_close(hb);
    b2_free(device_port, &r1);
    b2_free(device_port, &r2);

    if (!ok) {
        printf("cap_test: [19] WRONG — %s: 2nd into 1st's page kr=%d (%s), "
               "1st kr=%d 0x%x, 2nd again kr=%d (%s), 2nd own kr=%d 0x%x\n",
               name, (int)k1, i1 ? "untouched" : "WRITTEN", (int)k2, m2,
               (int)k3, i3 ? "untouched" : "WRITTEN", (int)k4, m4);
        return 0;
    }
    printf("cap_test: [19] %s: a second handle refused the first's page "
           "(kr=%d, kr=%d after the first read 0x%x), page untouched; own "
           "page 0x%x — a capability is its handle's\n", name, (int)k1,
           (int)k3, m2, m4);
    return 1;
}

/*
 * [20] Revoking a buffer's capability takes the device's mapping down.
 * Hand a page over, read into it (the grant is made then, on a machine that
 * confines devices), count the devices it is mapped for, revoke, count again:
 * 0.  And a read after the revocation is refused with the page untouched.
 * On a machine that confines nothing no mapping exists to take down, and the
 * count half says so rather than passing.
 */
static int
revoking_a_capability_takes_the_mapping_down(mach_port_t device_port,
                                             mach_port_t part_port,
                                             const char *name)
{
    mach_port_t     h;
    struct b2_page  r;
    struct uros_cap t;
    kern_return_t   kr, k1, k2, kr_rev, ku0, ku1;
    natural_t       users0 = 0, users1 = 0;
    unsigned        m1, m2;
    int             i1, i2, ok;

    memset(&t, 0, sizeof(t));
    h = b2_open(part_port, name);
    kr = (h == MACH_PORT_NULL || !b2_alloc(device_port, &r)) ? KERN_FAILURE
       : cap_request(RESOURCE_DMA_BUFFER, r.region,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE, 0, &t);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(h, (char *)&t, sizeof(t));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [20] WRONG — %s is registered, and no page was "
               "handed over (kr=%d) (#591)\n", name, (int)kr);
        b2_close(h);
        b2_free(device_port, &r);
        return 0;
    }

    k1 = b2_read(h, &r, &m1, &i1);
    ku0 = device_dma_region_users(device_port, r.region, &users0);
    kr_rev = cap_revoke(t.cap_id);
    ku1 = device_dma_region_users(device_port, r.region, &users1);
    k2 = b2_read(h, &r, &m2, &i2);
    b2_close(h);
    b2_free(device_port, &r);

    ok = k1 == KERN_SUCCESS && m1 == 0xEF53u && kr_rev == KERN_SUCCESS &&
         ku0 == KERN_SUCCESS && ku1 == KERN_SUCCESS && users1 == 0 &&
         k2 != KERN_SUCCESS && i2;
    if (!ok) {
        printf("cap_test: [20] WRONG — %s: read %d 0x%x, users %d/%u, revoke "
               "%d, users %d/%u, read after %d (page %s)\n", name, (int)k1,
               m1, (int)ku0, (unsigned)users0, (int)kr_rev, (int)ku1,
               (unsigned)users1, (int)k2, i2 ? "untouched" : "WRITTEN");
        return 0;
    }
    if (users0 == 0)
        printf("cap_test: [20] %s: the mapping half NOT APPLICABLE — no "
               "device mapping exists here (users 0); a read after the "
               "revocation refused (kr=%d), page untouched\n", name,
               (int)k2);
    else
        printf("cap_test: [20] %s: revoking the capability took the "
               "mapping down (users %u -> %u); a read after it refused "
               "(kr=%d), page untouched\n", name, (unsigned)users0,
               (unsigned)users1, (int)k2);
    return 1;
}

/*
 * [21] A handle holds four capabilities and refuses a fifth rather than
 * evicting one; the same one handed again replaces itself; a new handle
 * starts empty.
 */
static int
a_handle_refuses_rather_than_evicts(mach_port_t device_port,
                                    mach_port_t part_port, const char *name)
{
    mach_port_t     h, h2;
    struct b2_page  r;
    struct uros_cap t[5];
    kern_return_t   kr[5], kr_again, kr_read, kr_new;
    unsigned        magic;
    int             intact, i, ok;

    h = b2_open(part_port, name);
    if (h == MACH_PORT_NULL || !b2_alloc(device_port, &r)) {
        printf("cap_test: [21] WRONG — %s is registered, and no handle or "
               "no page came (#591)\n", name);
        b2_close(h);
        b2_free(device_port, &r);
        return 0;
    }
    for (i = 0; i < 5; i++) {
        memset(&t[i], 0, sizeof(t[i]));
        kr[i] = cap_request(RESOURCE_DMA_BUFFER, r.region,
                            CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE,
                            0, &t[i]);
        if (kr[i] == KERN_SUCCESS)
            kr[i] = device_register_dma(h, (char *)&t[i], sizeof(t[i]));
    }
    kr_again = device_register_dma(h, (char *)&t[0], sizeof(t[0]));
    kr_read = b2_read(h, &r, &magic, &intact);
    b2_close(h);

    h2 = b2_open(part_port, name);
    kr_new = h2 == MACH_PORT_NULL ? KERN_FAILURE
           : device_register_dma(h2, (char *)&t[4], sizeof(t[4]));
    b2_close(h2);
    b2_free(device_port, &r);

    ok = kr[0] == 0 && kr[1] == 0 && kr[2] == 0 && kr[3] == 0 &&
         kr[4] == KERN_NO_SPACE && kr_again == 0 && kr_read == 0 &&
         magic == 0xEF53u && kr_new == 0;
    if (!ok) {
        printf("cap_test: [21] WRONG — %s: five on one handle %d %d %d %d "
               "%d, first again %d, read %d 0x%x, fifth on a new handle %d; "
               "want 0 0 0 0 %d, 0, 0 0xef53, 0\n", name, (int)kr[0],
               (int)kr[1], (int)kr[2], (int)kr[3], (int)kr[4],
               (int)kr_again, (int)kr_read, magic, (int)kr_new,
               (int)KERN_NO_SPACE);
        return 0;
    }
    printf("cap_test: [21] %s: four capabilities taken, the fifth refused "
           "(kr=%d), the first again 0, a read 0x%x, a new handle took the "
           "fifth — full refuses, never evicts\n", name, (int)kr[4], magic);
    return 1;
}

/*
 * [24] Only a DMA buffer's capability is taken for a buffer: a block-device
 * capability that carries the buffer's id is refused.
 */
static int
a_buffer_capability_is_a_buffers(mach_port_t device_port,
                                 mach_port_t part_port, const char *name)
{
    mach_port_t    h;
    struct b2_page r;
    kern_return_t  kr;

    h = b2_open(part_port, name);
    if (h == MACH_PORT_NULL || !b2_alloc(device_port, &r)) {
        printf("cap_test: [24] WRONG — %s is registered, and no handle or "
               "no page came (#591)\n", name);
        b2_close(h);
        b2_free(device_port, &r);
        return 0;
    }
    kr = b2_hand(h, r.region, RESOURCE_BLK_DEVICE);
    b2_close(h);
    b2_free(device_port, &r);

    if (kr == KERN_SUCCESS) {
        printf("cap_test: [24] WRONG — %s took a block-device capability "
               "carrying buffer %llu's id as that buffer's\n", name,
               (unsigned long long)r.region);
        return 0;
    }
    printf("cap_test: [24] %s refused a block-device capability carrying a "
           "buffer's id (kr=%d) — the kind is checked, not only the id\n",
           name, (int)kr);
    return 1;
}

/*
 * ── [26] A revoked buffer's kept translation is refused by the IOMMU (#537) ─
 *
 * Step 3: where the device is isolated the block server translates a handed
 * buffer once and keeps the table.  A revocation takes the kernel's grant down
 * at once, but reaches the block server as a notice, which can be read after
 * the next transfer -- and with ablations/537-blk-table-kept.patch is ignored.
 * Then the revoked buffer's translation is USED, and only the IOMMU stands
 * between the device and the page.  On ahci0a, the controller behind it
 * (virtio is passed through, #591): hand a page over, read into it, revoke,
 * read again.  The page must hold what it held and the grant must be gone;
 * the read must be refused.  Either the block server had forgotten the
 * translation (the notice came first: KERN_NO_ACCESS, the kernel not asked), or
 * it still held the capability and answered the revocation (CAP_ERR_REVOKED):
 * asked of the kernel, or -- with a kept translation -- found when the epoch
 * read after the transfer had moved, the device refused meanwhile by the IOMMU
 * ("iommu: ... REFUSED" in the kernel's log).  A read answered 0 is WRONG: the
 * controller's success for a transfer the IOMMU refused, reported to the
 * client -- the window ablations/537-blk-no-epoch.patch leaves open.
 */
static int
a_revoked_translation_is_refused(mach_port_t device_port, mach_port_t part_port,
                                 const char *name)
{
    mach_port_t     h;
    struct b2_page  r;
    struct uros_cap t;
    kern_return_t   kr, k1, k2, kr_rev, ku0, ku1;
    natural_t       users0 = 0, users1 = 0;
    unsigned        m1, m2;
    int             i1, i2, ok;

    memset(&t, 0, sizeof(t));
    h = b2_open(part_port, name);
    kr = (h == MACH_PORT_NULL || !b2_alloc(device_port, &r)) ? KERN_FAILURE
       : cap_request(RESOURCE_DMA_BUFFER, r.region,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE, 0, &t);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(h, (char *)&t, sizeof(t));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [26] WRONG — %s is registered, and no page was "
               "handed over (kr=%d) (#591)\n", name, (int)kr);
        b2_close(h);
        b2_free(device_port, &r);
        return 0;
    }

    k1 = b2_read(h, &r, &m1, &i1);
    ku0 = device_dma_region_users(device_port, r.region, &users0);
    kr_rev = cap_revoke(t.cap_id);
    ku1 = device_dma_region_users(device_port, r.region, &users1);
    k2 = b2_read(h, &r, &m2, &i2);
    b2_close(h);
    b2_free(device_port, &r);

    ok = k1 == KERN_SUCCESS && m1 == 0xEF53u && kr_rev == KERN_SUCCESS &&
         ku0 == KERN_SUCCESS && ku1 == KERN_SUCCESS && users1 == 0 && i2 &&
         k2 != KERN_SUCCESS;
    if (!ok) {
        printf("cap_test: [26] WRONG — %s: read %d 0x%x, users %d/%u, revoke "
               "%d, users %d/%u, read after %d (page %s)%s\n", name, (int)k1,
               m1, (int)ku0, (unsigned)users0, (int)kr_rev, (int)ku1,
               (unsigned)users1, (int)k2, i2 ? "untouched" : "WRITTEN",
               k2 == KERN_SUCCESS && i2
               ? " — success reported for a transfer that wrote nothing" : "");
        return 0;
    }
    if (users0 == 0)
        printf("cap_test: [26] %s: NOT APPLICABLE — the device is not "
               "confined here, so nothing is kept; a read after the "
               "revocation refused (kr=%d), page untouched (#537)\n", name,
               (int)k2);
    else if (k2 == CAP_ERR_REVOKED)
        printf("cap_test: [26] %s: after the revocation the grant was gone "
               "(users %u -> 0) and a read was answered with the revocation "
               "(kr=%d), page untouched — the block server still held the "
               "capability (#537)\n", name, (unsigned)users0, (int)k2);
    else
        printf("cap_test: [26] %s: after the revocation the grant was gone "
               "(users %u -> 0) and a read was refused (kr=%d), page "
               "untouched — the block server had forgotten the capability "
               "(#537)\n", name, (unsigned)users0, (int)k2);
    return 1;
}

/*
 * ── [27] A handle moves data only the way it was opened (#614) ─────────────
 *
 * A read-only block capability opens D_READ, and the same capability is
 * refused a D_READ | D_WRITE open: the open asks the token for what the mode
 * needs.  The D_READ handle reads, and is refused by each of the four ways
 * of writing -- copying, batched, physical, and inband, which is a stub
 * today.  The mirror: a write-only capability's D_WRITE handle is refused
 * both ways of reading.
 *
 * ⚠️ The writes go to the scratch block of a disk [14] may write, and each
 * carries the block's own bytes, read just before.  Should a check be
 * missing (ablations/614-no-mode-check.patch) the write lands and changes
 * nothing, and the arm says WRONG for its having been accepted.  The opens
 * and the mirror write nothing and run on every disk.
 */
#define CT27_BYTES	4096u

/* A block capability for `ops' and an open for `mode' with it: the open's
 * answer, or *no_cap set when cap_server gave no capability at all. */
static kern_return_t
ct27_open(mach_port_t part_port, const char *name, uint32_t ops,
          dev_mode_t mode, mach_port_t *handle, int *no_cap)
{
    struct uros_cap  tok;
    security_token_t null_sec = { { 0, 0 } };
    char             blob[CAP_TOKEN_MAX];

    *handle = MACH_PORT_NULL;
    *no_cap = 0;
    if (cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(name), ops, 0, &tok)
        != KERN_SUCCESS) {
        *no_cap = 1;
        return KERN_FAILURE;
    }
    memcpy(blob, &tok, sizeof(tok));
    return device_open_cap(part_port, MACH_PORT_NULL, mode, null_sec,
                           (char *)name, blob,
                           (mach_msg_type_number_t)sizeof(tok), handle);
}

static void
ct27_close(mach_port_t handle)
{
    (void)device_close(handle);
    (void)mach_port_deallocate(mach_task_self(), handle);
}

/* One scatter-gather page handed over to `handle', for the physical paths. */
static kern_return_t
ct27_buffer(mach_port_t device_port, mach_port_t handle, vm_address_t *kva,
            vm_address_t **pa_list, mach_msg_type_number_t *pa_cnt)
{
    struct uros_cap buf_cap;
    vm_address_t    uva = 0;
    uint64_t        region_id = 0;
    kern_return_t   kr;

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), kva, &uva, pa_list, pa_cnt,
                             &region_id);
    if (kr != KERN_SUCCESS)
        return kr;
    memset(&buf_cap, 0, sizeof(buf_cap));
    kr = cap_request(RESOURCE_DMA_BUFFER, region_id,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE, 0,
                     &buf_cap);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(handle, (char *)&buf_cap, sizeof(buf_cap));
    return kr;
}

static void
ct27_buffer_free(mach_port_t device_port, vm_address_t kva,
                 vm_address_t *pa_list, mach_msg_type_number_t pa_cnt)
{
    if (kva != 0)
        (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    if (pa_list != NULL)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                            pa_cnt * sizeof(vm_address_t));
}

/* Each way of writing, offered the block's own bytes: none may be taken.
 * `block' is the last page of the partition scratch_target() chose (#596). */
static int
ct27_read_only_writes_nothing(mach_port_t device_port, mach_port_t h,
                              const char *name, recnum_t block)
{
    io_buf_ptr_t           data = NULL;
    mach_msg_type_number_t cnt = 0;
    io_buf_len_t           wr = 0, got = 0;
    recnum_t               rec = block;
    unsigned32             size = CT27_BYTES;
    vm_address_t           kva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    kern_return_t          kr;
    int                    ok = 1;

    kr = device_read(h, D_READ, block, CT27_BYTES, &data, &cnt);
    if (kr != KERN_SUCCESS || cnt != CT27_BYTES) {
        printf("cap_test: [27] WRONG — %s: a D_READ handle could not read "
               "the scratch block (kr=%d, %u bytes)\n", name, (int)kr,
               (unsigned)cnt);
        if (kr == KERN_SUCCESS && data != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
        return 0;
    }

    kr = device_write(h, D_WRITE, block, data, cnt, &wr);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_READ handle wrote %u bytes "
               "through device_write\n", name, (unsigned)wr);
        ok = 0;
    }
    kr = device_write_batch(h, D_WRITE, &rec, 1, &size, 1, data, cnt, &wr);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_READ handle wrote %u bytes "
               "through device_write_batch\n", name, (unsigned)wr);
        ok = 0;
    }
    kr = device_write_inband(h, D_WRITE, block, (const char *)data,
                             IO_INBAND_MAX, &wr);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_READ handle wrote %u bytes "
               "through device_write_inband\n", name, (unsigned)wr);
        ok = 0;
    }
    (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);

    kr = ct27_buffer(device_port, h, &kva, &pa_list, &pa_cnt);
    if (kr == KERN_SUCCESS)
        kr = device_read_phys(h, D_READ, block, CT27_BYTES, pa_list,
                              pa_cnt, &got);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_READ handle could not read "
               "the scratch block into a page it handed over (kr=%d), so "
               "the physical write was not offered\n", name, (int)kr);
        ok = 0;
    } else {
        kr = device_write_phys(h, D_WRITE, block, CT27_BYTES,
                               pa_list, pa_cnt, &got);
        if (kr == KERN_SUCCESS) {
            printf("cap_test: [27] WRONG — %s: a D_READ handle wrote %u "
                   "bytes through device_write_phys\n", name, (unsigned)got);
            ok = 0;
        }
    }
    ct27_buffer_free(device_port, kva, pa_list, pa_cnt);
    return ok;
}

/* The mirror: a write-only handle reads nothing, either way. */
static int
ct27_write_only_reads_nothing(mach_port_t device_port, mach_port_t h,
                              const char *name)
{
    io_buf_ptr_t           data = NULL;
    mach_msg_type_number_t cnt = 0;
    io_buf_len_t           got = 0;
    vm_address_t           kva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    kern_return_t          kr;
    int                    ok = 1;

    kr = device_read(h, D_READ, SCRATCH_BLOCK, CT27_BYTES, &data, &cnt);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_WRITE handle read %u bytes "
               "through device_read\n", name, (unsigned)cnt);
        if (data != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
        ok = 0;
    }

    kr = ct27_buffer(device_port, h, &kva, &pa_list, &pa_cnt);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a D_WRITE handle could not hand a "
               "page over (kr=%d), so the physical read was not offered\n",
               name, (int)kr);
        ok = 0;
    } else {
        kr = device_read_phys(h, D_READ, SCRATCH_BLOCK, CT27_BYTES, pa_list,
                              pa_cnt, &got);
        if (kr == KERN_SUCCESS) {
            printf("cap_test: [27] WRONG — %s: a D_WRITE handle read %u "
                   "bytes through device_read_phys\n", name, (unsigned)got);
            ok = 0;
        }
    }
    ct27_buffer_free(device_port, kva, pa_list, pa_cnt);
    return ok;
}

/* The four writes, through a read-only handle of its own on the partition
 * scratch_target() chose, at that partition's last page (#596). */
static int
ct27_writes_offered(mach_port_t device_port, mach_port_t tport,
                    const char *tname)
{
    mach_port_t   h = MACH_PORT_NULL;
    recnum_t      block = 0;
    int           no_cap = 0, ok;
    kern_return_t kr;

    kr = ct27_open(tport, tname, CAP_OP_BLK_READ, D_READ, &h, &no_cap);
    if (no_cap || kr != KERN_SUCCESS || h == MACH_PORT_NULL) {
        printf("cap_test: [27] WRONG — %s: no read-only handle to offer the "
               "writes through (kr=%d%s)\n", tname, (int)kr,
               no_cap ? ", no capability" : "");
        return 0;
    }
    if (!ct_last_block(h, &block)) {
        printf("cap_test: [27] WRONG — %s did not say its size, so it has no "
               "last page to offer the writes at (#596)\n", tname);
        ct27_close(h);
        return 0;
    }
    ok = ct27_read_only_writes_nothing(device_port, h, tname, block);
    ct27_close(h);
    return ok;
}

static int
a_handle_moves_data_only_its_way(mach_port_t device_port,
                                 mach_port_t part_port, const char *name,
                                 mach_port_t tport, const char *tname)
{
    mach_port_t   rh = MACH_PORT_NULL, both = MACH_PORT_NULL;
    mach_port_t   wh = MACH_PORT_NULL;
    int           no_cap = 0, ok = 1;
    kern_return_t kr;

    kr = ct27_open(part_port, name, CAP_OP_BLK_READ, D_READ, &rh, &no_cap);
    if (no_cap) {
        printf("cap_test: [27] WRONG — %s is registered, and no read-only "
               "block capability was given for it (#591)\n", name);
        return 0;
    }
    if (kr != KERN_SUCCESS || rh == MACH_PORT_NULL) {
        printf("cap_test: [27] WRONG — %s: a read-only capability was "
               "refused a D_READ open (kr=%d)\n", name, (int)kr);
        return 0;
    }

    kr = ct27_open(part_port, name, CAP_OP_BLK_READ, D_READ | D_WRITE, &both,
                   &no_cap);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [27] WRONG — %s: a read-only capability opened "
               "D_READ | D_WRITE\n", name);
        ct27_close(both);
        ok = 0;
    }

    /*
     * The writes only where [14] may write, the partition scratch_target()
     * chose (#596): should a check be missing they land, and they used to
     * land in ext_server's root.  The opens and the mirror below write
     * nothing, so they run on every candidate.
     */
    if (tport == MACH_PORT_NULL)
        printf("cap_test: [27] %s: no partition of this disk is free for "
               "writes, as [14]'s line says — the four writes are not "
               "offered, a missing check would let them through (#596)\n",
               name);
    else if (!ct27_writes_offered(device_port, tport, tname))
        ok = 0;
    ct27_close(rh);

    kr = ct27_open(part_port, name, CAP_OP_BLK_WRITE, D_WRITE, &wh, &no_cap);
    if (no_cap) {
        printf("cap_test: [27] WRONG — %s: no write-only block capability, "
               "so the mirror did not run\n", name);
        ok = 0;
    } else if (kr != KERN_SUCCESS || wh == MACH_PORT_NULL) {
        printf("cap_test: [27] WRONG — %s: a write-only capability was "
               "refused a D_WRITE open (kr=%d)\n", name, (int)kr);
        ok = 0;
    } else {
        if (!ct27_write_only_reads_nothing(device_port, wh, name))
            ok = 0;
        ct27_close(wh);
    }

    if (ok && tport != MACH_PORT_NULL)
        printf("cap_test: [27] %s: a read-only capability opened D_READ and "
               "not D_READ | D_WRITE; on %s one read, and was refused by each "
               "way of writing -- copying, batched, physical, inband; a "
               "write-only one was refused both ways of reading (#614)\n",
               name, tname);
    else if (ok)
        printf("cap_test: [27] %s: a read-only capability opened D_READ and "
               "not D_READ | D_WRITE; a write-only one was refused both ways "
               "of reading -- the writes are offered on a disk that may take "
               "them (#614)\n", name);
    return ok;
}

/*
 * ── [25] Many pages in one request (#537) ─────────────────────────────
 *
 * Every physical request the bundle made was one page -- 649 of 649 lines of
 * the block server's counter, on both targets (02/10): ext_server's only
 * physical read is a page-cache miss, and its readahead reads by copy.  #537's
 * batched ask is for requests of many pages, so this makes them: a 32-page
 * buffer handed to the block server, read into 64 times at 64 places, 128 KB
 * a request, every page compared with what a copying read of the same blocks
 * returns -- an answer that put a page in the wrong place, or a page in none,
 * is WRONG here.  The block server says what the asking cost on this handle:
 * at 1024 and 2048 pages, and when it closes.
 */
#define CT25_PAGES	32
#define CT25_ROUNDS	64

static int
many_pages_in_one_request(mach_port_t device_port, mach_port_t part_port,
                          const char *name)
{
    struct uros_cap        tok, buf_cap;
    security_token_t       null_sec = { { 0, 0 } };
    char                   blob[CAP_TOKEN_MAX];
    mach_port_t            handle = MACH_PORT_NULL;
    kern_return_t          kr = KERN_SUCCESS;
    vm_address_t           kva = 0, uva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    io_buf_len_t           got = 0;
    uint64_t               region_id = 0;
    const unsigned         bytes = CT25_PAGES * 4096;
    unsigned               r, p, bad_page = 0, done = 0;
    int                    ok = 0;

    /*
     * Read-only, as this arm only reads.  It asked READ | WRITE until #614:
     * ds_device_open_cap() asked every token for both whatever the mode.
     */
    kr = cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(name),
                     CAP_OP_BLK_READ, 0, &tok);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [25] WRONG — %s is registered and no block "
               "capability was given for it (kr=%d) (#591)\n", name, (int)kr);
        return 0;
    }
    memcpy(blob, &tok, sizeof(tok));
    kr = device_open_cap(part_port, MACH_PORT_NULL, D_READ, null_sec,
                         (char *)name, blob,
                         (mach_msg_type_number_t)sizeof(tok), &handle);
    if (kr != KERN_SUCCESS || handle == MACH_PORT_NULL) {
        printf("cap_test: [25] WRONG — %s is registered and refused the "
               "open (kr=%d) (#591)\n", name, (int)kr);
        return 0;
    }

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, CT25_PAGES,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (kr != KERN_SUCCESS || pa_cnt != CT25_PAGES) {
        printf("cap_test: [25] WRONG — no %u-page buffer to read %s into "
               "(kr=%d, %u pages) (#591)\n", CT25_PAGES, name, (int)kr,
               (unsigned)pa_cnt);
        if (pa_list != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                                pa_cnt * sizeof(vm_address_t));
        (void)device_close(handle);
        (void)mach_port_deallocate(mach_task_self(), handle);
        return 0;
    }

    memset(&buf_cap, 0, sizeof(buf_cap));
    kr = cap_request(RESOURCE_DMA_BUFFER, region_id,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE,
                     0, &buf_cap);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(handle, (char *)&buf_cap, sizeof(buf_cap));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [25] WRONG — %s: the buffer was not handed over "
               "(kr=%d) (#591)\n", name, (int)kr);
        goto out;
    }

    for (r = 0; r < CT25_ROUNDS; r++) {
        recnum_t rec = (recnum_t)(r * (bytes / 512));
        io_buf_ptr_t copy = NULL;
        mach_msg_type_number_t copied = 0;

        /* A fill of its own each round: last round's bytes cannot pass. */
        memset((void *)uva, (int)((r & 0xFF) ^ 0x5A), bytes);
        got = 0;
        kr = device_read_phys(handle, D_READ, rec, bytes, pa_list, pa_cnt,
                              &got);
        if (kr != KERN_SUCCESS || got != bytes)
            break;
        kr = device_read(handle, D_READ, rec, bytes, &copy, &copied);
        if (kr != KERN_SUCCESS || copied != bytes) {
            if (copy != NULL && copied > 0)
                (void)vm_deallocate(mach_task_self(), (vm_offset_t)copy,
                                    copied);
            break;
        }
        for (p = 0; p < CT25_PAGES; p++)
            if (memcmp((char *)uva + p * 4096, (char *)copy + p * 4096,
                       4096) != 0)
                break;
        (void)vm_deallocate(mach_task_self(), (vm_offset_t)copy, copied);
        if (p != CT25_PAGES) {
            bad_page = p;
            break;
        }
        done++;
    }

    if (done == CT25_ROUNDS) {
        printf("cap_test: [25] %s: %u reads of %u pages each, every page "
               "what a copying read of the same blocks returns — the block "
               "server's lines for this handle say what asking cost (#537)\n",
               name, CT25_ROUNDS, CT25_PAGES);
        ok = 1;
    } else if (kr != KERN_SUCCESS || got != bytes)
        printf("cap_test: [25] WRONG — %s: read %u of %u refused or short "
               "(kr=%d, %u of %u bytes)\n", name, done + 1, CT25_ROUNDS,
               (int)kr, (unsigned)got, bytes);
    else
        printf("cap_test: [25] WRONG — %s: read %u of %u put page %u where a "
               "copying read of the same blocks disagrees\n", name, done + 1,
               CT25_ROUNDS, bad_page);

out:
    (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, bytes);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                        pa_cnt * sizeof(vm_address_t));
    (void)device_close(handle);
    (void)mach_port_deallocate(mach_task_self(), handle);
    return ok;
}

static int
the_bytes_must_fit_the_pages(mach_port_t device_port, mach_port_t part_port,
                             const char *name)
{
    struct uros_cap        tok, buf_cap;
    security_token_t       null_sec = { { 0, 0 } };
    char                   blob[CAP_TOKEN_MAX];
    mach_port_t            handle = MACH_PORT_NULL;
    kern_return_t          kr;
    vm_address_t           kva = 0, uva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    io_buf_len_t           got = 0;
    uint64_t               region_id = 0;
    unsigned               magic;
    int                    ok = 0;

    /*
     * 🔴 A REFUSAL HERE IS WRONG, NOT AN ARM THAT DID NOT RUN (#591).  The
     * partition is registered -- an absent one never reaches this function --
     * so a capability, an open or a page refused is the path failing.  Each of
     * these said "DID NOT RUN" and returned success, and an arm that never ran
     * read as one that passed.
     */
    kr = cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(name),
                     CAP_OP_BLK_READ | CAP_OP_BLK_WRITE, 0, &tok);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [14] WRONG — %s is registered and no block "
               "capability was given for it (kr=%d)\n", name, (int)kr);
        return 0;
    }

    memcpy(blob, &tok, sizeof(tok));
    kr = device_open_cap(part_port, MACH_PORT_NULL, D_READ | D_WRITE,
                         null_sec, (char *)name, blob,
                         (mach_msg_type_number_t)sizeof(tok), &handle);
    if (kr != KERN_SUCCESS || handle == MACH_PORT_NULL) {
        printf("cap_test: [14] WRONG — %s is registered and refused the "
               "open (kr=%d)\n", name, (int)kr);
        return 0;
    }

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (kr != KERN_SUCCESS || pa_cnt != 1) {
        printf("cap_test: [14] WRONG — no scatter-gather page to read %s "
               "into (kr=%d, %u pages)\n", name, (int)kr, (unsigned)pa_cnt);
        if (pa_list != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                                pa_cnt * sizeof(vm_address_t));
        (void)device_close(handle);
        (void)mach_port_deallocate(mach_task_self(), handle);
        return 0;
    }

    memset(&buf_cap, 0, sizeof(buf_cap));
    kr = cap_request(RESOURCE_DMA_BUFFER, region_id,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE,
                     0, &buf_cap);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(handle, (char *)&buf_cap, sizeof(buf_cap));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [14] WRONG — %s: the buffer was not handed "
               "over (kr=%d)\n", name, (int)kr);
        goto out;
    }

    /* One page, one page's worth of bytes: the request that fits. */
    memset((void *)uva, 0, 4096);
    kr = device_read_phys(handle, D_READ, 0, 4096, pa_list, pa_cnt, &got);
    magic = (unsigned)(((unsigned char *)uva)[1080])
          | ((unsigned)(((unsigned char *)uva)[1081]) << 8);

    if (kr != KERN_SUCCESS || got != 4096 || magic != 0xEF53u) {
        printf("cap_test: [14] WRONG — %s refused a request its pages DO "
               "cover: kr=%d, %u bytes, +1080 holds 0x%x rather than "
               "0xef53\n", name, (int)kr, (unsigned)got, magic);
        goto out;
    }
    printf("cap_test: [14] %s moved 4096 bytes into one page — the covered "
           "request works, so a refusal below is about the size and not "
           "about the path\n", name);

    /*
     * Two pages' worth of bytes, one page offered.  The drive would be told
     * to move sixteen sectors into a table describing four thousand bytes.
     */
    memset((void *)uva, 0, 4096);
    got = 0;
    kr = device_read_phys(handle, D_READ, 0, 8192, pa_list, pa_cnt, &got);

    if (kr == KERN_SUCCESS) {
        printf("cap_test: [14] WRONG — %s accepted 8192 bytes into %u page "
               "and reported %u bytes read: the transfer that happened is "
               "smaller than the one that was asked for\n",
               name, (unsigned)pa_cnt, (unsigned)got);
        goto out;
    }

    printf("cap_test: [14] %s refused 8192 bytes into %u page (kr=%d) — the "
           "page list has to cover the byte count\n",
           name, (unsigned)pa_cnt, (int)kr);
    ok = 1;

out:
    (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                        pa_cnt * sizeof(vm_address_t));
    (void)device_close(handle);
    (void)mach_port_deallocate(mach_task_self(), handle);
    return ok;
}

/*
 * ── [14]'s write half, on the partition scratch_target() chose ────────────
 *
 * The refusal above is the same code on both paths, but the ADDRESSES are
 * not: the read half maps every page through the kernel before handing it to
 * the controller and the write half did not, so only a transfer that actually
 * happens can tell whether the write half is speaking the same language.
 * Read and write must agree about one block, which needs no oracle outside
 * the pair.
 *
 * 🔴 AND THE BLOCK IS PUT BACK WHENEVER A WRITE WAS ASKED FOR (#596).  The put
 * back was skipped when the write was refused or short -- a short write may
 * have written some of it -- and its own result was thrown away.  Now it runs
 * after every write asked for, and is read again: a block that does not read
 * as it did before the test is WRONG, and says it may still hold the pattern.
 */

/* Read, write the pattern, read back, put back, read again: 1 or 0. */
static int
ct14_write_and_put_back(mach_port_t device_port, mach_port_t handle,
                        const char *tname, recnum_t block)
{
    static unsigned char   saved[4096];
    struct uros_cap        buf_cap;
    vm_address_t           kva = 0, uva = 0;
    vm_address_t          *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    uint64_t               region_id = 0;
    io_buf_len_t           got = 0, wrote = 0, put = 0;
    kern_return_t          kr, wkr, pkr;
    unsigned int           i;
    int                    same = 1, back = 1, ok = 0;

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (kr != KERN_SUCCESS || pa_cnt != 1) {
        printf("cap_test: [14] WRONG — no scatter-gather page to write %s "
               "through (kr=%d, %u pages)\n", tname, (int)kr,
               (unsigned)pa_cnt);
        if (pa_list != NULL)
            (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                                pa_cnt * sizeof(vm_address_t));
        return 0;
    }
    memset(&buf_cap, 0, sizeof(buf_cap));
    kr = cap_request(RESOURCE_DMA_BUFFER, region_id,
                     CAP_OP_DMA_DEVICE_READ | CAP_OP_DMA_DEVICE_WRITE, 0,
                     &buf_cap);
    if (kr == KERN_SUCCESS)
        kr = device_register_dma(handle, (char *)&buf_cap, sizeof(buf_cap));
    if (kr != KERN_SUCCESS)
        printf("cap_test: [14] WRONG — %s: the buffer was not handed over "
               "(kr=%d)\n", tname, (int)kr);
    else {
        memset((void *)uva, 0, 4096);
        kr = device_read_phys(handle, D_READ, block, 4096, pa_list, pa_cnt,
                              &got);
        if (kr != KERN_SUCCESS || got != 4096)
            printf("cap_test: [14] WRONG — %s could not read block %u, so "
                   "nothing was written (kr=%d, %u bytes)\n", tname,
                   (unsigned)block, (int)kr, (unsigned)got);
        else {
            memcpy(saved, (const void *)uva, 4096);
            for (i = 0; i < 4096; i++)
                ((unsigned char *)uva)[i] = (unsigned char)(i * 7u + 0x5Au);
            wkr = device_write_phys(handle, D_WRITE, block, 4096, pa_list,
                                    pa_cnt, &wrote);

            memset((void *)uva, 0, 4096);
            kr = device_read_phys(handle, D_READ, block, 4096, pa_list,
                                  pa_cnt, &got);
            for (i = 0; i < 4096 && same; i++)
                if (((unsigned char *)uva)[i]
                    != (unsigned char)(i * 7u + 0x5Au))
                    same = 0;
            if (kr != KERN_SUCCESS)
                same = 0;

            /* Put back after any write asked for, and read again. */
            memcpy((void *)uva, saved, 4096);
            pkr = device_write_phys(handle, D_WRITE, block, 4096, pa_list,
                                    pa_cnt, &put);
            memset((void *)uva, 0, 4096);
            kr = device_read_phys(handle, D_READ, block, 4096, pa_list,
                                  pa_cnt, &got);
            if (pkr != KERN_SUCCESS || put != 4096)
                back = 0;
            else if (kr != KERN_SUCCESS
                     || memcmp((const void *)uva, saved, 4096) != 0)
                back = -1;

            if (wkr != KERN_SUCCESS || wrote != 4096)
                printf("cap_test: [14] WRONG — %s refused a covered "
                       "physical write of block %u (kr=%d, %u bytes)\n",
                       tname, (unsigned)block, (int)wkr, (unsigned)wrote);
            else if (!same)
                printf("cap_test: [14] WRONG — %s read back something else "
                       "after a physical write of block %u (kr=%d)\n",
                       tname, (unsigned)block, (int)kr);
            else
                ok = 1;
            /* Two failures, said apart: refused, or said done and not. */
            if (back == 0) {
                printf("cap_test: [14] WRONG — %s: putting block %u back "
                       "was refused or short (kr=%d, %u bytes), and it may "
                       "still hold the test pattern (#596)\n", tname,
                       (unsigned)block, (int)pkr, (unsigned)put);
                ok = 0;
            } else if (back < 0) {
                printf("cap_test: [14] WRONG — %s: block %u was put back "
                       "(%u bytes) and does not read as it did before the "
                       "test (kr=%d), so it may still hold the test pattern "
                       "(#596)\n", tname, (unsigned)block, (unsigned)put,
                       (int)kr);
                ok = 0;
            } else if (ok)
                printf("cap_test: [14] %s wrote block %u through physical "
                       "addresses, read the same bytes back, and put the "
                       "block back as it was — the write half's addresses "
                       "reach the disk (#596)\n", tname, (unsigned)block);
        }
    }
    (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                        pa_cnt * sizeof(vm_address_t));
    return ok;
}

/*
 * The write half on `tport': 1 passed, 0 WRONG, -1 not attempted (said).
 * Who holds the partition is asked again through this task's own handle, the
 * moment before writing: scratch_target() asked before the handle existed.
 */
static int
the_write_half(mach_port_t device_port, mach_port_t tport, const char *tname)
{
    struct uros_cap  tok;
    security_token_t null_sec = { { 0, 0 } };
    char             blob[CAP_TOKEN_MAX];
    mach_port_t      handle = MACH_PORT_NULL;
    unsigned         open = 0, writing = 0;
    recnum_t         block = 0;
    kern_return_t    kr;
    int              ok;

    kr = cap_request(RESOURCE_BLK_DEVICE, cap_name_hash(tname),
                     CAP_OP_BLK_READ | CAP_OP_BLK_WRITE, 0, &tok);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [14] WRONG — %s is registered and no block "
               "capability was given for it (kr=%d)\n", tname, (int)kr);
        return 0;
    }
    memcpy(blob, &tok, sizeof(tok));
    kr = device_open_cap(tport, MACH_PORT_NULL, D_READ | D_WRITE, null_sec,
                         (char *)tname, blob,
                         (mach_msg_type_number_t)sizeof(tok), &handle);
    if (kr != KERN_SUCCESS || handle == MACH_PORT_NULL) {
        printf("cap_test: [14] WRONG — %s is registered and refused the open "
               "(kr=%d)\n", tname, (int)kr);
        return 0;
    }

    if (!ct_holders(handle, &open, &writing)) {
        printf("cap_test: [14] %s: the block server would not say who holds "
               "it now — the write is not attempted (#596)\n", tname);
        ok = -1;
    } else if (open != 0) {
        printf("cap_test: [14] %s: %u other handle%s opened it since it was "
               "chosen — the write is not attempted (#596)\n", tname, open,
               open == 1 ? "" : "s");
        ok = -1;
    } else if (!ct_last_block(handle, &block)) {
        printf("cap_test: [14] WRONG — %s did not say its size, so it has no "
               "last page to write (#596)\n", tname);
        ok = 0;
    } else
        ok = ct14_write_and_put_back(device_port, handle, tname, block);

    (void)device_close(handle);
    (void)mach_port_deallocate(mach_task_self(), handle);
    return ok;
}

/*
 * ── [18] The PCI configuration ports are the kernel's (#597) ─────────
 *
 * The configuration mechanism is an address port and a data port, and the
 * kernel keeps the pair whole under pci_cfg_port_lock.  A task reaching the
 * ports could drive the pair as two RPCs, outside that lock, and its access
 * would land on whatever another processor addressed in between -- the race
 * that made the HAL count a device that is not there.  So the claim, the read
 * and the write must all refuse them.
 *
 * The claim in three shapes, as io_claim_race [5] asks of the rulers: the
 * whole window, the data port alone, and a range reaching 0xCF8 from below;
 * anything granted is given straight back.  Then a READ of each port, which
 * needs no claim: that was the route left open when only the claim refused
 * them.  Then 0x10CFC, which the kernel cut to 0xCFC before any check saw it
 * as reserved, and 0x61, the 8254's gate, which the same refusal keeps
 * (#508); not 0x40, whose read, if the refusal were missing, would move the
 * counter's byte flip-flop under i386's clock, which reads channel 0 as a
 * latch command and two one-byte reads.  No write is tried -- if the refusal
 * were missing, writing 0xCF8 would be the race itself -- and the write RPC
 * asks the same checks as the read.
 *
 * #620: the ISA DMA controller, which a task could program to move bytes to
 * physical memory, claimed whole, by one register, from below, and by one
 * page register; and read at the two status registers and a page register,
 * which move no byte flip-flop -- the address and count registers would.
 */
static int
the_pci_config_ports_are_the_kernels(mach_port_t device_port)
{
    static const struct {
        unsigned int port;
        unsigned int count;
    } ask[] = {
        { 0xCF8, 8 },   /* the address and the data */
        { 0xCFC, 1 },   /* the data port alone */
        { 0xCF4, 5 },   /* from below, reaching the address port */
        { 0x70,  2 },   /* the CMOS, whose index is the NMI mask (#599) */
        { 0x20,  2 },   /* the master 8259 (#599) */
        { 0xA0,  2 },   /* the slave 8259 (#599) */
        { 0x4D0, 2 },   /* the 8259s' edge/level registers (#599) */
        { 0x00,  16 },  /* the first 8237 DMA controller (#620) */
        { 0x0A,  1 },   /* its mask register alone (#620) */
        { 0xBE,  4 },   /* from below, reaching the second 8237 (#620) */
        { 0x87,  1 },   /* channel 0's page register alone (#620) */
    };
    static const struct {
        unsigned int  port;
        unsigned int  size;
        kern_return_t want;
    } read_at[] = {
        { 0xCF8,   4, KERN_NO_ACCESS },          /* the address port */
        { 0xCFC,   4, KERN_NO_ACCESS },          /* the data port */
        { 0x10CFC, 4, KERN_INVALID_ARGUMENT },   /* 0xCFC above 16 bits */
        { 0x61,    1, KERN_NO_ACCESS },          /* the 8254's gate (#508) */
        { 0x71,    1, KERN_NO_ACCESS },          /* the CMOS data (#599) */
        { 0x21,    1, KERN_NO_ACCESS },          /* the 8259's mask (#599) */
        { 0x08,    1, KERN_NO_ACCESS },          /* the first 8237's status (#620) */
        { 0xD0,    1, KERN_NO_ACCESS },          /* the second 8237's status (#620) */
        { 0x81,    1, KERN_NO_ACCESS },          /* channel 2's page register (#620) */
    };
    unsigned int  i, released, klog_from, data, bad = 0;
    kern_return_t kr;

    for (i = 0; i < sizeof(ask) / sizeof(ask[0]); i++) {
        kr = device_io_port_claim(device_port, ask[i].port, ask[i].count,
                                  &released, &klog_from);
        if (kr == KERN_NO_ACCESS)
            continue;
        bad++;
        if (kr == KERN_SUCCESS) {
            (void) device_io_port_unclaim(device_port, ask[i].port);
            printf("cap_test: [18] WRONG — a claim of 0x%x..0x%x was "
                   "GRANTED, and it covers ports the kernel keeps "
                   "(#597, #599)\n", ask[i].port,
                   ask[i].port + ask[i].count - 1);
        } else
            printf("cap_test: [18] WRONG — a claim of 0x%x..0x%x was "
                   "refused with kr=%d, not KERN_NO_ACCESS\n", ask[i].port,
                   ask[i].port + ask[i].count - 1, (int)kr);
    }
    for (i = 0; i < sizeof(read_at) / sizeof(read_at[0]); i++) {
        kr = device_io_port_read(device_port, read_at[i].port,
                                 read_at[i].size, &data);
        if (kr == read_at[i].want)
            continue;
        bad++;
        printf("cap_test: [18] WRONG — a read of 0x%x answered kr=%d%s, "
               "not %d: a task reaches a port the kernel keeps\n",
               read_at[i].port, (int)kr,
               kr == KERN_SUCCESS ? " (it was served)" : "",
               (int)read_at[i].want);
    }
    if (bad != 0)
        return 0;

    printf("cap_test: [18] claims of 0xcf8..0xcff, 0xcfc alone, "
           "0xcf4..0xcf8, the CMOS, both 8259s and the ELCR, the 8237s and "
           "their page registers, and reads of 0xcf8, 0xcfc, 0x61, 0x71, "
           "0x21, 0x8, 0xd0, 0x81 and 0x10cfc, refused — the claim and read "
           "RPCs keep the ports the kernel keeps (#508, #597, #599, #620)\n");
    return 1;
}

/*
 * ── [28] A port no claim covers is refused (#620) ─────────────────────
 *
 * Holding the master device port used to reach every I/O port that no claim
 * covered.  Now a task reaches only what it claimed, every byte of the
 * access.  COM4's scratch register, 0x2EF: no driver here claims it, it is
 * outside COM1's window, and it is only ever READ, so nothing is disturbed
 * whatever answers it -- io_claim_race races on COM3, char_test asks COM2.
 *
 *   - read without a claim: refused with KERN_NO_ACCESS;
 *   - claimed, and read again: answered -- the claim is the way in, so the
 *     refusal above was about the claim and not about the port;
 *   - still claimed, two bytes from it, 0x2EF..0x2F0: refused, because
 *     0x2F0 is nobody's.
 *
 * Then the claim is given back.
 *
 * ⚠️ The claim takes one of the kernel's four legacy slots, and a released
 * slot is retiring until a grace period has passed (#538): beside
 * io_claim_race's rounds the table can be full for a moment.  That is waited
 * out for a second, and if it lasts, the arm says NOT ASKED -- unless the
 * read without a claim was served, which is the defect whatever the table.
 */
static int
a_port_no_claim_covers_is_refused(mach_port_t device_port)
{
    const unsigned int port = 0x2EF;
    unsigned int data = 0, released = 0, klog_from = 0, tries;
    kern_return_t bare, claim = KERN_RESOURCE_SHORTAGE, claimed, wide;

    bare = device_io_port_read(device_port, port, 1, &data);
    for (tries = 0; tries < 50 && claim == KERN_RESOURCE_SHORTAGE; tries++) {
        if (tries != 0)
            thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20);
        claim = device_io_port_claim(device_port, port, 1, &released,
                                     &klog_from);
    }
    if (claim == KERN_RESOURCE_SHORTAGE) {
        if (bare != KERN_NO_ACCESS) {
            printf("cap_test: [28] WRONG — 0x%x read without a claim kr=%d "
                   "(want %d); no claim slot came free in a second to ask "
                   "the rest (#620)\n", port, (int)bare,
                   (int)KERN_NO_ACCESS);
            return 0;
        }
        printf("cap_test: [28] NOT ASKED — 0x%x was refused without a "
               "claim, but every legacy claim slot stayed taken or retiring "
               "for a second, so the claim that is the way in could not be "
               "made (#620)\n", port);
        return 1;
    }
    if (claim != KERN_SUCCESS) {
        printf("cap_test: [28] WRONG — a claim of 0x%x alone was refused "
               "with kr=%d, so the way in cannot be shown (#620)\n", port,
               (int)claim);
        return 0;
    }
    claimed = device_io_port_read(device_port, port, 1, &data);
    wide = device_io_port_read(device_port, port, 2, &data);
    (void) device_io_port_unclaim(device_port, port);

    if (bare == KERN_NO_ACCESS && claimed == KERN_SUCCESS
        && wide == KERN_NO_ACCESS) {
        printf("cap_test: [28] a read of 0x%x refused without a claim and "
               "answered with one, and 0x%x..0x%x refused past it — a port "
               "is reached through its claim, every byte (#620)\n", port,
               port, port + 1);
        return 1;
    }
    printf("cap_test: [28] WRONG — 0x%x read without a claim kr=%d (want "
           "%d), with one kr=%d (want 0), and 0x%x..0x%x with one kr=%d "
           "(want %d) (#620)\n", port, (int)bare, (int)KERN_NO_ACCESS,
           (int)claimed, port, port + 1, (int)wide, (int)KERN_NO_ACCESS);
    return 0;
}

/*
 * ── [22] A buffer is freed by its owner, whole, once (#599) ───────────────
 *
 * device_dma_free checked neither: an address that was no region's was
 * ignored by the drop and kmem_free ran on it anyway, a region could be freed
 * at half its size, and twice.  So any holder of the master port freed kernel
 * memory it named.  The kernel now answers KERN_INVALID_ARGUMENT for a size
 * that is not the allocation's -- and frees nothing, which the capability
 * cap_server still issues for the region shows -- 0 for the right free, and
 * KERN_INVALID_ADDRESS for the second.  Another task's region is
 * dma_reclaim_test's to show, since it takes two tasks.
 */
static int
a_buffer_is_freed_whole_and_once(mach_port_t device_port)
{
    kern_return_t   kr, kr_half, kr_alive, kr_whole, kr_again;
    vm_address_t    kva = 0, uva = 0;
    vm_address_t   *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0;
    uint64_t        region_id = 0;
    struct uros_cap tok;

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 2,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (pa_list != NULL)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                            pa_cnt * sizeof(vm_address_t));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [22] a buffer is freed whole and once — DID NOT "
               "RUN, no two-page buffer (kr=%d)\n", (int)kr);
        return 1;
    }

    kr_half = device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    memset(&tok, 0, sizeof(tok));
    kr_alive = cap_request(RESOURCE_DMA_BUFFER, region_id,
                           CAP_OP_DMA_DEVICE_READ, 0, &tok);
    kr_whole = kr_half == KERN_SUCCESS ? KERN_SUCCESS
             : device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 8192);
    kr_again = device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 8192);

    if (kr_half != KERN_INVALID_ARGUMENT || kr_alive != KERN_SUCCESS ||
        kr_whole != KERN_SUCCESS || kr_again != KERN_INVALID_ADDRESS) {
        printf("cap_test: [22] WRONG — a 2-page buffer freed at 1 page %d, "
               "then its capability %d, the whole free %d, a second free %d; "
               "want %d, 0, 0, %d\n", (int)kr_half, (int)kr_alive,
               (int)kr_whole, (int)kr_again, (int)KERN_INVALID_ARGUMENT,
               (int)KERN_INVALID_ADDRESS);
        return 0;
    }
    printf("cap_test: [22] a 2-page buffer freed at 1 page refused (kr=%d), "
           "still issuable (kr=%d); whole free %d, again %d — freed by its "
           "owner, whole, once\n", (int)kr_half, (int)kr_alive,
           (int)kr_whole, (int)kr_again);
    return 1;
}

/*
 * ── [23] A free takes back the buffer's mapping, not the address (#599) ───
 *
 * The drop of a region removed the owner's user mapping by address, whatever
 * occupied it by then -- and the AHCI driver deallocates its buffers' user
 * mappings before it frees them.  So a task that gave the range back and
 * mapped something else there lost that instead.  This deallocates a buffer's
 * mapping, maps a page of its own at the same address, writes 0x5A, frees the
 * buffer, and reads the page back: it must still be there, still 0x5A.
 */
static int
a_free_takes_its_mapping_not_the_address(mach_port_t device_port)
{
    kern_return_t   kr, kr_free, kr_read;
    vm_address_t    kva = 0, uva = 0, mine;
    vm_address_t   *pa_list = NULL;
    mach_msg_type_number_t pa_cnt = 0, got = 0;
    uint64_t        region_id = 0;
    vm_offset_t     data = 0;
    unsigned char   seen = 0;

    kr = device_dma_alloc_sg(device_port, DEVICE_DMA_NO_BDF, 1,
                             mach_task_self(), &kva, &uva, &pa_list, &pa_cnt,
                             &region_id);
    if (pa_list != NULL)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)pa_list,
                            pa_cnt * sizeof(vm_address_t));
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [23] a free takes its own mapping — WRONG, no "
               "buffer (kr=%d) (#591)\n", (int)kr);
        return 0;
    }

    (void)vm_deallocate(mach_task_self(), uva, 4096);
    mine = uva;
    kr = vm_allocate(mach_task_self(), &mine, 4096, FALSE);
    if (kr != KERN_SUCCESS || mine != uva) {
        printf("cap_test: [23] a free takes its own mapping — WRONG, the "
               "address was not given back to this task (kr=%d) (#591)\n",
               (int)kr);
        (void)device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
        return 0;
    }
    *(volatile unsigned char *)mine = 0x5A;

    kr_free = device_dma_free(device_port, DEVICE_DMA_NO_BDF, kva, 4096);
    kr_read = vm_read(mach_task_self(), mine, 4096, &data, &got);
    if (kr_read == KERN_SUCCESS) {
        seen = *(unsigned char *)data;
        (void)vm_deallocate(mach_task_self(), data, got);
    }
    (void)vm_deallocate(mach_task_self(), mine, 4096);

    if (kr_free != KERN_SUCCESS || kr_read != KERN_SUCCESS || seen != 0x5A) {
        printf("cap_test: [23] WRONG — a buffer freed after its range was "
               "remapped: free %d, the new page's read %d holding 0x%x — the "
               "free took an address no longer the buffer's\n",
               (int)kr_free, (int)kr_read, (unsigned)seen);
        return 0;
    }
    printf("cap_test: [23] a buffer freed after its range was remapped (free "
           "kr=%d): the new page still reads (kr=%d) 0x%x — the free took the "
           "mapping, not the address\n", (int)kr_free, (int)kr_read,
           (unsigned)seen);
    return 1;
}

static int
a_device_has_one_driver(mach_port_t device_port)
{
    kern_return_t kr;
    natural_t     bdf, by_other = 0;
    int           claimed = 0;
    vm_address_t  kva = 0, dma = 0;
    uint64_t      region = 0;

    /*
     * Bus zero, every function: whichever device the block server reached
     * first is the one this borrows.  Found by search and not by a constant,
     * because which slot a controller lands in is a property of the board and
     * this runs on more than one.
     */
    for (bdf = 0; bdf < 0x100; bdf++) {
        kr = device_dma_owned(device_port, bdf, &by_other);
        if (kr == KERN_SUCCESS && by_other) {
            claimed = 1;
            break;
        }
    }

    if (!claimed) {
        printf("cap_test: [11] a device has one driver — NOT ASKED, "
               "nothing on bus 0 is driven by another task yet (#591)\n");
        return 1;
    }

    kr = device_dma_alloc(device_port, bdf, 4096, &kva, &dma, &region);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [11] WRONG — this task mapped memory for "
               "%02x:%02x.%u, which another task drives\n",
               (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
               (unsigned)(bdf & 7));
        (void)device_dma_free(device_port, bdf, kva, 4096);
        return 0;
    }

    printf("cap_test: [11] mapping memory for %02x:%02x.%u refused with "
           "kr=%d — a device has one driver, and holding the master port "
           "is no longer the whole answer\n",
           (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
           (unsigned)(bdf & 7), kr);
    return 1;
}

static int
subsystem_survives_a_failed_allocation(void)
{
    /*
     * The smallest subsystem mach_subsystem_create() accepts: no routines,
     * so `end - start' is zero and the validation loop over the routine
     * array never runs.  Nothing in it is ever called -- what is under test
     * is the object's lifetime, not the RPC it describes.
     */
    struct rpc_subsystem  desc;
    mach_port_qos_t       qos;
    mach_port_t           subsys = MACH_PORT_NULL;
    mach_port_t           taken  = MACH_PORT_NULL;
    mach_port_t           probe  = MACH_PORT_NULL;
    mach_port_t           name;
    kern_return_t         kr;
    int                   ok = 1;

    memset(&desc, 0, sizeof desc);
    desc.start     = 0;
    desc.end       = 0;
    desc.base_addr = (vm_address_t)(uintptr_t) &desc;

    kr = mach_subsystem_create(mach_task_self(), (user_subsystem_t) &desc,
                               (mach_msg_type_number_t) sizeof desc, &subsys);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [10] mach_subsystem_create failed kr=%d — the test "
               "could not run, which is not a pass\n", (int) kr);
        return 0;
    }

    /* The probe, on a subsystem known to be alive. */
    kr = syscall_mach_port_allocate_subsystem(mach_task_self(), subsys,
                                              &probe);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [10] trap 100 refused a subsystem that was just "
               "created: kr=%d — the observation cannot see a live one, so "
               "it cannot report a dead one\n", (int) kr);
        (void) mach_port_deallocate(mach_task_self(), subsys);
        return 0;
    }
    printf("cap_test: [10] trap 100 accepted a live subsystem — the probe "
           "below can tell the two answers apart\n");
    (void) mach_port_destroy(mach_task_self(), probe);

    /* A name that already denotes a right, so the allocation must fail. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &taken);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [10] could not allocate a name to collide with: "
               "kr=%d\n", (int) kr);
        (void) mach_port_deallocate(mach_task_self(), subsys);
        return 0;
    }

    memset(&qos, 0, sizeof qos);
    qos.name = TRUE;
    name = taken;

    kr = syscall_mach_port_allocate_full(mach_task_self(),
                                         MACH_PORT_RIGHT_RECEIVE,
                                         subsys, &qos, &name);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [10] trap 103 allocated a name already in use — "
               "the failure path this tests was never taken\n");
        ok = 0;
    } else {
        printf("cap_test: [10] trap 103 refused a name already in use: "
               "kr=%d\n", (int) kr);
    }

    /* The assertion: the failed call must not have consumed the subsystem. */
    name = MACH_PORT_NULL;
    kr = syscall_mach_port_allocate_subsystem(mach_task_self(), subsys, &name);
    if (kr == KERN_SUCCESS) {
        printf("cap_test: [10] the subsystem survived a failed allocation\n");
        (void) mach_port_destroy(mach_task_self(), name);
    } else {
        printf("cap_test: [10] the subsystem is GONE after one failed "
               "allocation: kr=%d — its count was decremented without ever "
               "having been incremented\n", (int) kr);
        ok = 0;
    }

    (void) mach_port_destroy(mach_task_self(), taken);
    (void) mach_port_deallocate(mach_task_self(), subsys);

    return ok;
}

static int
mig_check_fires(const char *label, mach_port_t dest,
                mach_msg_id_t id, mach_msg_size_t bad_size)
{
    /*
     * One buffer, not two.  mach_msg sends from and receives into the same
     * storage, so the reply lands on top of the request; a separate reply
     * struct is never written, and telling mach_msg it may receive more
     * than this buffer holds is how the message after it gets corrupted.
     * rcv_size below is sizeof(msg) for that reason and no other.
     */
    union {
        mach_msg_header_t  head;
        mig_reply_error_t  err;
        char               space[512];
    } msg;

    mach_port_t   reply = MACH_PORT_NULL;
    kern_return_t kr;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: %s reply port allocate FAIL kr=%d\n", label, (int)kr);
        return 0;
    }

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
                                        MACH_MSG_TYPE_MAKE_SEND_ONCE);
    msg.head.msgh_size        = bad_size;
    msg.head.msgh_remote_port = dest;
    msg.head.msgh_local_port  = reply;
    msg.head.msgh_id          = id;

    kr = mach_msg(&msg.head, MACH_SEND_MSG | MACH_RCV_MSG, bad_size,
                  sizeof(msg), reply, 5000, MACH_PORT_NULL);

    if (kr != MACH_MSG_SUCCESS) {
        printf("cap_test: %s no reply kr=0x%x — check did not answer\n",
               label, (unsigned)kr);
        (void)mach_port_destroy(mach_task_self(), reply);
        return 0;
    }

    (void)mach_port_destroy(mach_task_self(), reply);

    if (msg.err.RetCode != MIG_BAD_ARGUMENTS) {
        printf("cap_test: %s expected MIG_BAD_ARGUMENTS (%d), got %d "
               "(reply id=%d size=%d bits=0x%x)\n",
               label, MIG_BAD_ARGUMENTS, (int)msg.err.RetCode,
               (int)msg.err.Head.msgh_id, (int)msg.err.Head.msgh_size,
               (unsigned)msg.err.Head.msgh_bits);
        return 0;
    }

    /*
     * The routine name goes to the console, not into this reply, so what is
     * asserted here is that the check fired; that it named the routine is
     * read from the serial log ("mig: <routine> refused a message: ...").
     */
    printf("cap_test: %s refused with MIG_BAD_ARGUMENTS: OK "
           "(console must name the routine above)\n", label);
    return 1;
}

/*
 * #546: [16] a wired page that its owner gives back.
 *
 * 🔴 THIS ARM CANNOT REPORT ITS OWN FAILURE.  The defect it looks for is
 *
 *	panic(cpu N): pmap_remove_all removing a wired page
 *
 * so when it fires the test is gone and nothing after it runs.  That is why
 * every probe announces itself BEFORE acting and confirms itself after: the
 * line that has no partner names the probe that killed the machine, and a log
 * with all the pairs is the pass.  A verdict computed at the end would only
 * ever be reached by the runs that had nothing to report.
 *
 * ⚠️ And the first thing checked is that vm_wire SUCCEEDED.  #546's second
 * finding is a caller whose wiring never once happened because it asked with
 * the unprivileged host port and threw the answer away -- a probe that never
 * wires cannot panic, and would pass for the wrong reason forever.  The port
 * used here is the one bootstrap_ports hands out, which the kernel builds from
 * realhost.host_priv_self, so the call is expected to be accepted; if it is
 * refused, that is reported as the arm not having run.
 */
static boolean_t
a_wired_page_can_be_given_back(mach_port_t host_priv)
{
    const vm_size_t size = 3 * 4096;	/* more than one page, deliberately */
    boolean_t	    ok   = 1;

    /*
     * Probe 1 -- the issue's own words: wire your own memory, then free it.
     */
    vm_address_t a = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &a, size, TRUE);

    if (kr != KERN_SUCCESS) {
        printf("cap_test: [16] WRONG — vm_allocate refused (kr=%d), so the "
               "arm did not run\n", (int)kr);
        return 0;
    }

    /* Resident before wired: an unfaulted page has no mapping to wire. */
    memset((void *)a, 0x5a, size);

    kr = vm_wire(host_priv, mach_task_self(), a, size,
                 VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [16] WRONG — vm_wire refused (kr=%d), so the arm "
               "did not run (it needs the privileged host port, see #546)\n",
               (int)kr);
        (void)vm_deallocate(mach_task_self(), a, size);
        return 0;
    }
    printf("cap_test: [16] wired 0x%lx+%lu\n",
           (unsigned long)a, (unsigned long)size);

    printf("cap_test: [16] probe 1: deallocating a wired range\n");
    kr = vm_deallocate(mach_task_self(), a, size);
    printf("cap_test: [16] probe 1 survived, kr=%d\n", (int)kr);
    if (kr != KERN_SUCCESS)
        ok = 0;

    /*
     * Probe 2 and 3 -- the same physical pages reachable through two mappings,
     * one of them wired.
     *
     * 🔑 The kernel clears the wired bit for ONE map and one address
     * (vm_fault_unwire calls pmap_change_wiring(pmap, va, FALSE)), then acts
     * on the PHYSICAL page for every map that holds it
     * (pmap_page_protect).  Those two are not the same set, so a second
     * mapping of a wired page is the shape where the accounting and the page
     * tables can disagree.  vm_remap with copy = FALSE is how a task can build
     * that shape without a memory object of its own.
     *
     * Both orders are tried because which side is torn down first decides
     * which of the two is still wired when the physical page is touched.
     */
    for (int first_b = 0; first_b < 2; first_b++) {
        vm_address_t b = 0;
        vm_prot_t    cur = VM_PROT_NONE, max = VM_PROT_NONE;

        a = 0;
        kr = vm_allocate(mach_task_self(), &a, size, TRUE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe %d: vm_allocate FAIL kr=%d\n",
                   2 + first_b, (int)kr);
            ok = 0;
            continue;
        }
        memset((void *)a, 0xa5, size);

        kr = vm_remap(mach_task_self(), &b, size, 0, TRUE,
                      mach_task_self(), a, FALSE,
                      &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe %d: vm_remap kr=%d — no second "
                   "mapping, probe skipped\n", 2 + first_b, (int)kr);
            (void)vm_deallocate(mach_task_self(), a, size);
            continue;
        }

        kr = vm_wire(host_priv, mach_task_self(), a, size,
                     VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe %d: vm_wire refused kr=%d\n",
                   2 + first_b, (int)kr);
            (void)vm_deallocate(mach_task_self(), b, size);
            (void)vm_deallocate(mach_task_self(), a, size);
            ok = 0;
            continue;
        }

        printf("cap_test: [16] probe %d: two mappings 0x%lx and 0x%lx, "
               "0x%lx wired, freeing %s first\n",
               2 + first_b, (unsigned long)a, (unsigned long)b,
               (unsigned long)a, first_b ? "the unwired one" : "the wired one");

        if (first_b) {
            (void)vm_deallocate(mach_task_self(), b, size);
            (void)vm_deallocate(mach_task_self(), a, size);
        } else {
            (void)vm_deallocate(mach_task_self(), a, size);
            (void)vm_deallocate(mach_task_self(), b, size);
        }
        printf("cap_test: [16] probe %d survived\n", 2 + first_b);
    }

    /*
     * Probe 4 -- copy-on-write over a wired page, which is the shape that can
     * actually reach the guard.
     *
     * 🔑 The two probes above cannot: vm_map_delete unwires the entry it is
     * about to remove, and memory_object_lock_page refuses outright when
     * m->wire_count is non-zero.  The copy-on-write fault does neither.  When
     * it substitutes a fresh page it severs the SOURCE page's mappings in every
     * space that holds it -- vm_fault.c's pmap_page_protect(cur_m, VM_PROT_NONE),
     * deliberately unconditional since #385, because a mapping left live over a
     * page about to be freed is how a page on the free list stays readable.
     *
     * So: wire a range, make a copy-on-write mapping of it, then write through
     * the copy.  The source page is wired and mapped, and the fault revokes it.
     *
     * ⚠️ The read before the write is part of the shape, not caution: the PTE
     * the revoke has to find is the one a prior read fault installed.
     */
    {
        vm_address_t a4 = 0, b4 = 0;
        vm_prot_t    cur = VM_PROT_NONE, max = VM_PROT_NONE;
        volatile char *p;

        kr = vm_allocate(mach_task_self(), &a4, size, TRUE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 4: vm_allocate FAIL kr=%d\n", (int)kr);
            return 0;
        }
        memset((void *)a4, 0x3c, size);

        kr = vm_wire(host_priv, mach_task_self(), a4, size,
                     VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 4: vm_wire refused kr=%d\n", (int)kr);
            (void)vm_deallocate(mach_task_self(), a4, size);
            return 0;
        }

        kr = vm_remap(mach_task_self(), &b4, size, 0, TRUE,
                      mach_task_self(), a4, TRUE,	/* copy = TRUE */
                      &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 4: vm_remap(copy) kr=%d — probe "
                   "skipped\n", (int)kr);
            (void)vm_deallocate(mach_task_self(), a4, size);
            return ok;
        }

        p = (volatile char *)b4;
        printf("cap_test: [16] probe 4: read the copy at 0x%lx: 0x%02x\n",
               (unsigned long)b4, (unsigned)(unsigned char)p[0]);

        printf("cap_test: [16] probe 4: writing through a copy-on-write "
               "mapping of a WIRED page\n");
        for (vm_size_t off = 0; off < size; off += 4096)
            p[off] = 0x77;
        printf("cap_test: [16] probe 4 survived\n");

        (void)vm_deallocate(mach_task_self(), b4, size);
        (void)vm_deallocate(mach_task_self(), a4, size);
    }

    /*
     * Probe 5 -- a lazy copy over the wired page.  It does not reach the guard
     * either, and that is recorded rather than tidied away.
     *
     * The idea: vm_remap with copy = TRUE (probe 4) copies EAGERLY, so the
     * write through the copy finds a page of its own and never faults over the
     * original.  A LAZY copy should behave differently, and vm_read is how a
     * task arms one on its own memory -- vm_map_copyin sets the source object
     * to copy-delay and drops the source mappings to read-only, so the next
     * write through the wired mapping should be a copy-on-write fault whose
     * source page is the wired page itself.
     *
     * 🔴 IT IS NOT.  Ablation says so: with the vm_fault.c fix removed, this
     * probe survives on both targets, while the FLIPC bufgroup bench on i386
     * panics in the same build.  So something between vm_read and the fault
     * materialises the copy, and this probe measures the wrong thing.
     *
     * ⚠️ Kept because it cost two wrong attempts to establish, and the next
     * person to want a copy-on-write over a wired page from one task will
     * reach for exactly these two primitives.  🔑 The only witness there is at
     * the moment is ipc_bench's wired FLIPC pool, which the x86-64 bundle does
     * not carry -- so this arm cannot yet fail on that target at all.
     */
    {
        vm_address_t   a5 = 0;
        vm_offset_t    copy = 0;
        mach_msg_type_number_t got = 0;
        volatile char *p;

        kr = vm_allocate(mach_task_self(), &a5, size, TRUE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 5: vm_allocate FAIL kr=%d\n", (int)kr);
            return 0;
        }
        memset((void *)a5, 0x69, size);

        kr = vm_wire(host_priv, mach_task_self(), a5, size,
                     VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 5: vm_wire refused kr=%d\n", (int)kr);
            (void)vm_deallocate(mach_task_self(), a5, size);
            return 0;
        }

        kr = vm_read(mach_task_self(), a5, size, &copy, &got);
        if (kr != KERN_SUCCESS) {
            printf("cap_test: [16] probe 5: vm_read kr=%d — no lazy copy, "
                   "probe skipped\n", (int)kr);
            (void)vm_deallocate(mach_task_self(), a5, size);
            return ok;
        }
        printf("cap_test: [16] probe 5: lazy copy armed (%u bytes)\n",
               (unsigned)got);

        printf("cap_test: [16] probe 5: writing through the WIRED mapping "
               "with a copy outstanding\n");
        p = (volatile char *)a5;
        for (vm_size_t off = 0; off < size; off += 4096)
            p[off] = 0x11;
        printf("cap_test: [16] probe 5 survived\n");

        (void)vm_deallocate(mach_task_self(), copy, got);
        (void)vm_deallocate(mach_task_self(), a5, size);
    }

    return ok;
}

/*
 * #552: [17] the kernel's semaphores, called from userland on this target.
 *
 * 🔑 `sync_server.c` has been generated for `mach_kernel` on x86-64 all along,
 * so the kernel has served `semaphore_create` here since the port started --
 * and no user stub existed, because the x86-64 branch of libmach's .defs list
 * did not carry `sync.defs`.  Served and uncallable: produced and not consumed,
 * invisible because the only caller in the tree was ipc_bench, which does not
 * build on this target.
 *
 * ⚠️ So this arm exists to CALL them.  A generated stub that nothing calls is
 * the same state the port was already in, one layer along -- `ms_thread_create.c`
 * was present and uncompiled for the life of the port and looked like coverage.
 * The round trip is create, signal, wait, destroy: the wait must not block,
 * because the signal came first and the count is one.
 */
static boolean_t
the_kernel_semaphores_answer(mach_port_t task)
{
    mach_port_t   sema = MACH_PORT_NULL;
    kern_return_t kr;

    kr = semaphore_create(task, &sema, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS || sema == MACH_PORT_NULL) {
        printf("cap_test: [17] semaphore_create FAIL kr=%d\n", (int)kr);
        return 0;
    }

    kr = semaphore_signal(sema);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [17] semaphore_signal FAIL kr=%d\n", (int)kr);
        (void)semaphore_destroy(task, sema);
        return 0;
    }

    /*
     * Signalled once with a count of zero, so exactly one wait is satisfied
     * without blocking.  ⚠️ A second one would not return, which is why there
     * is one: this arm has no way to time out, and a test that hangs reports
     * nothing about anything.
     */
    kr = semaphore_wait(sema);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [17] semaphore_wait FAIL kr=%d\n", (int)kr);
        (void)semaphore_destroy(task, sema);
        return 0;
    }

    kr = semaphore_destroy(task, sema);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [17] semaphore_destroy FAIL kr=%d\n", (int)kr);
        return 0;
    }

    printf("cap_test: [17] semaphore create/signal/wait/destroy: OK\n");
    return 1;
}

/*
 * #708: [29] semaphore_timedwait() under a second.
 *
 * The kernel turned the timeout into ticks as tv_sec * hz plus
 * tv_nsec / NSEC_PER_SEC / hz -- integer division from the left, so the
 * nanoseconds came to 0 below a second -- and with no whole second in it, it
 * answered KERN_OPERATION_TIMED_OUT before it had looked at the count.  So four
 * questions, each against what a semaphore promises: a timed wait under a
 * second, and a zero timeout, take a count that is there; a zero timeout on a
 * count of zero answers at once; and a wait under a second on a count of zero
 * lasts about as long as it was told, by the realtime clock.
 *
 * ⚠️ That last one has a lower bound and only a loose upper one.  The defect
 * answers at once, and a loaded boot can wake a waiter late: a test that fails
 * for a scheduler's lateness reports the wrong thing.
 */
static boolean_t
the_semaphore_waits_as_long_as_told(mach_port_t task)
{
    tvalspec_t    zero = { 0, 0 }, half = { 0, 500000000 };
    tvalspec_t    fifth = { 0, 200000000 }, t0, t1;
    mach_port_t   host = mach_host_self(), clk = MACH_PORT_NULL;
    mach_port_t   sema = MACH_PORT_NULL;
    kern_return_t kr;
    long          ms;
    boolean_t     ok = 1;

    kr = semaphore_create(task, &sema, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS || sema == MACH_PORT_NULL) {
        printf("cap_test: [29] semaphore_create FAIL kr=%d\n", (int)kr);
        (void)mach_port_deallocate(mach_task_self(), host);
        return 0;
    }
    kr = host_get_clock_service(host, REALTIME_CLOCK, &clk);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [29] host_get_clock_service FAIL kr=%d\n", (int)kr);
        (void)semaphore_destroy(task, sema);
        return 0;
    }

    (void)semaphore_signal(sema);
    kr = semaphore_timedwait(sema, half);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [29] a 0.5 s wait on a count of one answered %d "
               "-- WRONG (#708)\n", (int)kr);
        ok = 0;
    }

    (void)semaphore_signal(sema);
    kr = semaphore_timedwait(sema, zero);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [29] a zero timeout on a count of one answered %d "
               "-- WRONG (#708)\n", (int)kr);
        ok = 0;
    }

    kr = semaphore_timedwait(sema, zero);
    if (kr != KERN_OPERATION_TIMED_OUT) {
        printf("cap_test: [29] a zero timeout on a count of zero answered %d "
               "-- WRONG (#708)\n", (int)kr);
        ok = 0;
    }

    (void)clock_get_time(clk, &t0);
    kr = semaphore_timedwait(sema, fifth);
    (void)clock_get_time(clk, &t1);
    ms = (long)(t1.tv_sec - t0.tv_sec) * 1000
         + ((long)t1.tv_nsec - (long)t0.tv_nsec) / 1000000;
    if (kr != KERN_OPERATION_TIMED_OUT || ms < 150 || ms > 2000) {
        printf("cap_test: [29] a 0.2 s wait on a count of zero answered %d "
               "after %ld ms -- WRONG (#708)\n", (int)kr, ms);
        ok = 0;
    }

    (void)mach_port_deallocate(mach_task_self(), clk);
    (void)semaphore_destroy(task, sema);
    if (ok)
        printf("cap_test: [29] semaphore_timedwait under a second: the "
               "counts taken, and %ld ms waited of 200 (#708)\n", ms);
    return ok;
}

/* Microseconds from t0 to t1, by the clock [29] reads. */
static long
us_between(const tvalspec_t *t0, const tvalspec_t *t1)
{
    return (long)(t1->tv_sec - t0->tv_sec) * 1000000
           + ((long)t1->tv_nsec - (long)t0->tv_nsec) / 1000;
}

/*
 * #708: [30] a futex's timeout, rounded down to a tick.
 *
 * The futex turned its milliseconds into ticks rounding down, with one tick
 * for anything under one, so a 15 ms wait was given one tick -- 10 ms at most
 * at 100 Hz -- where mach_msg()'s timeouts, and since [29]
 * semaphore_timedwait()'s, are rounded up.  So each of the three ways a futex
 * waits is told FUTEX_WAITS times in a row to wait 15 ms on a word nobody
 * changes, and the row must take at least the time it was told.  Rounded up,
 * two ticks a wait, it takes more than 150 ms; rounded down, one tick a wait,
 * about 80.
 *
 * ⚠️ In a row and against the time told, because then lateness cannot make
 * the fixed kernel fail: each wait starts when the one before it woke, so a
 * late wake moves the rest of the row later and never shortens it.  The
 * arm's first version measured each wait against a
 * semaphore wait told the same, and a loaded uniprocessor boot under TCG woke
 * the semaphore later than the futex: 238 ms against 409, a WRONG for a
 * futex that had waited twice what it was told.
 */
#define FUTEX_WAITS	8
#define FUTEX_WAIT_MS	15

static boolean_t
a_futex_waits_as_long_as_told(void)
{
    static const char *const way_name[3] = { "WAIT", "WAKE_WAIT", "WAITV" };
    tvalspec_t    t0, t1;
    mach_port_t   host = mach_host_self(), clk = MACH_PORT_NULL;
    unsigned int  word = 0, nobody = 0, woken;
    struct urmach_futexv v;
    kern_return_t kr;
    long          ms;
    boolean_t     ok = 1;
    int           way, i;

    kr = host_get_clock_service(host, REALTIME_CLOCK, &clk);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [30] host_get_clock_service FAIL kr=%d\n", (int)kr);
        return 0;
    }

    v.uaddr = &word;
    v.val = 0;
    for (way = 0; way < 3; way++) {
        kr = KERN_OPERATION_TIMED_OUT;
        (void)clock_get_time(clk, &t0);
        for (i = 0; i < FUTEX_WAITS && kr == KERN_OPERATION_TIMED_OUT; i++) {
            if (way == 0)
                kr = urmach_futex(&word,
                                  URMACH_FUTEX_WAIT | URMACH_FUTEX_PRIVATE,
                                  0, FUTEX_WAIT_MS, (unsigned int *)0);
            else if (way == 1)
                kr = urmach_futex(&word,
                                  URMACH_FUTEX_WAKE_WAIT | URMACH_FUTEX_PRIVATE,
                                  0, FUTEX_WAIT_MS, &nobody);
            else
                kr = urmach_futex_waitv(&v, 1, FUTEX_WAIT_MS, &woken);
        }
        (void)clock_get_time(clk, &t1);
        ms = us_between(&t0, &t1) / 1000;

        if (kr != KERN_OPERATION_TIMED_OUT) {
            printf("cap_test: [30] a futex %s of %d ms answered %d "
                   "-- WRONG (#708)\n", way_name[way], FUTEX_WAIT_MS, (int)kr);
            ok = 0;
        } else if (ms < FUTEX_WAITS * FUTEX_WAIT_MS) {
            printf("cap_test: [30] futex %s: %d waits of %d ms in a row took "
                   "%ld ms, of the %d told -- WRONG (#708)\n", way_name[way],
                   FUTEX_WAITS, FUTEX_WAIT_MS, ms, FUTEX_WAITS * FUTEX_WAIT_MS);
            ok = 0;
        } else
            printf("cap_test: [30] futex %s: %d waits of %d ms in a row took "
                   "%ld ms, of the %d told (#708)\n", way_name[way],
                   FUTEX_WAITS, FUTEX_WAIT_MS, ms, FUTEX_WAITS * FUTEX_WAIT_MS);
    }

    (void)mach_port_deallocate(mach_task_self(), clk);
    return ok;
}

/*
 * #708: [31] a mach_msg() timeout longer than 2^32 / hz milliseconds.
 *
 * convert_ipc_timeout_to_ticks() multiplied the milliseconds by hz in 32
 * bits, so a timeout past 42949672 ms -- eleven hours and fifty-six minutes at
 * 100 Hz -- wrapped, and 42949673 ms came out as one tick.  A receive is told
 * to wait that long on a port a second thread sends to after 0.3 s, and must
 * come back with the message, not with MACH_RCV_TIMED_OUT.
 *
 * ⚠️ If the send fails the sender destroys the port, so that the receive ends
 * either way: with the conversion right it would otherwise wait for its
 * twelve hours, and a test that hangs reports nothing.
 */
#define IPC_TIMEOUT_WRAPS	42949673u	/* x 100 is 2^32 + 4 */

struct late_sender {
    mach_port_t   port;
    kern_return_t idle_kr;
    kern_return_t send_kr;
};

static void *
send_after_a_while(void *arg)
{
    struct late_sender *s = arg;
    struct { mach_msg_header_t head; char room[64]; } idle_msg;
    mach_msg_header_t m;
    mach_port_t   idle = MACH_PORT_NULL;

    /* 0.3 s by a receive nobody sends to, short enough for either way. */
    s->idle_kr = mach_port_allocate(mach_task_self(),
                                    MACH_PORT_RIGHT_RECEIVE, &idle);
    if (s->idle_kr == KERN_SUCCESS) {
        s->idle_kr = mach_msg(&idle_msg.head,
                              MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                              sizeof(idle_msg), idle, 300, MACH_PORT_NULL);
        (void)mach_port_destroy(mach_task_self(), idle);
    }

    memset(&m, 0, sizeof(m));
    m.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    m.msgh_size        = sizeof(m);
    m.msgh_remote_port = s->port;
    m.msgh_local_port  = MACH_PORT_NULL;
    m.msgh_id          = 708;
    s->send_kr = mach_msg(&m, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL,
                          MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (s->send_kr != MACH_MSG_SUCCESS)
        (void)mach_port_destroy(mach_task_self(), s->port);
    return (void *)0;
}

static boolean_t
a_long_receive_timeout_is_long(void)
{
    struct { mach_msg_header_t head; char room[64]; } got;
    struct late_sender s;
    mach_port_t   host = mach_host_self(), clk = MACH_PORT_NULL;
    pthread_t     sender;
    tvalspec_t    t0, t1;
    kern_return_t kr;
    long          ms;
    boolean_t     ok = 1;

    kr = host_get_clock_service(host, REALTIME_CLOCK, &clk);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [31] host_get_clock_service FAIL kr=%d\n", (int)kr);
        return 0;
    }
    s.port = MACH_PORT_NULL;
    s.idle_kr = s.send_kr = KERN_FAILURE;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &s.port);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [31] mach_port_allocate FAIL kr=%d\n", (int)kr);
        (void)mach_port_deallocate(mach_task_self(), clk);
        return 0;
    }
    if (pthread_create(&sender, NULL, send_after_a_while, &s) != 0) {
        printf("cap_test: [31] pthread_create FAIL\n");
        (void)mach_port_destroy(mach_task_self(), s.port);
        (void)mach_port_deallocate(mach_task_self(), clk);
        return 0;
    }

    memset(&got, 0, sizeof(got));
    (void)clock_get_time(clk, &t0);
    kr = mach_msg(&got.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(got),
                  s.port, IPC_TIMEOUT_WRAPS, MACH_PORT_NULL);
    (void)clock_get_time(clk, &t1);
    ms = us_between(&t0, &t1) / 1000;
    (void)pthread_join(sender, NULL);

    if (kr != MACH_MSG_SUCCESS || got.head.msgh_id != 708 || ms < 150) {
        printf("cap_test: [31] a receive told %u ms answered 0x%x after %ld "
               "ms -- WRONG (#708)\n", IPC_TIMEOUT_WRAPS, (unsigned)kr, ms);
        ok = 0;
    } else
        printf("cap_test: [31] a receive told %u ms had the message sent "
               "after 0.3 s, after %ld ms (#708)\n", IPC_TIMEOUT_WRAPS, ms);
    if (s.idle_kr != MACH_RCV_TIMED_OUT || s.send_kr != MACH_MSG_SUCCESS) {
        printf("cap_test: [31] the sender's wait answered 0x%x and its send "
               "0x%x -- FAIL\n", (unsigned)s.idle_kr, (unsigned)s.send_kr);
        ok = 0;
    }

    if (s.send_kr == MACH_MSG_SUCCESS)
        (void)mach_port_destroy(mach_task_self(), s.port);
    (void)mach_port_deallocate(mach_task_self(), clk);
    return ok;
}

/*
 * #707: [32] a signal in flight when a timed semaphore wait times out.
 *
 * semaphore_signal() takes its waiter off the queue and marks it done under
 * the semaphore's lock, and then clear_wait()s it.  The waiter, back from
 * thread_block(), read whether it had been done WITHOUT that lock.  So a
 * timeout that woke it between the signaller's two statements made it return
 * its wait_result, THREAD_TIMED_OUT, as if that were a kern_return_t -- 1 --
 * for a wait whose signal it had taken; and the signaller's clear_wait() then
 * landed in its next wait and ended it.  A second thread signals S 10 ms after
 * this one starts a 40 ms wait on it, and then this one waits 500 ms on T,
 * which nobody signals.  The first wait must answer KERN_SUCCESS or
 * KERN_OPERATION_TIMED_OUT, with the signal counted once -- taken by the wait,
 * or left in S's count -- and the second must time out, after 450 ms or more.
 *
 * ⚠️ The window is two adjacent statements at splsched, so this arm catches
 * the defect only with ablations/707-widen-signal.patch holding the signaller
 * between them.  Without it, it passes on either kernel.
 */
struct sema_signaller {
    mach_port_t   sema;
    kern_return_t idle_kr;
    kern_return_t signal_kr;
};

static void *
signal_after_a_while(void *arg)
{
    struct sema_signaller *s = arg;
    struct { mach_msg_header_t head; char room[64]; } idle_msg;
    mach_port_t   idle = MACH_PORT_NULL;

    /* 10 ms by a receive nobody sends to. */
    s->idle_kr = mach_port_allocate(mach_task_self(),
                                    MACH_PORT_RIGHT_RECEIVE, &idle);
    if (s->idle_kr == KERN_SUCCESS) {
        s->idle_kr = mach_msg(&idle_msg.head,
                              MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                              sizeof(idle_msg), idle, 10, MACH_PORT_NULL);
        (void)mach_port_destroy(mach_task_self(), idle);
    }
    s->signal_kr = semaphore_signal(s->sema);
    return (void *)0;
}

static boolean_t
a_signal_in_flight_ends_one_wait(mach_port_t task)
{
    tvalspec_t    forty = { 0, 40000000 }, half = { 0, 500000000 };
    tvalspec_t    zero = { 0, 0 }, t0, t1;
    mach_port_t   host = mach_host_self(), clk = MACH_PORT_NULL;
    mach_port_t   s_sema = MACH_PORT_NULL, t_sema = MACH_PORT_NULL;
    struct sema_signaller sig;
    pthread_t     signaller;
    kern_return_t kr, first, second, left;
    long          ms;
    boolean_t     ok = 1;

    kr = host_get_clock_service(host, REALTIME_CLOCK, &clk);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [32] host_get_clock_service FAIL kr=%d\n", (int)kr);
        return 0;
    }
    kr = semaphore_create(task, &s_sema, SYNC_POLICY_FIFO, 0);
    if (kr == KERN_SUCCESS)
        kr = semaphore_create(task, &t_sema, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS) {
        printf("cap_test: [32] semaphore_create FAIL kr=%d\n", (int)kr);
        ok = 0;
    }
    if (ok) {
        sig.sema = s_sema;
        sig.idle_kr = sig.signal_kr = KERN_FAILURE;
        if (pthread_create(&signaller, NULL, signal_after_a_while, &sig) != 0) {
            printf("cap_test: [32] pthread_create FAIL\n");
            ok = 0;
        }
    }
    if (ok) {
        first = semaphore_timedwait(s_sema, forty);
        (void)clock_get_time(clk, &t0);
        second = semaphore_timedwait(t_sema, half);
        (void)clock_get_time(clk, &t1);
        ms = us_between(&t0, &t1) / 1000;
        (void)pthread_join(signaller, NULL);
        left = semaphore_timedwait(s_sema, zero);

        if (first != KERN_SUCCESS && first != KERN_OPERATION_TIMED_OUT) {
            printf("cap_test: [32] a 40 ms wait with a signal in flight "
                   "answered %d -- WRONG (#707)\n", (int)first);
            ok = 0;
        } else if ((first == KERN_SUCCESS) != (left == KERN_OPERATION_TIMED_OUT)) {
            printf("cap_test: [32] the wait answered %d and a try after "
                   "the signal %d: the signal counted %s -- WRONG (#707)\n",
                   (int)first, (int)left,
                   first == KERN_SUCCESS ? "twice" : "never");
            ok = 0;
        }
        if (second != KERN_OPERATION_TIMED_OUT || ms < 450) {
            printf("cap_test: [32] the next wait, 500 ms on a semaphore "
                   "nobody signals, answered %d after %ld ms -- WRONG "
                   "(#707)\n", (int)second, ms);
            ok = 0;
        }
        if (sig.idle_kr != MACH_RCV_TIMED_OUT || sig.signal_kr != KERN_SUCCESS) {
            printf("cap_test: [32] the signaller's wait answered 0x%x and "
                   "its signal %d -- FAIL\n", (unsigned)sig.idle_kr,
                   (int)sig.signal_kr);
            ok = 0;
        }
        if (ok)
            printf("cap_test: [32] a signal in flight: the wait answered %d, "
                   "the signal counted once, the next wait timed out after "
                   "%ld ms (#707)\n", (int)first, ms);
    }

    if (t_sema != MACH_PORT_NULL)
        (void)semaphore_destroy(task, t_sema);
    if (s_sema != MACH_PORT_NULL)
        (void)semaphore_destroy(task, s_sema);
    (void)mach_port_deallocate(mach_task_self(), clk);
    return ok;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    mach_port_t host_port, device_port, security_port;
    mach_port_t root_ledger_wired, root_ledger_paged;
    kern_return_t kr = bootstrap_ports(bootstrap_port,
                                       &host_port, &device_port,
                                       &root_ledger_wired, &root_ledger_paged,
                                       &security_port);
    if (kr != KERN_SUCCESS)
        return 1;

    printf_init(device_port);

    /*
     * Issue #180: report which SHA-256 path libcap selected and run a
     * known-answer test against the FIPS 180-4 vector for "abc".  This
     * proves the SHA-NI fast path (when active) produces the same
     * digest as the C reference; if it doesn't, every HMAC-gated RPC
     * after this would fail in confusing ways, so catch it up front.
     */
    sha256_dispatch_init();
    /*
     * #394: the KAT verdict must reach `pass` below.  It used to only print:
     * the KAT runs here, ahead of `pass`, so a mismatch could not fail the
     * suite and cap_test announced ALL TESTS PASSED on top of a provably
     * broken SHA-256 (the SHA-NI path shipped wrong digests -- invisible to
     * every test here, because both sides of each HMAC use the same function).
     * A known-answer test that cannot fail the run is not a test.
     */
    int kat_ok = 1;
    {
        static const uint8_t kat_in[] = { 'a', 'b', 'c' };
        static const uint8_t kat_expected[32] = {
            0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
            0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
            0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
            0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
        };
        uint8_t got[32];
        sha256(kat_in, sizeof kat_in, got);
        printf("cap_test: SHA-256 path = %s\n",
               sha256_using_sha_ni() ? "SHA-NI (CPU extension)" : "C reference");
        int diff = 0;
        for (unsigned ii = 0; ii < 32; ii++) {
            if (got[ii] != kat_expected[ii]) { diff = 1; break; }
        }
        if (!diff) {
            printf("cap_test: SHA-256 KAT(\"abc\") OK\n");
        } else {
            printf("cap_test: SHA-256 KAT(\"abc\") FAIL — digest mismatch\n");
            kat_ok = 0;
        }
    }

    printf("cap_test: starting, waiting for cap_server netname\n");
    (void)wait_for_cap_server();
    printf("cap_test: cap_server is up — running negative tests\n");

    /*
     * Build a well-formed setup token: cap_id == 0 (the kernel treats
     * the hmac[] bytes as a candidate key) with an obviously fake key.
     * If this call ever succeeded by mistake it would replace
     * cap_server's real key, so the 0xCC sentinel also works as a
     * post-mortem canary for "someone broke TOFU".
     */
    struct uros_cap setup;
    for (size_t i = 0; i < sizeof(setup); i++)
        ((uint8_t *)&setup)[i] = 0;
    setup.cap_id = 0;
    for (size_t i = 0; i < CAP_HMAC_SIZE; i++)
        setup.hmac[i] = 0xCC;

    kr = urmach_cap_register(&setup);

    int pass = kat_ok;		/* #394: a failed KAT fails the suite */
    if (kr == CAP_ERR_UNAUTHORIZED) {
        printf("cap_test: [1] unauthorized register rejected: OK\n");
    } else {
        printf("cap_test: [1] unauthorized register FAIL — "
               "expected CAP_ERR_UNAUTHORIZED (%d), got %d\n",
               CAP_ERR_UNAUTHORIZED, (int)kr);
        pass = 0;
    }

    /*
     * #395 probe.  Fill xmm0..7 with a known pattern, cross the kernel once,
     * read them back.  Interleaved:
     *
     *   control  = mach_thread_self()      no FPU in the kernel; proves the
     *                                      bare trap round-trip and this
     *                                      harness preserve XMM
     *   probe    = urmach_cap_verify()     the kernel HMACs the token
     *                                      (SHA-NI on this CPU) before
     *                                      rejecting it
     *
     * The token is garbage on purpose: cap_hmac_check runs unconditionally
     * (constant-time reject), so CAP_ERR_INVALID_TOKEN back means the HMAC
     * DID run.  CAP_ERR_INTERNAL means the cap key was never set, the HMAC
     * never executed, and the probe proved nothing — reported as such, not
     * as a pass.  A clobber that repeats with identical bytes is the SHA
     * signature (same input -> same intermediates), distinguishing it from
     * stray preemption noise.
     */
    {
        static uint8_t pat[128], got[128], snap[128];
        struct uros_cap bogus;
        int ctl_bad = 0, hmac_bad = 0, hmac_ran = 0, stable = 0;
        unsigned reg_mask = 0;
        int it;

        for (it = 0; it < 128; it++)
            pat[it] = (uint8_t)(0xA0 ^ (it * 7));
        for (size_t bi = 0; bi < sizeof(bogus); bi++)
            ((uint8_t *)&bogus)[bi] = 0x5A;

        for (it = 0; it < 50; it++) {
            xmm_fill(pat);
            (void)mach_thread_self();
            xmm_read(got);
            if (memcmp(pat, got, 128) != 0)
                ctl_bad++;

            xmm_fill(pat);
            kr = urmach_cap_verify(&bogus, RESOURCE_BLK_DEVICE, 1, 0);
            xmm_read(got);
            if (kr != CAP_ERR_INTERNAL)
                hmac_ran = 1;
            if (memcmp(pat, got, 128) != 0) {
                if (hmac_bad == 0)
                    memcpy(snap, got, 128);
                else if (memcmp(snap, got, 128) == 0)
                    stable++;
                hmac_bad++;
                for (unsigned r = 0; r < 8; r++)
                    if (memcmp(pat + 16*r, got + 16*r, 16) != 0)
                        reg_mask |= 1u << r;
            }
        }

        if (ctl_bad != 0) {
            printf("cap_test: [#395] probe INVALID — control trap clobbered "
                   "xmm %d/50 (trap round-trip does not preserve FPU)\n",
                   ctl_bad);
            pass = 0;
        } else if (!hmac_ran) {
            printf("cap_test: [#395] probe NOT ASKED — cap key not set, "
                   "HMAC never ran (#591)\n");
        } else if (hmac_bad == 0) {
            printf("cap_test: [#395] xmm preserved across cap_verify "
                   "(50/50): OK\n");
        } else {
            printf("cap_test: [#395] CONFIRMED — cap_verify clobbered xmm "
                   "%d/50, regs 0x%02x, stable %d/%d\n",
                   hmac_bad, reg_mask, stable, hmac_bad > 1 ? hmac_bad - 1 : 0);
            printf("cap_test: [#395]   xmm0 after: %02x%02x%02x%02x...\n",
                   snap[0], snap[1], snap[2], snap[3]);
            pass = 0;
        }
    }

    /*
     * Test [2]: device_open_cap with a zero-filled token must return
     * KERN_NO_ACCESS from block_device_server.  Tries the virtio-blk and
     * AHCI first-partition names in turn; if neither is registered in the
     * name server the arm says NOT ASKED rather than failing the suite:
     * whether the board carries a disk is the harness's to say (#591).
     */
    /*
     * ── The subject is chosen, not raced for (#529) ───────────────────
     *
     * 🔴 THIS USED TO TAKE WHICHEVER NAME APPEARED FIRST, and that decided
     * what arm [12] was testing.  The block server publishes the partitions
     * of its controllers one after the other ('virtio_blk0a' and then 'ahci0a'
     * under run-x86_64.sh), so a lookup landing between the two saw only one
     * of them -- and the arm's verdict became a report of when it happened to
     * look.  Twelve boots in twenty failed that way at -smp 4, on this branch
     * and on the baseline alike.
     *
     * 🔑 So each candidate gets the WHOLE budget before the next is
     * considered.  The list is a preference, not a race, and virtio-blk is
     * first on purpose: the capability path had never been exercised on it --
     * virtio-blk carried no physical DMA entry points until this same issue
     * added them -- and under run-x86_64.sh it is also the boot disk, the
     * partition every server is loaded from.  Under run-qemu.sh it is the boot
     * disk only with --virtio-first, so whether the chosen partition is the
     * boot disk is asked of "disk0a" (#592).
     *
     * ⚠️ The total wait is unchanged.  One thousand passes each rather than
     * two thousand over both, so a machine with neither disk waits exactly as
     * long as it used to and a machine with the preferred one answers just as
     * fast.
     */
    static const char * const candidates[] = { "virtio_blk0a", "ahci0a" };
    mach_port_t part_port = MACH_PORT_NULL;
    const char *found_name = NULL;
    /* Poll patiently for a BDS partition: HAL replay + MBR parse runs
     * in parallel with ipc_bench holding the CPU for several seconds.
     * The generous bound keeps the test stable across boot reordering
     * without hanging forever when no disk is present. */
    for (unsigned i = 0;
         i < sizeof(candidates) / sizeof(candidates[0]) && found_name == NULL;
         i++) {
        for (int tries = 0; tries < 1000; tries++) {
            if (netname_look_up(name_server_port, "",
                                (char *)candidates[i], &part_port) == KERN_SUCCESS) {
                found_name = candidates[i];
                break;
            }
            thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
        }
        if (found_name == NULL)
            printf("cap_test: '%s' never appeared in the name server; "
                   "trying the next candidate\n", candidates[i]);
    }

    if (found_name == NULL) {
        /* NOT ASKED, as [14] says of a missing partition below (#591). */
        printf("cap_test: [2] device_open_cap negative — NOT ASKED, no "
               "partition in the name server (#591)\n");
    } else {
        int boot = is_the_boot_disk(part_port);

        printf("cap_test: the disk arms run on '%s'%s, %s\n", found_name,
               found_name == candidates[0]
               ? "" : " (not the preferred 'virtio_blk0a')",
               boot > 0 ? "the boot disk (disk0a)"
               : boot == 0 ? "not the boot disk (disk0a is another partition)"
               : "and 'disk0a' is not registered, so whether it is the boot "
                 "disk is not known");
        char zero_tok[CAP_TOKEN_MAX];
        memset(zero_tok, 0, sizeof(zero_tok));

        security_token_t null_sec = { { 0, 0 } };
        mach_port_t dev = MACH_PORT_NULL;
        kern_return_t dkr = device_open_cap(part_port,
                                            MACH_PORT_NULL,
                                            D_READ | D_WRITE,
                                            null_sec,
                                            (char *)found_name,
                                            zero_tok,
                                            (mach_msg_type_number_t)
                                                sizeof(struct uros_cap),
                                            &dev);
        if (dkr == KERN_NO_ACCESS) {
            printf("cap_test: [2] device_open_cap(%s) zero-token "
                   "rejected: OK\n", found_name);
        } else {
            printf("cap_test: [2] device_open_cap(%s) zero-token FAIL — "
                   "expected KERN_NO_ACCESS (%d), got %d\n",
                   found_name, KERN_NO_ACCESS, (int)dkr);
            pass = 0;
        }
    }

    /*
     * Test [3]: positive device_open_cap, then explicit device_close
     * (Issue #182).  BDS must print "blk: handle ... closed" while the
     * test is still running — i.e. before [4] runs — proving the
     * release path is synchronous, not deferred to the no-senders
     * notification.  Test [4] re-issues device_close on the same name
     * and expects a graceful failure (the receive right is gone, so
     * MIG returns a send error rather than a second server invocation):
     * the protocol-level "second close is harmless" guarantee.
     */
    if (found_name != NULL) {
        struct uros_cap tok;
        kern_return_t ckr = cap_request(RESOURCE_BLK_DEVICE,
                                        cap_name_hash(found_name),
                                        CAP_OP_BLK_READ | CAP_OP_BLK_WRITE,
                                        0,
                                        &tok);
        if (ckr != KERN_SUCCESS) {
            printf("cap_test: [3] WRONG — cap_request(%s) refused (kr=%d) on "
                   "a registered partition (#591)\n", found_name, ckr);
            pass = 0;
        } else {
            char ok_blob[CAP_TOKEN_MAX];
            memcpy(ok_blob, &tok, sizeof(tok));
            security_token_t null_sec = { { 0, 0 } };
            mach_port_t handle = MACH_PORT_NULL;
            kern_return_t okr = device_open_cap(part_port,
                                                MACH_PORT_NULL,
                                                D_READ | D_WRITE,
                                                null_sec,
                                                (char *)found_name,
                                                ok_blob,
                                                (mach_msg_type_number_t)sizeof(tok),
                                                &handle);
            if (okr == KERN_SUCCESS && handle != MACH_PORT_NULL) {
                printf("cap_test: [3] positive open ok, handle=0x%x — "
                       "calling device_close\n",
                       (unsigned)handle);

                if (!a_disk_reads_into_a_foreign_page(device_port, handle,
                                                      found_name))
                    pass = 0;

                kern_return_t ckr2 = device_close(handle);
                if (ckr2 == KERN_SUCCESS) {
                    printf("cap_test: [3] device_close ok — "
                           "BDS should have logged 'handle ... closed' above\n");
                } else {
                    printf("cap_test: [3] device_close FAIL — kr=%d\n",
                           (int)ckr2);
                    pass = 0;
                }

                /*
                 * Test [4]: a second close on the same name is harmless.
                 * The receive right is gone server-side, so the MIG send
                 * is expected to fail (dead name / invalid dest) rather
                 * than re-enter ds_device_close.  Either KERN_SUCCESS or
                 * a send-side error counts as pass — what we must NOT
                 * see is BDS logging a second "handle ... closed".
                 */
                kern_return_t ckr3 = device_close(handle);
                printf("cap_test: [4] second device_close kr=%d (expected "
                       "send-error or 0; must NOT trigger a second "
                       "server-side release)\n", (int)ckr3);

                (void)mach_port_deallocate(mach_task_self(), handle);
            } else {
                printf("cap_test: [3] positive open FAIL — kr=%d\n",
                       (int)okr);
                pass = 0;
            }
        }
    }

    /*
     * Test [5]: per-handle revocation (Issue #183).  Open a fresh
     * handle on the same partition with a NEW cap_id, do a small read
     * (must succeed), call cap_revoke on that cap_id, then issue
     * another read (must fail with KERN_NO_ACCESS because BDS marked
     * the handle as revoked from the cap_revoke_notify back-channel).
     *
     * BDS is single-threaded: by the time cap_revoke returns, the
     * notify message is already in BDS's port set; the next read on
     * the same handle is processed *after* the notify in FIFO order,
     * so the revoked flag is observed without any sleep.
     */
    if (found_name != NULL) {
        struct uros_cap tok5;
        kern_return_t ck5 = cap_request(RESOURCE_BLK_DEVICE,
                                        cap_name_hash(found_name),
                                        CAP_OP_BLK_READ | CAP_OP_BLK_WRITE,
                                        0,
                                        &tok5);
        if (ck5 != KERN_SUCCESS) {
            printf("cap_test: [5] WRONG — cap_request(%s) refused (kr=%d) on "
                   "a registered partition (#591)\n", found_name, (int)ck5);
            pass = 0;
        } else {
            char blob5[CAP_TOKEN_MAX];
            memcpy(blob5, &tok5, sizeof(tok5));
            security_token_t null_sec = { { 0, 0 } };
            mach_port_t h5 = MACH_PORT_NULL;
            kern_return_t okr5 = device_open_cap(part_port,
                                                 MACH_PORT_NULL,
                                                 D_READ | D_WRITE,
                                                 null_sec,
                                                 (char *)found_name,
                                                 blob5,
                                                 (mach_msg_type_number_t)sizeof(tok5),
                                                 &h5);
            if (okr5 != KERN_SUCCESS || h5 == MACH_PORT_NULL) {
                printf("cap_test: [5] open FAIL kr=%d\n", (int)okr5);
                pass = 0;
            } else {
                /* Pre-revoke: small read must succeed. */
                io_buf_ptr_t  buf = NULL;
                mach_msg_type_number_t got = 0;
                kern_return_t rkr1 = device_read(h5, 0, 0, 512,
                                                 &buf, &got);
                if (rkr1 == KERN_SUCCESS) {
                    printf("cap_test: [5] pre-revoke read ok (%u bytes)\n",
                           (unsigned)got);
                    if (buf && got > 0)
                        (void)vm_deallocate(mach_task_self(),
                                            (vm_offset_t)buf, got);
                } else {
                    printf("cap_test: [5] pre-revoke read FAIL kr=%d\n",
                           (int)rkr1);
                    pass = 0;
                }

                /* Revoke the cap.  BDS receives cap_revoke_notify
                 * synchronously via its main port set. */
                kern_return_t rvk = cap_revoke(tok5.cap_id);
                /* Note: sa_mach printf doesn't grok %llu; print kr on
                 * its own line so the int doesn't slip on the va_list. */
                printf("cap_test: [5] cap_revoke kr=%d\n", (int)rvk);
                if (rvk != KERN_SUCCESS) {
                    printf("cap_test: [5] cap_revoke FAIL\n");
                    pass = 0;
                }

                /* Post-revoke: same handle must reject I/O. */
                io_buf_ptr_t  buf2 = NULL;
                mach_msg_type_number_t got2 = 0;
                kern_return_t rkr2 = device_read(h5, 0, 0, 512,
                                                 &buf2, &got2);
                if (rkr2 == KERN_NO_ACCESS) {
                    printf("cap_test: [5] post-revoke read rejected: OK\n");
                } else {
                    printf("cap_test: [5] post-revoke read FAIL — "
                           "expected KERN_NO_ACCESS (%d), got %d (%u bytes)\n",
                           KERN_NO_ACCESS, (int)rkr2, (unsigned)got2);
                    if (buf2 && got2 > 0)
                        (void)vm_deallocate(mach_task_self(),
                                            (vm_offset_t)buf2, got2);
                    pass = 0;
                }

                (void)device_close(h5);
                (void)mach_port_deallocate(mach_task_self(), h5);
            }
        }
    }

    if (!bad_descriptor_unwind())
        pass = 0;

    /*
     * #443.  Two stubs compiled on opposite sides of the tree.
     *
     * [7] mach_port_type is msgh_id 3204 and wants exactly 36 bytes; the
     *     kernel is the half where these checks were off from the port.
     * [8] netname_look_up is msgh_id 1041 and wants 48 upwards; name_server
     *     is userland, where they have always been on and always silent.
     *
     * Sizes and ids come from the generated stubs, and the _Static_asserts
     * #416 put around them are what keeps those numbers honest per target.
     */
    if (!mig_check_fires("[7] kernel stub (mach_port_allocate)",
                         mach_task_self(), 3204, 32))
        pass = 0;

    if (name_server_port != MACH_PORT_NULL &&
        !mig_check_fires("[8] userland stub (netname_look_up)",
                         name_server_port, 1041, 40))
        pass = 0;

    if (!subsystem_name_is_not_a_pointer())
        pass = 0;

    if (!subsystem_survives_a_failed_allocation())
        pass = 0;

    if (!a_device_has_one_driver(device_port))
        pass = 0;

    if (!the_manifest_refuses_what_it_does_not_declare())
        pass = 0;

    if (!the_master_port_bypasses_the_manifest(device_port))
        pass = 0;

    /*
     * #546.  host_port is the PRIVILEGED host port: do_bootstrap_ports builds
     * it from realhost.host_priv_self, so vm_wire is expected to accept it.
     */
    if (!a_wired_page_can_be_given_back(host_port))
        pass = 0;

    /* #552: the semaphore stubs this target never had, exercised. */
    if (!the_kernel_semaphores_answer(mach_task_self()))
        pass = 0;

    /* #708: and a timed wait under a second, which never waited. */
    if (!the_semaphore_waits_as_long_as_told(mach_task_self()))
        pass = 0;

    /* #708: a futex's, rounded down to a tick. */
    if (!a_futex_waits_as_long_as_told())
        pass = 0;

    /* #708: and mach_msg()'s past 2^32 / hz ms, wrapped. */
    if (!a_long_receive_timeout_is_long())
        pass = 0;

    /* #707: a signal in flight ends one wait, the one it was for. */
    if (!a_signal_in_flight_ends_one_wait(mach_task_self()))
        pass = 0;

    /* #597: no task reaches the configuration ports, claimed or not. */
    if (!the_pci_config_ports_are_the_kernels(device_port))
        pass = 0;

    /* #620: and no task reaches a port it has not claimed. */
    if (!a_port_no_claim_covers_is_refused(device_port))
        pass = 0;

    /* #599: a DMA buffer is freed by its owner, whole, once. */
    if (!a_buffer_is_freed_whole_and_once(device_port))
        pass = 0;

    /* #599: and its free takes its mapping, not whatever is there now. */
    if (!a_free_takes_its_mapping_not_the_address(device_port))
        pass = 0;

    /*
     * #532 runs last and on every candidate.  By this point the name server
     * has had the whole test's worth of time, so a partition that is absent
     * here is absent, not late -- and a short budget is enough to say so.
     */
    int b2_done = 0;		/* #599: the handle arms run once */
    for (unsigned i = 0;
         i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        mach_port_t p = MACH_PORT_NULL;

        if (netname_look_up(name_server_port, "", (char *)candidates[i], &p)
            != KERN_SUCCESS) {
            /*
             * 🔑 NOT ASKED, the third word, and not WRONG (#591).  This
             * binary cannot tell a board without the controller from one
             * that lost it -- i386's default board has no virtio-blk, so
             * there this line is printed for virtio_blk0a on every boot.
             * Whether the controller should be here is the harness's
             * question: it knows what it put on the board, and fails a
             * boot whose controller never attached (disks-attached.sh).
             * Said in the words the harnesses count, where it had read as
             * neither a failure nor a question set aside.
             */
            printf("cap_test: [14] '%s' NOT ASKED — not registered, so the "
                   "arm cannot run on that controller (#591)\n",
                   candidates[i]);
            continue;
        }
        if (!the_bytes_must_fit_the_pages(device_port, p, candidates[i]))
            pass = 0;
        /*
         * The writes go where nobody else holds the partition (#596): the
         * candidate, else its disk's partition c, else nowhere, said.  It
         * used to be "not the boot disk" (#592), and ext_server mounts
         * ahci0a as / by that name, so where ahci0a is not disk0a --
         * run-x86_64.sh, run-qemu.sh --virtio-first -- they landed in a
         * mounted filesystem.
         */
        {
            mach_port_t tport = MACH_PORT_NULL;
            char        tname[32] = "";

            if (scratch_target(p, candidates[i], &tport, tname,
                               sizeof(tname))
                && the_write_half(device_port, tport, tname) == 0)
                pass = 0;
            if (!a_handle_moves_data_only_its_way(device_port, p,
                                                  candidates[i], tport,
                                                  tname))
                pass = 0;		/* #614 */
            if (tport != MACH_PORT_NULL && tport != p)
                (void)mach_port_deallocate(mach_task_self(), tport);
        }
        /* #537 step 3: on the controller behind the IOMMU, whichever is first. */
        if (strcmp(candidates[i], "ahci0a") == 0 &&
            !a_revoked_translation_is_refused(device_port, p, candidates[i]))
            pass = 0;
        /* #599: once, on the first candidate that is there. */
        if (!b2_done) {
            if (!a_page_nobody_granted_is_refused(device_port, p,
                                                  candidates[i]))
                pass = 0;
            if (!a_capability_is_its_handles(device_port, p, candidates[i]))
                pass = 0;
            if (!revoking_a_capability_takes_the_mapping_down(device_port, p,
                                                              candidates[i]))
                pass = 0;
            if (!a_handle_refuses_rather_than_evicts(device_port, p,
                                                     candidates[i]))
                pass = 0;
            if (!a_buffer_capability_is_a_buffers(device_port, p,
                                                  candidates[i]))
                pass = 0;
            if (!many_pages_in_one_request(device_port, p, candidates[i]))
                pass = 0;		/* #537 */
            b2_done = 1;
        }
        (void)mach_port_deallocate(mach_task_self(), p);
    }

    if (pass) {
        printf("cap_test: ALL TESTS PASSED\n");
        return 0;
    }
    printf("cap_test: SOME TESTS FAILED\n");
    return 1;
}
