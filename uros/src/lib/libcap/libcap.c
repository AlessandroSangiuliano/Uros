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
 * libcap.c — client-side wrappers around the cap_server MIG interface.
 *
 * Thread-safety: cap_server_port() caches the port after first lookup
 * with no lock — concurrent callers may race on the first assignment
 * but will all converge to the same port send right, and the race is
 * harmless since netname_look_up is idempotent.
 */

#include <stddef.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach_init.h>
#include <mach/cap_types.h>
#include <mach/mach_traps.h>	/* the cap traps, declared once (#426) */

#include "libcap.h"

/*
 * MIG-generated user stub prototypes (from cap_server.defs).  Included
 * via the build system's generated header search path.
 */
#include "cap_server.h"

/*
 * netname_look_up: return a send right for the named service.
 */
extern kern_return_t netname_look_up(mach_port_t server_port,
                                     char *host_name,
                                     char *service_name,
                                     mach_port_t *service_port);

#define CAP_SERVER_NETNAME   "cap_server"

static mach_port_t cached_cap_port = MACH_PORT_NULL;

mach_port_t
cap_server_port(void)
{
    mach_port_t port;
    kern_return_t kr;

    if (cached_cap_port != MACH_PORT_NULL)
        return cached_cap_port;

    /*
     * #216 v2.1 — prefer the per-task cap_port stamped by bootstrap
     * (TASK_CAP_PORT, exposed by libmach as mach_cap_port).  Manifest
     * enforcement runs on this path.  Fall back to the well-known
     * netname-looked-up cap_server (legacy / permissive) when no
     * per-task port was provisioned: bootstrap itself, unmigrated
     * servers, or tasks created outside the bootstrap-managed
     * lineage.
     */
    if (mach_cap_port != MACH_PORT_NULL) {
        cached_cap_port = mach_cap_port;
        return cached_cap_port;
    }

    kr = netname_look_up(name_server_port, "",
                         (char *)CAP_SERVER_NETNAME, &port);
    if (kr != KERN_SUCCESS)
        return MACH_PORT_NULL;

    cached_cap_port = port;
    return port;
}

/*
 * Helpers: the on-wire blob is sizeof(struct uros_cap).  We memcpy
 * between the struct and the byte array to keep the public API typed
 * while matching the opaque MIG transport.
 */

kern_return_t
cap_request(uint32_t resource_type,
            uint64_t resource_id,
            uint64_t ops,
            uint32_t lifetime_ms,
            struct uros_cap *out)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    if (out == NULL) return CAP_ERR_INVALID_TOKEN;

    char buf[CAP_TOKEN_MAX];
    mach_msg_type_number_t buflen = CAP_TOKEN_MAX;

    kern_return_t kr = mig_cap_acquire(srv, resource_type, resource_id, ops,
                                       lifetime_ms, buf, &buflen);
    if (kr != KERN_SUCCESS) return kr;
    if (buflen != sizeof(struct uros_cap)) return CAP_ERR_INVALID_TOKEN;

    memcpy(out, buf, sizeof(struct uros_cap));
    return KERN_SUCCESS;
}

kern_return_t
__attribute__((weak)) cap_derive(const struct uros_cap *parent,
           uint64_t reduced_ops,
           uint32_t lifetime_ms,
           struct uros_cap *out)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    if (parent == NULL || out == NULL) return CAP_ERR_INVALID_TOKEN;

    char in_buf[CAP_TOKEN_MAX];
    char out_buf[CAP_TOKEN_MAX];
    mach_msg_type_number_t out_len = CAP_TOKEN_MAX;

    memcpy(in_buf, parent, sizeof(struct uros_cap));

    kern_return_t kr = mig_cap_derive(srv, in_buf,
                                      (mach_msg_type_number_t)sizeof(struct uros_cap),
                                      reduced_ops, lifetime_ms,
                                      out_buf, &out_len);
    if (kr != KERN_SUCCESS) return kr;
    if (out_len != sizeof(struct uros_cap)) return CAP_ERR_INVALID_TOKEN;

    memcpy(out, out_buf, sizeof(struct uros_cap));
    return KERN_SUCCESS;
}

kern_return_t
__attribute__((weak)) cap_revoke(uint64_t cap_id)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    return mig_cap_revoke(srv, cap_id);
}

kern_return_t
__attribute__((weak)) cap_subscribe_revoke(mach_port_t notify_port)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    /*
     * mach_port_send_t in MIG expands to (port, type) at the wire.
     * The user gives us a send right; pass it as MOVE_SEND so the
     * kernel transfers ownership and we don't keep a stale copy.
     */
    return mig_cap_subscribe_revoke(srv, notify_port,
                                    MACH_MSG_TYPE_COPY_SEND);
}

kern_return_t
__attribute__((weak)) cap_verify(const struct uros_cap *token, uint64_t op, uint64_t resource_id)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    if (token == NULL) return CAP_ERR_INVALID_TOKEN;

    char buf[CAP_TOKEN_MAX];
    memcpy(buf, token, sizeof(struct uros_cap));

    return mig_cap_verify(srv, buf,
                          (mach_msg_type_number_t)sizeof(struct uros_cap),
                          op, resource_id);
}

/*
 * ⚠️ The declarations of urmach_cap_verify and urmach_cap_use used to be
 * HERE, with a comment saying no kernel-facing header shipped them.  That was
 * true and it was the problem: a token is a pointer, the other half of these
 * calls is an assembly stub that carries no types, and nothing anywhere
 * compared this file's idea of them with the kernel's.  They are declared in
 * <mach/mach_traps.h> now -- which kern/syscall_sw.c also includes, so the
 * trap table and this call site finally see one prototype (#426, #448).
 */

kern_return_t
cap_verify_local(const struct uros_cap *token,
                 uint32_t op,
                 uint64_t resource_id)
{
    if (token == NULL) return CAP_ERR_INVALID_TOKEN;
    return urmach_cap_verify(token, op, resource_id);
}

kern_return_t
cap_use_local(const struct uros_cap *token,
              uint32_t op,
              uint64_t resource_id)
{
    if (token == NULL) return CAP_ERR_INVALID_TOKEN;
    return urmach_cap_use(token, op, resource_id);
}

/*
 * ⚠️ Hand-written, and it shadows the generated one rather than agreeing with
 * it -- so it went stale the moment the stub's signature moved.  #432 made an
 * in-line argument sent IN a `const' pointer, because that is what the stub
 * does with it, and this declaration said `char *' and turned a corrected
 * prototype into a link-time contradiction the compiler caught.
 *
 * 🔑 Kept and corrected rather than replaced with the generated header,
 * because including it here would pull cap_server's whole interface into a
 * library that wants one routine out of it.  The cost of that choice is
 * exactly this: a second copy of a signature, which is a thing that has to be
 * corrected in two places forever.
 */
extern kern_return_t mig_cap_provision_task(mach_port_t cap_server,
                                            mach_port_t task_port,
                                            const char *manifest,
                                            mach_msg_type_number_t manifestCnt,
                                            mach_port_t *task_cap_port);

kern_return_t
cap_provision(mach_port_t task_port,
              const void *manifest_blob,
              unsigned int manifest_len,
              mach_port_t *out_task_cap_port)
{
    mach_port_t srv = cap_server_port();
    if (srv == MACH_PORT_NULL) return CAP_ERR_INTERNAL;
    if (!manifest_blob || manifest_len == 0 || !out_task_cap_port)
        return CAP_ERR_INVALID_TOKEN;

    *out_task_cap_port = MACH_PORT_NULL;
    return mig_cap_provision_task(srv, task_port,
                                  (char *)manifest_blob,
                                  (mach_msg_type_number_t)manifest_len,
                                  out_task_cap_port);
}
