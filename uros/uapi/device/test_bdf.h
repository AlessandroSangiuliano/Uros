/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Which PCI device the tests are allowed to take, written down once (#533).
 *
 * ── Why this file exists ──────────────────────────────────────────────
 *
 * 🔴 TWO PROGRAMMES REASONED THEIR WAY TO 0:0.0 AND USED IT FOR OPPOSITE
 * THINGS.  block_device_server wanted a device that is present and never
 * claimed, as the control half of "the registry reports a value a client set,
 * and one it did not".  dma_reclaim_test wanted a device it could claim and
 * then die holding, and picked the one whose loss cannot take the boot disk
 * with it.  Both wrote out the same argument -- the host bridge is on every
 * board and no driver will ever want it -- and both were right about every
 * DRIVER and wrong about the other TEST.
 *
 * The two files were written months apart and neither mentioned the other, so
 * the collision was found as a failure rate: three boots in twenty at -smp 4,
 * reported for months as noise.
 *
 * 🔑 And 0:0.0 is not an arbitrary choice either of them could simply give up.
 * It is the ONLY function guaranteed on both boards this project keeps: on
 * i440fx the host bridge is 0x1237 and on q35 it is 0x29c0, while everything
 * else moves -- the LPC bridge is at 00:01.0 on one and 00:1f.0 on the other.
 * So "just use a different address" would trade a collision for a test that
 * silently does not run on one of the two machines.
 *
 * ── What this file settles, and what it does not ──────────────────────
 *
 * It names the device reserved for tests that CLAIM one.  Everything else must
 * treat that device as possibly claimed at any moment, and check rather than
 * assume -- which is the half block_device_server was missing: it read the
 * control's state and compared it, but never asked whether the control was
 * free to be a control in the first place.
 *
 * ⚠️ It does not make the collision impossible, and saying so is the point.
 * A second test that claims a device must claim THIS one, and anything using
 * it as a fixed point must verify its premise.  A constant cannot enforce
 * that; what it can do is make sure the next person to need "a device nobody
 * wants" finds the answer here instead of deriving it for the third time.
 */

#ifndef	_DEVICE_TEST_BDF_H_
#define	_DEVICE_TEST_BDF_H_

/*
 * The host bridge: present on every board either target runs on, driven by
 * nothing, and the one device whose claim being mishandled cannot cost the
 * boot disk.
 */
#define UROS_TEST_CLAIM_BUS	0u
#define UROS_TEST_CLAIM_SLOT	0u
#define UROS_TEST_CLAIM_FUNC	0u

/* The same thing packed the way device_claim() and the manifests want it. */
#define UROS_TEST_CLAIM_BDF	0u

/* Its class, which the manifest names and the kernel re-reads from the
 * hardware -- bridges, base class 6. */
#define UROS_TEST_CLAIM_CLASS	0x060000ULL

#endif	/* _DEVICE_TEST_BDF_H_ */
