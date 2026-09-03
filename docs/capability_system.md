# The Uros Capability System

How authority is named, granted, checked and withdrawn.

This document describes **what is built**, not what is designed. The design
documents (`Uros_tecnico_v6.pdf`, `Uros_riferimento.md`) describe a larger
system — 64 shards, four cap_server threads, four policy layers intersected.
None of that is here yet. Where the two differ, this file is the one that
matches the source, and it says so at each point.

Source:

- `uros/uapi/mach/cap_types.h` — the token, the resource types, the ops
- `uros/uapi/mach/cap_manifest.h` — the compiled policy file
- `uros/uapi/mach/cap_server.defs` — the RPCs
- `uros/uapi/mach/syscall_sw.h`, `mach_traps.h` — the traps
- `uros/src/mach_kernel/kern/cap.c` — the kernel half
- `uros/src/servers/cap_server/` — the issuing server
- `uros/src/tools/mkmanifest/` — the policy compiler

---

## 1. The idea in one page

A capability is a **token**: a struct that names a resource, the operations
permitted on it, and carries an HMAC-SHA256 over its own fields, signed with a
key only the kernel and cap_server know.

Two properties follow, and they are the whole design:

**It is autoverifying.** Any server can check a token with a single syscall —
no lookup, no RPC back to cap_server, no shared table. The token carries its
own rights and its own proof.

**Therefore verification is decentralised.** cap_server *issues*; whoever owns
the resource *checks*. The block server checks block-device tokens; the device
master checks PCI-device and DMA-buffer tokens; a filesystem server would check
file tokens. There is no central chokepoint on the use of authority, only on
its creation.

⚠️ The one thing a token cannot carry is **what happened after it was signed**.
That is why the kernel keeps a small revocation table, and why it is the only
table on the checking path.

---

## 2. The actors

| actor | holds | does |
|---|---|---|
| **kernel** (`kern/cap.c`) | the HMAC key, a revocation table, the facts about resources it created | verifies tokens; answers ownership questions; tears down materialised authority on revocation |
| **cap_server** | the issuance table (by id / by owner / by parent), the per-task manifests | applies policy and mints tokens; cascades revocation |
| **bootstrap** | the boot bundle | installs each task's manifest before the task runs |
| **resource servers** (block, device master, …) | nothing special | check tokens presented to them |
| **client tasks** | their own tokens | request, delegate, present |

🔑 **Identity is a port.** cap_server knows a requester as the port its message
arrived on, not as a task id or a name. `cap_provision_task` returns a fresh
per-task `cap_port`; only the holder of that send right lands on the matching
receive port, so the port name *is* the principal. There is no "who is calling"
question to answer.

---

## 3. The token

```c
struct uros_cap {
    uint64_t  cap_id;                /* unique, assigned by cap_server     */
    uint32_t  resource_type;         /* cap_resource_type_t                */
    uint32_t  _pad0;
    uint64_t  resource_id;           /* which one, per-type meaning        */

    uint64_t  allowed_ops;           /* bitmask, per-type namespace        */
    /* … owner name, delegation tree links, expiry, max_uses, revoked …    */

    uint8_t   hmac[32];              /* MUST be last: the input is every
                                        preceding byte, verbatim           */
};
```

`hmac[]` is last because the HMAC input is the bytes before it. Field order is
wire format: the blob crosses MIG as `cap_token_t`, an opaque
`array[*:CAP_TOKEN_MAX] of char`, so the struct can evolve without regenerating
every stub.

### Resource types and what `resource_id` means

| type | `resource_id` is | checked by |
|---|---|---|
| `RESOURCE_FILE`, `DIRECTORY`, `MOUNT_POINT` | reserved | a filesystem server |
| `RESOURCE_BLK_DEVICE` (4) | the partition | block_device_server |
| `RESOURCE_PCI_DEVICE` (5) | **the device CLASS**, not the bus address | the device master |
| `RESOURCE_DMA_BUFFER` (10) | the kernel's region id | the device master |
| `RESOURCE_BUDGET` (9) | reserved | — |

🔑 **`RESOURCE_PCI_DEVICE` names a class and not a bus address**, and that is a
decision worth understanding. A bus/device/function is a property of the
*board*: this tree's AHCI controller is `00:04.0` under one QEMU command line
and `00:1f.2` under another, and different again on real hardware. A policy
file naming one would have to be rewritten per machine — wrong somewhere by
construction. A class code (`0x010601` = SATA/AHCI) is a property of the device
*kind*, so one manifest works everywhere.

⚠️ The consequence is that the manifest is only **half** the check. It says what
kind of device a driver may drive; it cannot say which instance, because it
does not know what is plugged in. The other half is the device master's
**claim** — see §6.

---

## 4. The policy: manifests

A manifest is a text file compiled by `mkmanifest` into a `.cmf` blob:

```
name cap_test

# type  id    ops
required 4    any   0x3      # block devices, read|write
required 10   any   0x3      # DMA buffers, device read|write
```

Compiled entries are `{type, resource_id, ops}` — 24 bytes,
`CAP_MANIFEST_VERSION 2`.

🔴 **`resource_id` is the v2 addition and it is what makes a manifest a
policy.** Version 1 entries were `{type, ops}`: they could say "this task may
acquire block-device capabilities with READ" and could *not* say which block
device. A task allowed one was allowed all of them — the file constrained the
*kind* of authority without constraining its *extent*. For devices that would
have meant a driver entitled to one PCI device being entitled to every PCI
device in the machine.

⚠️ `CAP_MANIFEST_ANY_ID` is `~0ULL` and **not zero**, because zero is a
plausible resource id — device `00:00.0`, class `0x000000`, the first of
anything. An entry that forgot the field would otherwise have named a real
resource and been quietly *narrower* than its author meant.

⚠️ `mkmanifest` **refuses** a two-field line rather than reading it as "any". A
v1 file describes a different world; compiling it silently would produce a
permission wider than its author believed.

### How a manifest reaches a task

```
bootstrap creates the child task
   └─ looks up "<symtab>.cmf" in the boot bundle
       └─ cap_provision_task(cap_server, task_port, blob) ──▶ cap_server
           └─ validates, installs in the per-task table keyed by a NEW port
           ◀── returns a send right to that port
   └─ task_set_special_port(child, TASK_CAP_PORT, that port)
```

The manifest comes from the **bundle**, from a source the task cannot alter,
and is handed to cap_server directly — the child never sees it pass.

⚠️ A task with **no** manifest falls back to the well-known cap_server port and
the permissive path. So *enforcement is per-task*, and a task nobody wrote a
policy for is a task with no policy.

⚠️ Every `.cmf` must be shipped in the bundle **and** the build rule must depend
on the compiled **file**, not on the target that produces it. In Ninja a target
dependency is an ordering dependency: the bundle will not notice the file
changed. A policy that is compiled and not shipped is worse than none, because
it looks like one.

---

## 5. The interfaces

### RPCs — `cap_server.defs`, subsystem 3500

| routine | who calls it | what it does |
|---|---|---|
| `cap_acquire(type, id, ops, lifetime) → token` | any task | policy check, then mint |
| `cap_derive(parent, reduced_ops) → token` | a holder | delegate a weaker child |
| `cap_revoke(cap_id)` | a holder / an authority | revoke it and its whole subtree |
| `cap_verify(token, op, id)` | anyone | slow path; the trap is the fast one |
| `cap_subscribe_revoke(port)` | a resource server | receive `cap_revoke_notify` |
| `cap_provision_task(task_port, manifest) → cap_port` | bootstrap only (by convention) | install a policy, return the principal port |

### Traps — `syscall_sw.h`

Plain syscalls, **not** Mach messages. That is not an optimisation: cap_server
calls two of them *inside its own message loop with a client waiting*, and a
server that had to send a message to answer a message can deadlock against
itself.

| trap | slot | who | question |
|---|---|---|---|
| `urmach_cap_verify(token, op, id)` | 37 | any resource server | is this token good for this? |
| `urmach_cap_use(token, op, id)` | 38 | ditto | …and consume one use |
| `urmach_cap_revoke(cap_id)` | 39 | cap_server only | mark it dead in the kernel |
| `urmach_cap_register(setup)` | 40 | cap_server once | hand the kernel the HMAC key |
| `urmach_dma_region_owner(id, task)` | 43 | cap_server | does that task own this DMA buffer? |

---

## 6. Control flow

### 6.1 Issuing

```
client ── cap_acquire(type, id, ops) ──▶ cap_server
                                          │
                          which port did this arrive on?
                                          │
                            ┌─────────────┴──────────────┐
                     per-task port                 well-known port
                            │                             │
                  look up the manifest            no manifest —
                            │                     permissive (legacy)
                  cap_manifest_allows(type, id, ops)?
                            │
              ┌─────────────┴─────────────┐
        type == DMA_BUFFER?           everything else
              │                            │
   ask the KERNEL who owns it        the manifest decides alone
   urmach_dma_region_owner(id, task)
              │
        both must say yes
                            │
                    mint, sign, record in cap_table
                            │
                            ▼
                        token to the client
```

🔑 **Why the DMA-buffer branch exists.** A manifest can name a device class or a
mount point because those exist before the policy is written. It cannot name a
buffer that will exist for four milliseconds. So the manifest says whether the
task may hold buffer capabilities *at all* — declared with `any`, the only
honest thing it can say — and the kernel says whether *this* buffer is that
task's to give away, which it knows because it made the allocation.

⚠️ The bridge between the two halves is the **task port** that
`cap_provision_task` carries. cap_server knows a requester as a port; the kernel
knows an owner as a task; the port is what turns one into the other.

### 6.2 Checking

```
holder ── presents token ──▶ resource server
                               │
                     urmach_cap_verify(token, op, resource_id)   [trap]
                               │
                        ┌──────┴──────┐
                    HMAC good?   resource_id matches?
                    ops cover the request?   token->revoked clear?
                               │
                     kernel revocation table: was cap_id revoked?
                               │
                          KERN_SUCCESS
```

Four comparisons and a hash. The only table touched is the kernel's revocation
table, and only because that is the one fact a signed token cannot carry.

⚠️ `token->revoked` is in the **holder's copy** and proves nothing against a
malicious holder. The kernel's table is the real defence.

### 6.3 Revoking

```
somebody ── cap_revoke(cap_id) ──▶ cap_server
                                     │
                    walk the delegation tree (by_parent), for each:
                                     │
                     ┌───────────────┼────────────────┐
              mark own copy    urmach_cap_revoke  cap_revoke_notify
                               (trap, kernel)     (IPC, subscribers)
                                     │
                        device_master_cap_revoked(cap_id)
                                     │
                     drop the claim; leave the device BLOCKED
```

🔴 **The trap is the half that cannot wait for a message.** Some authority is
*materialised*: a capability for a PCI device becomes an IOMMU domain, which is
a page table the device walks without consulting anything. Revoking the token
changes nothing there — the withdrawal has to reach the *mapping*, and the
kernel is where that can happen immediately.

🔴 **Blocked, not pass-through.** Restoring the entry the device started in
would make the revocation *give* it all of memory: a widening dressed as a
withdrawal, and one somebody trusted.

---

## 7. Two families of authority

This matters for reasoning about the system, and it is the question the design
is easiest to get wrong about.

**Mediated invocation** — every use goes through the guard. A Mach port works
this way: the kernel is inside every message. Revoking is easy: refuse the next
use.

**Materialised mapping** — the capability is presented once, a mapping is
installed, and the *hardware* enforces every access afterwards. Memory has
always been this: present a capability for a page, the kernel writes a PTE, and
the MMU checks every access without anyone calling back. An **IOMMU domain is a
page table**, so device DMA authority is this family — and it is *stronger*
than a software guard, not weaker, because the check happens on every access at
hardware speed.

🔑 What the second family owes is **revocation by teardown**. Without it,
authority that cannot be withdrawn is not authority that was granted; it is
authority that was released. §6.3 is that obligation being paid.

⚠️ A corollary worth stating plainly: **a device transfer is checked by
nothing in software**, because it does not reach the kernel at all. A userspace
driver reads a disk by writing an address into the device's own descriptor and
poking a register — no system call, nothing to see, nowhere to put a check.
That is not a gap; it is why the IOMMU is required.

---

## 8. Worked example: a browser

A browser is a good subject because it does the two things this system has to
get right at once: it talks to the network constantly, and it writes to disk
constantly — cache, cookies, session state, IndexedDB, the profile — while
being the least trustworthy program on the machine.

### 8.1 What the manifest says

```
name browser

# Its own profile directory, and nothing else on the filesystem.
required 2  0x00000000c0ffee01  0x3     # DIRECTORY ~/.browser, read|write

# One TCP endpoint capability, from the network server.
required 7  any                 0x3     # SERIAL/socket-like, read|write

# It may hold DMA buffers?  NO.  There is no line here for type 10.
```

🔴 **The absent lines are the policy.** A browser has no business driving a PCI
device or handing memory to one. `cap_acquire(RESOURCE_PCI_DEVICE, …)` from
this task comes back `CAP_ERR_NOT_IN_MANIFEST`, and it does so at cap_server
before any server is involved.

⚠️ The directory is named by **id** and not `any`. Files and directories are
one of the resource types where the policy *can* name the instance, because the
profile path exists before the browser runs. Compare with §3: a device class
must be `any` at the instance level and a DMA buffer must be `any` entirely —
what a manifest can pin down is a property of the resource type, not a style
choice.

### 8.2 Starting up

```
bootstrap ── browser.cmf ──▶ cap_server ──▶ per-task cap_port
                                              │
browser ── cap_acquire(DIRECTORY, profile, READ|WRITE) ──▶ token_dir
browser ── cap_open(token_dir, "cache/index") ─────────▶ fs server
                                       fs: urmach_cap_verify(token_dir,
                                                 CAP_OP_FILE_WRITE, profile)
```

The filesystem server checks the token itself, with a trap. cap_server is not
in this path — it was only in the path once, at issue time.

### 8.3 The disk writes, which are constant

The browser writes through the filesystem server, which writes through the
block device server, which programs the disk. Three tasks, and the interesting
part is that **the browser's pages are not the disk's to reach**.

```
browser        fs server              block server            kernel/IOMMU
   │               │                       │                       │
   │  write(...)   │                       │                       │
   ├──────────────▶│                       │                       │
   │               │ device_dma_alloc_sg(NO_BDF) ──────────────────▶│
   │               │◀── pages + region_id ─────────────────────────│
   │               │                       │                       │
   │               │ cap_acquire(DMA_BUFFER, region_id, R|W)       │
   │               │        ──▶ cap_server ──[trap]──▶ owner? ─────▶│
   │               │◀── token_buf ─────────────────────────────────│
   │               │                       │                       │
   │               │ device_register_dma(disk, token_buf) ─────────▶│
   │               │                       │                       │
   │               │ device_write_phys(disk, lba, [phys addrs]) ───▶│
   │               │                       │ map_foreign(bdf, pa,   │
   │               │                       │             token_buf)─▶
   │               │                       │◀── IOVA ───────────────│
   │               │                       │ programs the PRDT      │
   │               │                       │ ═══ DMA ══════════════▶ disk
```

🔑 **Four separate authorities, and none of them is "holds a port".**

1. The filesystem server may hold DMA buffers — its *manifest* says so.
2. *This* buffer is its to give away — the *kernel* says so, because it made
   the allocation.
3. The block server may drive that disk controller — its *device capability*
   names the class, and the kernel read the class off the hardware.
4. The controller may touch those pages — only because the filesystem server
   **handed over** `token_buf`, and only for the pages in that region.

⚠️ Note what step 4 replaces. Before it, the block server presented a physical
address and the kernel checked only that the kernel had allocated it for DMA —
which is *every other server's buffers*. The block server could have put the
browser's page cache inside the disk's reach without the browser or the
filesystem ever having offered it. The capability is what turns "I know where
it is" into "somebody gave it to me".

### 8.4 The network, and why it looks the same

The browser has no network *device* capability and never will. It holds an
endpoint capability from the network server; the network server holds the
device capability for the NIC and its own DMA buffers. The same four-step
shape, with a different card at the end.

🔑 So a compromised browser has: its profile directory, its sockets, and no way
to name a device, no way to obtain a DMA buffer, and nothing to hand to a
driver. Its authority is exactly the list in its manifest, and each item was
checked by whoever owned the thing.

### 8.5 Revoking, mid-write

Suppose the browser is found to be compromised while a write is in flight.

```
authority ── cap_revoke(token_dir.cap_id) ──▶ cap_server
                                               ├─ cascade: every derived child
                                               ├─ urmach_cap_revoke  [trap] ──▶ kernel table
                                               └─ cap_revoke_notify ─────────▶ fs server
                                                                                 │
                                                          fs: closes live handles
```

The next `urmach_cap_verify` on any of those tokens fails, wherever it is
presented, without cap_server being consulted.

⚠️ **The DMA buffer is the interesting case.** Revoking `token_buf` reaches
`device_master_cap_revoked` through the trap, which un-maps the region from the
disk controller's domain — so the transfer in flight is refused **by the
engine**, not by a check somebody remembered to write. That is the materialised
family being withdrawn properly, and it is the reason §6.3 exists.

---

## 9. What is NOT built

Stated so nobody reads this document as a description of a finished system.

- **cap_server's policy is one layer, not four.** `policy_allows_v1` is
  permissive; the manifest layer is the only real gate. User and runtime layers
  from the design docs do not exist.
- **cap_server is single-shard and single-threaded.** No 64 shards, no revoke /
  snapshot / policy threads.
- **`cap_provision_task` is not gated to bootstrap.** Any caller can invoke it;
  the setup-port pattern that would restrict it is a follow-up.
- **Most resource types have no server checking them.** Only
  `RESOURCE_BLK_DEVICE`, `RESOURCE_PCI_DEVICE` and `RESOURCE_DMA_BUFFER` are
  verified anywhere today. `FILE`, `DIRECTORY`, `MOUNT_POINT`, `PRINTER`,
  `SERIAL`, `FRAMEBUFFER`, `BUDGET` are reserved values with no enforcement
  behind them — the browser example above is therefore *architecture*, not a
  transcript.
- **Manifests are shipped for one task.** `cap_test` on both targets. Everything
  else runs on the permissive path.
- **Budgets do not exist.** `RESOURCE_BUDGET` is a reserved number.

## 10. Where the checks can be seen to fail

Every guard described here has an arm that fails when the guard is removed;
this is where to look, and what to break to confirm it still works.

| check | arm | ablate by |
|---|---|---|
| a device has one driver | `cap_test [11]` | removing `check_claim` from `device_dma_alloc` |
| a client's buffer needs a capability | `cap_test [12]` | not calling `device_register_dma` |
| the manifest refuses what it does not declare | `cap_test [13]` | shipping no `.cmf` |
| revocation tears the mapping down | `blk: gave 0:31.2 back …` | removing `urmach_cap_revoke` from cap_server's `cap_revoke` |

⚠️ `[11]` asks `device_dma_owned` *before* trying, because a test written as
"try it and see" would claim any device nobody had claimed yet — a self-test
breaking the thing it checks. And the revocation arm proves itself by being
**refused a DMA buffer**, not by asking who owns the device: the block server
*was* the owner, so that question answered "no" either way and read as a pass
for one run.
