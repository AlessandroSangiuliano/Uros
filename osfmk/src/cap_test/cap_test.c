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
#include <mach/bootstrap.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <device/device.h>
#include <device/device_types.h>

/*
 * Trap stub emitted by libmach (mach_traps.s): slot 40.
 */
extern kern_return_t urmach_cap_register(struct uros_cap *token);

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

    int pass = 1;
    if (kr == CAP_ERR_UNAUTHORIZED) {
        printf("cap_test: [1] unauthorized register rejected: OK\n");
    } else {
        printf("cap_test: [1] unauthorized register FAIL — "
               "expected CAP_ERR_UNAUTHORIZED (%d), got %d\n",
               CAP_ERR_UNAUTHORIZED, (int)kr);
        pass = 0;
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
    /* Poll up to ~20 s for a BDS partition to show up in the name
     * server.  BDS publishes partitions only after HAL replay + MBR
     * parse, and in parallel ipc_bench holds the CPU for a few
     * seconds; the generous bound keeps the test stable across boot
     * reordering without hanging forever when no disk is present. */
    for (int tries = 0; tries < 400 && found_name == NULL; tries++) {
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

    if (pass) {
        printf("cap_test: ALL TESTS PASSED\n");
        return 0;
    }
    printf("cap_test: SOME TESTS FAILED\n");
    return 1;
}
