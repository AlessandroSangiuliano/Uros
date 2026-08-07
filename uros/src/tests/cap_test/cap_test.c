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
#include <mach/bootstrap.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <device/device.h>
#include <device/device_types.h>
#include <libcap.h>
#include "sha256.h"     /* Issue #180: SHA-NI dispatch query + KAT */

/*
 * Trap stub emitted by libmach (mach_traps.s): slot 40.
 */
extern kern_return_t urmach_cap_register(struct uros_cap *token);
extern kern_return_t urmach_cap_verify(const struct uros_cap *token,
                                       uint32_t op, uint64_t resource_id);

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
            kr = urmach_cap_verify(&bogus, 1, 0);
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
            printf("cap_test: [#395] probe SKIPPED — cap key not set, "
                   "HMAC never ran (proves nothing)\n");
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
     * KERN_NO_ACCESS from block_device_server.  Tries the AHCI and
     * virtio-blk first-partition names in turn; if neither is
     * registered in the name server the test is skipped without
     * failing the suite (boot may not have published partitions yet
     * when cap_test runs).
     */
    static const char * const candidates[] = { "ahci0a", "virtio_blk0a" };
    mach_port_t part_port = MACH_PORT_NULL;
    const char *found_name = NULL;
    /* Poll patiently for a BDS partition: HAL replay + MBR parse runs
     * in parallel with ipc_bench holding the CPU for several seconds.
     * The generous bound keeps the test stable across boot reordering
     * without hanging forever when no disk is present. */
    for (int tries = 0; tries < 2000 && found_name == NULL; tries++) {
        for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (netname_look_up(name_server_port, "",
                                (char *)candidates[i], &part_port) == KERN_SUCCESS) {
                found_name = candidates[i];
                break;
            }
        }
        if (found_name == NULL)
            thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
    }

    if (found_name == NULL) {
        printf("cap_test: [2] device_open_cap negative — "
               "SKIPPED (no BDS partition in name server)\n");
    } else {
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
            printf("cap_test: [3] cap_request(%s) failed (%d) — SKIPPED\n",
                   found_name, ckr);
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
            printf("cap_test: [5] cap_request FAIL kr=%d — SKIPPED\n",
                   (int)ck5);
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

    if (pass) {
        printf("cap_test: ALL TESTS PASSED\n");
        return 0;
    }
    printf("cap_test: SOME TESTS FAILED\n");
    return 1;
}
