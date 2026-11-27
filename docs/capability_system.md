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
| **kernel** (`kern/cap.c`) | the HMAC key, which task may install it, a revocation table and its epoch, the facts about resources it created | verifies tokens; answers ownership questions; tears down materialised authority on revocation; gives every task an identity of its own and stamps it in the trailer of what the task sends (#747) |
| **cap_server** | the issuance table, by id (#974: the indexes by owner and by parent were never read), the per-task manifests, a watch on each task it issued to or lent through (#983) | draws the key; applies policy and mints tokens; cascades revocation; ends a task's capabilities when it dies (§6.6) |
| **bootstrap** | the boot bundle, cap_server's port | makes cap_server's port and names its task to the kernel; installs each task's manifest before the task runs |
| **name server** | the names, the mount registry | publishes "cap_server" for bootstrap's port; checks a mount in only on a capability for its prefix |
| **resource servers** (block, device master, name server, …) | nothing special | check tokens presented to them |
| **client tasks** | their own tokens | request, delegate, present |

🔑 **Who cap_server is, is bootstrap's to say (#744).** Bootstrap creates the
port cap_server serves and hands it over through `service_checkin` to that
task alone; the name server publishes "cap_server" for it before any task can
ask, so the name cannot be taken first; and bootstrap names cap_server's task
to the kernel (`urmach_cap_server_set`, on the privileged host port), which
takes the key from that task and from no other. Before, the name went to
whichever task checked it in first, with a null signature, and the key to
whichever task sent one first.

🔑 **Identity is a port.** cap_server knows a requester as the port its message
arrived on, not as a task id or a name. `cap_provision_task` and
`cap_derive_task` return a fresh per-task `cap_port`; only the holder of that
send right lands on the matching receive port, so the port name *is* the
principal. There is no "who is calling" question to answer — and for the same
reason who may provision is decided by a port too, cap_server's provisioning
port, whose one send right is bootstrap's (#743).

---

## 3. The token

```c
struct uros_cap {
    uint64_t  cap_id;                /* unique, assigned by cap_server     */
    uint32_t  resource_type;         /* cap_resource_type_t                */
    uint32_t  _pad0;
    uint64_t  resource_id;           /* which one, per-type meaning        */

    uint64_t  allowed_ops;           /* bitmask, per-type namespace        */
    uint64_t  owner;                 /* the task it was issued to (#747)   */
    /* … delegation tree links, expiry, revoked …                           */

    uint8_t   hmac[32];              /* MUST be last: the input is every
                                        preceding byte, verbatim           */
};
```

`hmac[]` is last because the HMAC input is the bytes before it. Field order is
wire format: the blob crosses MIG as `cap_token_t`, an opaque
`array[*:CAP_TOKEN_MAX] of char`, so the struct can evolve without regenerating
every stub. It is format 2 (#747), 120 bytes with the hmac at 88 on both
targets; the two static assertions in `cap_types.h` name the versions.

🔑 **A token is good for the task it was issued to, until its deadline (#747).**
- `owner` is the identity the kernel gave that task when it made it
  (`<mach/urmach_identity.h>`): never inherited, never set, and stamped in the
  trailer of every message the task sends. A token presented by any other task
  is refused with `CAP_ERR_NOT_OWNER` -- one passed on in a message, one read
  out of another task's memory, one a forked child finds in its copy of its
  parent's. Handing a capability on is a derivation (§6.4), not a copy.
- `expires` is a deadline in nanoseconds since boot on `URMACH_CAP_CLOCK`,
  written by cap_server from the lifetime asked for, and 0 for none. A token
  past it is refused with `CAP_ERR_EXPIRED`, and when it comes cap_server
  revokes the token as `cap_revoke` would, so that what it bought -- a claim,
  with its interrupts and register mappings, and a lent buffer's pages -- is
  taken back too (§6.5). A child's deadline is never past its parent's.
- There is no use count. Counting operations is the quota's job
  (`uros_resource_model.md`), not the permission's.

### Resource types and what `resource_id` means

| type | `resource_id` is | checked by |
|---|---|---|
| `RESOURCE_FILE`, `DIRECTORY` | reserved | a filesystem server |
| `RESOURCE_MOUNT_POINT` (3) | `cap_name_hash()` of the prefix, exactly as it is checked in | the name server, at `netname_check_in_mount` |
| `RESOURCE_BLK_DEVICE` (4) | the partition | block_device_server |
| `RESOURCE_PCI_DEVICE` (5) | **the device CLASS**, not the bus address | the device master |
| `RESOURCE_DMA_BUFFER` (10) | the kernel's region id | the device master |
| `RESOURCE_REVOKE_NOTICES` (11) | — a permission, not a capability: no token is issued | cap_server, at `cap_subscribe_revoke` |
| `RESOURCE_PCI_FUNCTION` (12) | one function's bus address, `bus<<8 \| slot<<3 \| func`; a manifest writes only `any`, and the task that starts a driver writes the address into the driver's manifest at run time (#982) | the device master, at `device_claim` |
| `RESOURCE_DRIVER_ADMIN` (13) | **the device CLASS**, as for type 5: a manifest can name the network cards and not the disks; the operations are stop, start and update (#964) | the HAL, at `stop_driver`, `start_driver` and `update_driver` |
| `RESOURCE_BUDGET` (9) | reserved | — |

🔑 **A mount is put on a capability and taken out on a signature (#716).** The
name server checks a mount in only when the caller shows a
`RESOURCE_MOUNT_POINT` token for that prefix, which it verifies with the trap;
a prefix that already has a mount is refused whoever holds one. Taking a mount
out asks for the signature it was checked in with, and
`MACH_PORT_NULL` signs for nobody. Before, every mount was checked in
unsigned and anyone's to check out, "/" included, and a longer prefix needed
nobody's leave: a task that registered "/etc" answered every open under it.
The capability is asked at check-in only — one revoked later does not take
the mount out; its server's signature or its death does.

🔑 **And a mount's server holds nothing of its readers (#911).** The capability
decides who may serve a prefix; until #911 it also decided, without saying so,
who would hold every reader's task port. `fs_open` carried the client's task
port so that ext_server could reclaim a dead client's handles, and FLIPC's
connect handed it to the endpoint so that it could `vm_remap()` the channels in
— and a send right to a task port is all of the task: its memory, its threads,
its port rights. Now `fs_open` carries a send right made from the client's
*lifetime port* (`urmach_lifetime_port()`, libmach), which dies with the client
and grants nothing else, and a FLIPC client sends a token made for the
connection and maps the channels itself from the **memory entries** the server
answers (`urmach_make_memory_entry()`, a range of the server's memory named by
a port, each segment with the most a mapping may have). Both rights travel as
`MACH_MSG_TYPE_MAKE_SEND`, which the kernel makes from a receive right the
sender holds, so a task port cannot be sent there even by mistake.

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

🔑 **And the claim records what the token carries** (#621). A
`RESOURCE_PCI_DEVICE` token carries some of three operations —
`CAP_OP_PCI_DMA_MAP`, `CAP_OP_PCI_MMIO_MAP`, `CAP_OP_PCI_IRQ` — and the kernel
checks each against the token once, when the device is claimed, and keeps the
ones that pass. Afterwards every operation asks for its own:

| operation | the routines |
|---|---|
| DMA | `device_dma_alloc`, `device_dma_alloc_sg`, `device_dma_identity`, the `device_dma_map_foreign` routines |
| MMIO | `device_mmio_map`, `device_region_map` |
| IRQ | `device_msi_register`, and `device_intr_register` on a line the device raises |

`device_dma_free` needs the claim alone, and a line no claimed device raises —
the serial ports', say — needs no claim at all. A second claim by the holder,
with another token, only **narrows**: the claim keeps the operations both
tokens carry and the token that established it. A driver can give MMIO up once
its registers are mapped; it cannot get anything back, and revoking the first
token still ends the claim.

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

The id is a number, `any`, or `name:<string>` — `cap_name_hash()` of the string
(#716), for a resource named by one: `required 3 name:/proc 0x1` is the mount
point `/proc`. mkmanifest and the servers that verify compute the hash with
the one function, from `<mach/cap_name_hash.h>`.

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

Two ways, and only two (#743).

**Provisioned, by bootstrap, for the tasks it starts.**

```
cap_server makes its provisioning port
   └─ bootstrap_cap_provisioner(its bootstrap port, that port) ──▶ bootstrap
       └─ taken from the cap_server bootstrap started, once
bootstrap creates the child task
   └─ looks up "<symtab>.cmf" in the boot bundle
       └─ cap_provision_task(provisioning port, task_port, blob) ──▶ cap_server
           └─ validates, installs in the per-task table keyed by a NEW port
           ◀── makes the caller a send right to that port
   └─ task_set_special_port(child, TASK_CAP_PORT, that port)
   └─ gives its own send right back
```

The manifest comes from the **bundle**, from a source the task cannot alter,
and is handed to cap_server directly — the child never sees it pass.
`cap_provision_task` answers on the provisioning port and on no other, and
bootstrap holds that port's one send right: on the port every task inherits,
or on a per-task port, the answer is `CAP_ERR_NOT_PROVISIONER`. Until #743 any
task could provision itself with a manifest of its own writing and acquire
whatever it said. bootstrap takes the provisioning port from the cap_server it
started and from no other task, which it refuses with `KERN_NO_ACCESS`; and it
takes it once, since the loader reads it without a lock once it has seen it,
so a second offer from cap_server is refused with `KERN_NAME_EXISTS`. Two
answers, so that a test made after cap_server has given its port, when both
rules refuse it, can tell which one did.

**Derived, by exec_server, for a program a task execs.**

```
the caller makes the program's task: task_create(mach_task_self(), …)
the caller ── exec_load(…, that task, its per-task port, path, …) ──▶ exec_server
   └─ reads the program's .uros_manifest (or an empty default)
       └─ cap_derive_task(the caller's port, the task, blob) ──▶ cap_server
           └─ the program's lists, cut by the caller's delegatable[]
           ◀── makes exec_server a send right to the new port
   └─ task_set_special_port(program, TASK_CAP_PORT, that port)
   └─ gives both ports and the task back (#926)
```

The program's file says what it **asks** for; what it **gets** is its caller's
to give. An entry survives for the types, the instances and the operations the
caller's `delegatable[]` names, and its own `delegatable[]` is cut the same
way, so a chain of execs never widens. A caller with no per-task port has
nothing to hand on, and its program runs with none; so does one whose cut
leaves nothing, which `cap_derive_task` refuses with
`CAP_ERR_NOTHING_TO_DERIVE` rather than make a port that grants nothing and
holds a slot (#937). Until #743 exec_server
provisioned the file's manifest as it stood: whoever wrote a binary that
something exec'd wrote its policy.

**Not inherited, by a child made with `fork()`.** A per-task port is its task's
identity to cap_server, and a child holding its parent's would be its parent.
The kernel does not hand it on: `task_create()` leaves the child's
`TASK_CAP_PORT` null. libmach forgets the name the child's memory kept for its
parent's port before the child's first RPC (`mach_task_self_init()`), and
`fork()` copies the registered ports into the child, cap_server's well-known
port among them. So a forked child asks that port, which issues no capability,
and its exec hands exec_server no port (#945). Before #945 the child kept its
parent's name, which named something else in its space: hello_server's child
handed it to exec_server on exec and was answered `MIG_BAD_ID`.

🔑 A per-task port has no senders once its task and everyone it was handed to
have let it go: cap_server keeps none, and the provisioner gives its own back.
The no-senders notification frees the table slot and the task port kept for
#432. Only the kernel can send that notification — it comes through the
send-once right cap_server made for the request — so a holder that forges one
through a send right ends nothing.

⚠️ A task with **no** manifest has no per-task port and is refused every
capability it asks for (`cap_manifest_allows()`, #511) — and every permission:
`cap_subscribe_revoke` asks the manifest too (#744). A server that needs either
needs a manifest.

⚠️ Every `.cmf` must be shipped in the bundle **and** the build rule must depend
on the compiled **file**, not on the target that produces it. In Ninja a target
dependency is an ordering dependency: the bundle will not notice the file
changed. A policy that is compiled and not shipped is worse than none, because
it looks like one.

⚠️ The same holds for a manifest embedded in a binary as `.uros_manifest`. A
`POST_BUILD` step runs only when the binary is linked, and CMake ignores the
`DEPENDS` of a build event, so the link itself must depend on the manifest
(`LINK_DEPENDS`). Without it, hello_exec went on carrying the manifest it was
first linked with after its file had changed (#743). Read the section back with
`objcopy --dump-section`: `-O binary` copies only what is loaded, and the
section is not.

---

## 5. The interfaces

### RPCs — `cap_server.defs`, subsystem 3500

| routine | who calls it | what it does |
|---|---|---|
| `cap_acquire(type, id, ops, lifetime) → token` | any task | policy check, then mint: issued to the requester, whose identity is the request's trailer, with a deadline `lifetime` ms on (#747) |
| `cap_derive(parent, reduced_ops, lifetime) → token` | the parent's owner | a weaker child for the owner itself, never outliving its parent |
| `cap_derive_for(parent, reduced_ops, lifetime, delegate) → token` | the parent's owner | a weaker child for the task that receives on the port `delegate` (#747); `CAP_ERR_NO_DELEGATE` for a port no task receives on |
| `cap_revoke(cap_id)` | a holder / an authority | revoke it and its whole subtree; answer the kernel's first refusal, if it refused any (#745) |
| `cap_verify(token, presenter, type, op, id)` | anyone | slow path; the trap is the fast one, and both ask the type as well as the id (#745) and who presents it (#747) |
| `cap_subscribe_revoke(port)` | a task whose manifest grants `RESOURCE_REVOKE_NOTICES`, on its per-task port (#744) | receive `cap_revoke_notify`, sent without blocking |
| `cap_provision_task(task_port, manifest) → cap_port` | bootstrap, on the provisioning port, which no other task holds (#743) | install a policy for the task its identity port names, return the principal port; refuse a port that carries more than identity (#938) |
| `cap_derive_task(task_port, manifest) → cap_port` | a parent, on its per-task port: exec_server with its caller's (#743) | install the child's manifest cut by the parent's `delegatable[]`, for the child its identity port names (#938); refuse a cut that leaves nothing, and one past the bounds on derived ports (#937) |
| `cap_slots() → in_use, size, derived, quota, family, family_max` | anyone | how many manifest slots are taken, how many by derived ports, and their bounds: how a test sees a slot come back (#743) and a bound met (#937) |
| `cap_census() → entries, issued, watches, untold` | anyone | how many capabilities the issuance table holds, how many ids it has handed out, how many deaths it watches, and how many revocations at a death it told no subscriber of: how a test sees a revoked capability leave the table (#974), a watch go with the last capability and slot that held it, and a dead owner's capabilities go unannounced (#983) |

### Traps — `syscall_sw.h`

Plain syscalls, **not** Mach messages. That is not an optimisation: cap_server
calls two of them *inside its own message loop with a client waiting*, and a
server that had to send a message to answer a message can deadlock against
itself.

| trap | slot | who | question |
|---|---|---|---|
| `urmach_cap_verify(token, presenter, type, op, id)` | 37 | any resource server | is this token good for this, presented by this task? |
| `urmach_cap_revoke(token)` | 39 | cap_server only | mark its id dead in the kernel, for as long as its deadline does not refuse it first; the token is cap_server's own copy, whose MAC the kernel checks (#974) |
| `urmach_cap_register(setup)` | 40 | the task bootstrap named, once | hand the kernel the HMAC key |
| `urmach_dma_region_owner(id, task)` | 43 | cap_server | does that task own this DMA buffer? |
| `urmach_cap_epoch()` | 45 | a server that keeps verified handles | has anything been revoked since I last looked? |
| `urmach_cap_census(&entries, &dated)` | 47 | anyone | how many revocations the kernel holds, and how many of them it will forget when their deadlines come (#974) |

Slot 38 was `urmach_cap_use`, the verification that also counted a use against
a token's `max_uses`. Nothing called it and every token was minted with no
count, and counting operations is the quota's job, not the permission's
(`uros_resource_model.md`): #747 took the count out of the token and the trap
out of the table.

And on the host port, `urmach_port_receiver(host, port) → identity`
(`mach_host.defs`, #747): the identity of the task that receives on a port, which
cap_server asks for `cap_derive_for`. It is read from the receiving space's copy
of its task's identity, and only while the port is in a space: a port in
transit, a dead one and the kernel's own are answered `KERN_INVALID_ARGUMENT`.

And one routine on the privileged host port, `urmach_cap_server_set(host_priv,
task)` (`mach_host.defs`), by which bootstrap names cap_server's task: the
kernel takes the key from that task alone, and naming another forgets the key
(#744).

🔑 **The key is the processor's to draw, and it says where it came from.**
cap_server takes it from RDSEED, then RDRAND (Intel SDM Vol. 1, 253665-093US,
§7.3.17), and
only when neither answers from the TSC, which it prints as guessable (#744).

---

## 6. Control flow

### 6.1 Issuing

```
client ── cap_acquire(type, id, ops, lifetime) ──▶ cap_server
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
                    owner = the request's sender (#747)
                    expires = now + lifetime on URMACH_CAP_CLOCK
                            │
                            ▼
                        token to the client
```

🔑 **The owner is the request's sender, as the kernel stamped it (#747).**
cap_server receives with `URMACH_RCV_SENDER`, and the trailer of every request
carries the sending task's identity, which no task can choose. A request that
came without one is refused: there would be nobody to issue the token to.

🔑 **Why the DMA-buffer branch exists.** A manifest can name a device class or a
mount point because those exist before the policy is written. It cannot name a
buffer that will exist for four milliseconds. So the manifest says whether the
task may hold buffer capabilities *at all* — declared with `any`, the only
honest thing it can say — and the kernel says whether *this* buffer is that
task's to give away, which it knows because it made the allocation.

⚠️ The bridge between the two halves is the task's **identity port** that
`cap_provision_task` and `cap_derive_task` carry (#938). cap_server knows a
requester as a port; the kernel knows an owner as a task; the port is what
turns one into the other, and the identity port, the reduced port with no
right (§7), is all of the task the question needs: `urmach_dma_region_owner()`
takes it. Until #938 the RPCs carried the full task port, and cap_server kept
the memory and port rights of every task with a manifest for this comparison
alone; it now refuses a port that carries more, with
`CAP_ERR_TASK_NOT_IDENTITY`, and libcap makes the identity port from the task
port its caller holds. For a derivation it is the parent's word: cap_server
does not check that the task is the parent's child. It does not need to,
because only the holder of a task's full port can make the task's identity
port, and that right is control of the task already.

### 6.2 Checking

```
holder ── presents token ──▶ resource server
                               │
             urmach_cap_verify(token, presenter, type, op, resource_id)   [trap]
                               │
                        ┌──────┴──────┐
                    HMAC good?   past its deadline?   presented by its owner?
                    type and resource_id match?
                    ops cover the request?   token->revoked clear?
                               │
                     kernel revocation table: was cap_id revoked?
                               │
                          KERN_SUCCESS
```

Seven comparisons and a hash: the deadline, who presents it, the type and the
id, the operations and the revoked flag, then the kernel's revocation table,
the only table touched, because a revocation is the one fact a signed token
cannot carry.

🔴 **Who presents it (#747).** A resource server cannot know from the token who
handed it over, and the trap cannot know either -- its caller is the server.
So the server reads the presenter from the trailer of the request that carried
the token (`urmach_msg_sender()`, receiving with `URMACH_RCV_SENDER`) and passes
it: the block server at an open, keeping it with the handle for every check
after; char_server, gpu_server, and the name server for a mount. Inside the
kernel -- a device claim, the foreign DMA maps -- the presenter is the calling
task. A task asking about its own token passes its own identity, `cap_self()`.

⚠️ **The deadline is never taken from the cache.** #537's per-processor cache
skips what cannot change while a token's bytes do not; the deadline's answer
changes while the bytes stay the same, so it is asked on every check, outside
`cap_lock`. A token with a deadline is refused when the clock cannot be read,
rather than made endless. An open handle and a lent buffer's kept translation,
which the block server asks again only when the revocation epoch moves, meet
the deadline by its revocation (§6.5), which moves the epoch.

⚠️ **The type as well as the id.** Ids are numbered per type, so a block
device and a DMA buffer can share one: a check of the id alone takes a token
for one of them as a token for the other. The trap has asked the type since
#599; the RPC took no type until #745, so `cap_verify` said yes to a token of
any kind whose id matched.

⚠️ `token->revoked` is in the **holder's copy** and proves nothing against a
malicious holder. The kernel's table is the real defence.

### 6.3 Revoking

```
somebody ── cap_revoke(cap_id) ──▶ cap_server
                                     │
          walk the delegation tree by its own links, for each:
                                     │
                     ┌───────────────┼────────────────┐
              mark own copy    urmach_cap_revoke  cap_revoke_notify
                               (trap, kernel)     (IPC, subscribers)
                                     │
                        device_master_cap_revoked(cap_id)
                                     │
            drop the claim; give back its lines and message-signalled
            slots; take its register mappings out of every task;
            leave the device BLOCKED
```

🔑 **The walk has no bound but the tree (#745).** The tree is linked first
child, next sibling, parent, and the walk follows those links down, across
and back up, never past the root, so it needs no stack. It ran on a stack of
seventeen entries, `CAP_MAX_DEPTH + 1`, and a capability with more than
seventeen children waiting lost the rest of them with their subtrees: the
revocation answered success and their holders kept their authority.
`CAP_MAX_DEPTH` bounds how deep a tree goes, not how wide.

🔴 **The kernel's answer is heard (#745).** `urmach_cap_revoke` refuses any
caller but the cap_server that gave it the key, and a revocation its table
has no room for. cap_server threw the answer away, so a refusal was counted
and answered as done while the trap went on verifying the token. It says each
refusal now, goes on with the rest of the tree, and answers the request with
the first.

🔑 **What the two tables keep, and for how long (#974).** Neither freed an
entry: cap_server's held one for every capability it ever issued, and the
kernel's one for every id ever revoked, and since #747 a deadline revokes
every capability asked for with a lifetime, so a task asking for short-lived
ones grew both without bound. An entry is now freed once it can no longer
decide an answer.

- **cap_server forgets a revoked tree** once its notices are sent, after
  `cap_revoke` and after a deadline's sweep. Its parent may live, and the
  parent's list of children runs through it, and a walk ends at an id it
  cannot find: so the link that names it -- the parent's first child, or the
  previous sibling's next -- is pointed past it first, and that token signed
  again. `cap_verify` answers `CAP_ERR_REVOKED` for an id the table no longer
  holds: a token whose MAC verifies was issued by this server, whose key is
  new each time it starts, and only a revoked entry leaves. A revocation of
  such an id is answered `CAP_ERR_REVOKED`, one of an id never issued
  `CAP_ERR_INVALID_TOKEN`.
- **The kernel reads the deadline from the token.** `urmach_cap_revoke` is
  handed cap_server's own copy and checks its MAC, so the deadline it acts on
  is the issuer's and not cap_server's word. The deadline is checked before
  the revocation table, on every call and never from the cache (§6.2), so a
  token past it is refused before the table is asked: an id revoked when its
  deadline has come -- every revocation a deadline makes -- is given no
  entry; one revoked before it is kept, in a list by deadline, until the first
  revocation after the deadline frees it; one whose token has no deadline is
  kept for good, since nothing else would ever refuse it. The epoch moves and
  the mappings are torn down for every revocation all the same.
- ⚠️ **Only on a clock that is never set.** x86-64's capability clock is its
  monotonic clock, which has no `c_settime`. i386's is `REALTIME_CLOCK`, which
  `rtc_settime()` can set back, and with it a deadline that has come; so i386
  keeps every revocation (`URMACH_CAP_CLOCK_NEVER_SET`, `<mach/clock_types.h>`).

`cap_census` and trap 47 are how `cap_test` [55] to [57] see the entries go.
The capabilities of a task that has died end with it: §6.6.

🔴 **The trap is the half that cannot wait for a message.** Some authority is
*materialised*: a capability for a PCI device becomes an IOMMU domain, which is
a page table the device walks without consulting anything. Revoking the token
changes nothing there — the withdrawal has to reach the *mapping*, and the
kernel is where that can happen immediately.

🔴 **Blocked, not pass-through.** Restoring the entry the device started in
would make the revocation *give* it all of memory: a widening dressed as a
withdrawal, and one somebody trusted.

🔴 **Everything the claim made, not only the domain (#745).** A claim
materialises three things: the domain, the interrupts registered under it and
the mappings of the device's registers. A revocation used to end the claim and
the domain, and the holder kept hearing the device and kept programming it
through registers still mapped: only its next call was refused. So the lines
and message-signalled slots resting on the device are given back as an
unregister gives them back -- a message-signalled slot is disarmed in the
device's own table -- and every mapping of the device's memory is taken out
of the task it was made in, by its frames (`vm_map_remove_frames()`, #599),
before the domain goes. The kernel writes down each mapping it makes, and one
it cannot write down it does not make: a mapping a revocation cannot find is
one it cannot take back. A driver that dies gives its interrupts back the same
way; it used to have them masked only, with the message-signalled slots left
armed in the device and the lines held.

🔑 **And the notice is not what stops a mediated use (#744).** A server that
keeps a handle opened on a token — the block server does — reads
`urmach_cap_epoch()` on each use and asks the token again whenever the epoch
has moved, so a revocation reaches the handle whether or not a notice does.
cap_server sends `cap_revoke_notify` without blocking (`MACH_SEND_TIMEOUT` 0)
and says when one was not delivered: a subscriber that does not read cannot
hold the revocation up, and a notice is an early word, not the guard.

### 6.4 Delegating

```
owner ── cap_derive_for(parent, ops, lifetime, port) ──▶ cap_server
                                                            │
                                  is the asker the parent's owner?
                                                            │
                          urmach_port_receiver(host, port)  [kernel]
                                  who receives on it?
                                                            │
                     child: ops ⊆ parent's, deadline ≤ parent's,
                     owner = that task, linked under the parent
```

🔑 **A capability is handed on by deriving it for the task that will present
it (#747).** The owner names that task by a port it receives on -- the port it
talks to it on -- and the kernel, not the owner, says whose port it is. The
child is the delegate's alone; revoking the parent revokes it, and everything
it bought. A client's DMA buffer is the case that exists today: the block
server presents the buffer's capability to the kernel for every page it maps
for its disk, so the client (ext_server's page cache, cap_test's arms) derives a
child for it, named by the handle, and hands over the child.

### 6.5 A deadline is a revocation at that time

```
cap_acquire / cap_derive* ── a deadline written ──▶ cap_expiry_due, lowered
                                                         │ ring
                                  the waiter (a thread of cap_server's)
                                  waits until cap_expiry_due comes
                                                         │ ring
                     the receive loop: every token past its deadline,
                     with its tree, revoked as cap_revoke revokes it;
                     cap_expiry_due set to the earliest left; and then
                     every revoked tree it met forgotten (#974)
```

🔴 **A deadline took back nothing a token had bought (#747).** Past it the
token was refused wherever it was presented, by the trap and by cap_server,
and a claim made with it before then went on: the card stayed claimed, its
interrupts registered and its registers mapped, and a buffer lent with it
stayed in the device's IOMMU domain. A revocation takes all of that back
(§6.3), so cap_server, which writes every deadline, revokes each capability
when its deadline comes: the kernel told of it and of its tree, and the
subscribers given their notices.

🔑 **The table is the receive loop's.** The waiting is a thread of its own,
which shares one value with the loop, the next deadline, under a mutex: the
loop lowers it when it writes an earlier one, and after a sweep sets it to the
earliest left, and rings the waiter either way so that it looks again. The
waiter rings a port in the loop's set when the deadline comes, and again every
100 ms until a sweep has moved it, so a ring lost to a full queue or to no
memory is rung again. A tree's deadlines are never past its root's, so a sweep
that revokes a tree finds all of it due.

⚠️ **The deadline holds at its instant; the revocation follows it.** The
trap's checks and cap_server's refuse the token from the deadline on, by the
clock. The revocation comes a wake-up and a message later -- 0 to 14 ms after
it in eleven boots under KVM on 19/11, at one processor and at four: an open
block handle reads up to it (`cap_test [54]`), and a claim keeps its page that
long (`irq_claim_test [21]`). Without the waiter a
lifetime is refused, since nothing would take back what the capability
bought.

### 6.6 A capability ends with its lender (#983)

A capability asked for with no lifetime by a task that then died was never
revoked: nobody could present it, a token being good for its owner alone, and
nobody could revoke it, since only the task whose per-task port it was asked on
may, or one above it in the tree, and that port was gone. Every program that
asked for a capability and ended grew cap_server's table for as long as it ran.

🔑 **A loan ends with its lender** (decided in #983). When a task dies,
everything asked for on its per-task port is revoked with its tree -- the
children it lent to other tasks included -- and forgotten as a revoked tree is
(§6.3); and so is everything it owns, wherever it was asked and whoever lent it.
It is Mach's rule for ports: when the task that receives dies, the send rights
others hold to it go dead. Today's one lender is ext_server, lending the block
server its page-cache buffers; once it has died its DMA regions are reclaimed,
and the loan named a region that was no longer there.

```
dead-name notification ──▶ cap_server's death port
                                  │
              an EXACT watch (a task's identity port): the task has died
                 ├─ its slots given back, their per-task ports destroyed
                 └─ every capability holding the watch, as lender or owner:
                       revoked with its tree; the kernel told only of the
                       ones another task owns; all of it forgotten
              a PORT's watch (the port a borrower with no slot was named by):
                 └─ the loans made through that port, and nothing else
```

- **A watch is a dead-name request on a port that dies with what it watches.**
  An exact one is on the identity port a slot keeps (#938), which dies with its
  task and with nothing else; a borrower with a slot is watched by its slot's.
  A port's watch is on the port `cap_derive_for` named a borrower with no slot
  by, which cap_server keeps from then on: its death may be the borrower's
  giving the receive right up, and not its death, so it ends the loans made
  through it alone.
- **Every capability holds two watches**, its lender's -- the task whose
  per-task port it was asked on -- and its owner's; a death ends what holds its
  watch either way. Ids name the watches, never given again; a watch goes with
  the last capability and slot that held it, its request taken back first. A
  slot that cannot watch its task is refused.
- **A per-task port given up is not a death.** Its no-senders notification gives
  the slot back, and the capabilities asked on it stay, for their owner may live
  and present them, until the watch says the task has died.
- 🔴 **What the dead task owned is forgotten, not revoked in the kernel, and
  no subscriber is told of it.** The kernel keeps a revocation with no deadline
  for good (§6.3), and for a token nobody can present again it would keep it
  for nothing; what the token bought the kernel took back when the task
  terminated, and what a subscriber kept for it ends with the dead task's ports
  -- the block server's handle with its no-senders -- as it did before #983,
  when a death revoked nothing. So an exact watch's walk tells the kernel and
  the subscribers only of capabilities other tasks own, and knows whose death
  it is by `urmach_task_identity()`, a kernel call that answers the identity of
  the task any task port names, reduced ports included. A port's watch knows
  no death, and tells of all it revokes. `cap_census` counts the revocations
  told to no subscriber.
- ⚠️ **A notice is a console line when it misses**, and a full subscriber's
  queue makes most of a burst miss: under char_server, at one processor and
  TCG, a burst of them overran the kernel's log ring (#762), and the lines other
  tasks were waiting on went with it.

⚠️ What it does not do. A capability asked for on the well-known port -- a tag,
on i386 alone since #511 -- has no slot to watch and stays. The kernel keeps,
for nothing, the revocation of a loan whose borrower with no slot has died, and
of a capability owned by a third task whose death cap_server has not heard yet:
both are #1007's, a revocation kept after its owner. And an entry still names
the per-task port it was asked on by its name, which a later slot can be given:
#1008.

`cap_test` [58] to [61] see it: tasks made to die, with capabilities asked on
their ports and lent to them; one that gives its port up and lives; a loan
through a port destroyed while its task lives; and watches that go while their
tasks live, once nothing holds them.

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

### Rights on a task (#927)

A send right to a task port is the whole task: its memory, its threads, its
port rights and its special ports. A server that needed a little of that used
to be handed all of it. The kernel now names rights on a task
(`<mach/task_rights.h>`), and a routine that takes a task names the one it
needs:

| right | routines |
|---|---|
| `URMACH_TASK_RIGHT_INFO` | `task_info()` |
| `URMACH_TASK_RIGHT_LIFECYCLE` | `task_terminate()`, `task_suspend()`, `task_resume()` |
| none, `URMACH_TASK_RIGHTS_IDENTITY`: any port on the task | `urmach_dma_region_owner()` (#938) |
| the full port only | everything else: memory, threads, port rights, special ports |

The traps of `task_terminate()`, `task_suspend()` and `task_info()`, which
libmach tries before MIG, take a reduced port the same way: one lookup of the
name, the one a task port gets, and the same check
(`ref_task_right_port_locked()`). On omen under KVM the difference is within
the spread of five boots each: `task_info()` took 0.61 µs by a reduced port
against 0.59 by the task port, and a suspend and resume 1.44 against 1.45
(x86-64; on i386 0.86 against 0.84, and 1.87 against 1.87). `cap_test [45]`
calls the traps themselves, because when a trap refuses, libmach goes through
MIG, which gives the same answer, only slower.

**A reduced port carries a set of these rights.**

- `urmach_task_rights_port(task, rights)` makes it, from the full task port
  and from nothing else, so a right is taken away and never added.
- There is one per set and per task, so two registrations of one process
  name it alike.
- It dies with the task, so a dead-name request on it reports the death, as
  one on the task port does.
- The set with no bits is the task's identity.

`urmach_task_port_rights(host, port)` says what a port carries. It is asked of
the host, so that a plain port gets an answer, where asking the port itself
would send a request nobody reads.

proc_server is handed each process's reduced port with the info and lifecycle
rights, and refuses a full one with `PROC_ERR_FULL_TASK`. cap_server is
handed each task's identity port, the set with no bits
(`URMACH_TASK_RIGHTS_IDENTITY`), which it keeps with the task's slot to ask
`urmach_dma_region_owner()` which task a DMA region belongs to, and refuses
any other with `CAP_ERR_TASK_NOT_IDENTITY` (#938). A right is named when a
routine comes to check it: reading a task's memory, for a debugger, is not
named yet.

Three servers held task ports they did not need, as found while enumerating
for #911. exec_server held the port of every task that spawned a program, and
of every program and first thread it started, until #926: the caller now
makes the task and lends it, and exec_server gives it back. proc_server held
every process's task port until #927, and cap_server every task's with a
manifest until #938.

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
- **The manifest table holds 64 per-task ports at once, and its bounds are
  fixed numbers.** Slots come back when a per-task port has no senders
  (#743), so the limit is on the ports alive, not on how many tasks have ever
  run. Derived ports are bounded so that no task and no family of tasks can
  fill the table (#937):
  - a derivation that keeps nothing takes no slot (`CAP_ERR_NOTHING_TO_DERIVE`);
  - derived ports hold at most 32 slots (`CAP_ERR_DERIVED_QUOTA`), so the other
    32 are bootstrap's for the tasks it starts: 12 on x86-64, 18 on i386;
  - one family -- a task bootstrap provisioned and every task derived from it,
    down any chain of execs -- holds at most 16 (`CAP_ERR_LINEAGE_FULL`). A
    family is a number given at provisioning and never given again.

  Before #937 a task with a per-task port could derive and hold until the
  table was full, and bootstrap then started its tasks without their
  manifests: in #743's ablations that left the slots taken, ext_server,
  mount_test and proc_server ran without theirs in every boot. What is not
  built: a table that grows, and a quota sized by what bootstrap starts rather
  than half the table. Only bootstrap, with more than 32 tasks of its own,
  could fill it now.
- **Only `cap_test`'s manifest delegates anything.** It hands on block devices
  READ, to show a derivation cut; every other task's programs get nothing,
  whatever their files ask for.
- **A forked child holds no per-task port, and nothing gives it one (#945).**
  A port derived for it at `fork()`, cut by its parent's `delegatable[]` as an
  exec's is, would let a forking server's children hold capabilities. It waits
  for a forking task that needs one: today no task that forks delegates
  anything, so the derivation would be refused for every child.
- **Most resource types have no server checking them.** Only
  `RESOURCE_BLK_DEVICE`, `RESOURCE_PCI_DEVICE`, `RESOURCE_PCI_FUNCTION`,
  `RESOURCE_DMA_BUFFER`, `RESOURCE_MOUNT_POINT` and `RESOURCE_DRIVER_ADMIN`
  are verified anywhere today, and
  `RESOURCE_REVOKE_NOTICES` is a permission cap_server asks for. `FILE`,
  `DIRECTORY`, `PRINTER`, `SERIAL`, `FRAMEBUFFER`, `BUDGET` are reserved values
  with no enforcement behind them — the browser example above is therefore
  *architecture*, not a transcript.
- **A manifest decides every capability.** A task that ships none is refused
  all of them (`cap_manifest_allows()`, #511). In the x86-64 build fifteen
  programs ship one: eleven in the bundle, for the tasks bootstrap starts
  (block_device_server, hal_server, ext_server, proc_server, `cap_test`,
  mount_test, dma_reclaim_test, irq_claim_test, `fork_cap_test`, ipc_bench
  when it is bundled, and bootstrap's own stage 2), and four in the binary
  itself as `.uros_manifest` (`hello_exec`, `ush`, `hello_dyn_world`,
  `mach_error_dyn`).
  For those four and every other program exec'd rather than started by
  bootstrap, the file says only what they ask for: what they get is cut by
  their caller's `delegatable[]` (#743).
- **Budgets do not exist.** `RESOURCE_BUDGET` is a reserved number.
- **The kernel writes down at most 64 mappings of device memory at once**
  (`MMIO_MAPPINGS_MAX`), and refuses the next one, since a revocation takes
  back only the mappings it can find (#745). A table that grows waits for a
  machine with more devices than that to map.

## 10. Where the checks can be seen to fail

Every guard described here has an arm that fails when the guard is removed;
this is where to look, and what to break to confirm it still works.

| check | arm | ablate by |
|---|---|---|
| a device has one driver | `cap_test [11]` | removing `check_claim` from `device_dma_alloc` |
| a client's buffer needs a capability | `cap_test [12]` | not calling `device_register_dma` |
| the manifest refuses what it does not declare | `cap_test [13]` | shipping no `.cmf` |
| revocation tears the mapping down | `blk: gave 0:31.2 back …` | removing `urmach_cap_revoke` from cap_server's `cap_revoke` |
| a claim's operations are its own | `irq_claim_test [14]`, `[15]`, `[16]` | `ablations/621-dma-unchecked`, `621-irq-unchecked`, `621-mmio-unchecked` |
| a second claim only narrows | `irq_claim_test [14]`, `[17]` | `ablations/621-claim-not-narrowed`, `621-claim-widened` |
| the kernel takes the key from bootstrap's cap_server only | `netname_test [9]`; `cap: urmach_cap_register (setup) failed` | `ablations/744-key-from-the-first-task`, `744-cap-server-never-named` |
| "cap_server" is held before cap_server starts | `netname_test [10]`, `cap_test [33]` | `ablations/744-cap-server-name-unpublished`, `744-cap-server-taken-over` |
| a slot's port is its own server's | `cap_test [34]` | `ablations/744-service-checkin-to-anyone` |
| subscribing to revocations is a permission | `cap_test [35]` | `ablations/744-subscribe-from-anyone` |
| a revoked handle stops with no notice | `cap_test [5]` | `ablations/744-handle-not-reverified`, over `744-widen-no-revoke-notices` |
| a mount is put on a capability for its prefix | `mount_test [4]`, `[6]` | `ablations/716-mount-without-capability` |
| a mount is checked in and out on a signature | `mount_test [2]`, `[7]`, `[11]` | `ablations/716-null-mount-signature-accepted` |
| `fs_open` hands a server no task port | `mount_test [27]` | `ablations/911-fs-open-hands-the-task` |
| FLIPC's connect hands an endpoint no task port | `mount_test [28]` | `ablations/911-connect-hands-the-task` |
| a connection ends with its token, refused ones keep nothing | `mount_test [29]`, `[30]` | `ablations/911-disconnect-keeps-the-token`, `911-one-token-for-every-connection`, `911-refused-connect-keeps-its-right` |
| a failed open leaves the client's right alone | `mount_test [31]` | `ablations/911-failed-open-released-twice` |
| a mount's FLIPC endpoint has no name, and its port is had through the mount's | `mount_test [34]` | `ablations/715-endpoint-named` |
| a FLIPC request is held to what the server keeps: the write's source and the read's cap to its own copy of each region, the handle at its full width, the offset to the Mach path's limit | `mount_test [35]` to `[38]` | `ablations/715-write-source-unbounded`, `715-safe-ptr-trusts-header`, `715-read-cap-from-header`, `715-handle-cut-to-int`, `715-flipc-asks-no-offset-limit`, `715-offset-limit-taken-out` |
| an entry's segments hold, and never exceed its maker's mapping | `cow_test [10]`, `[11]` | `ablations/911-segments-not-enforced`, `911-entry-allows-more-than-its-maker` |
| an entry ends with its last send right, and a fork keeps it shared | `cow_test [12]`, `[14]` | `ablations/911-entry-outlives-its-port`, `911-symmetric-copy-kept` |
| a task cannot provision itself | `cap_test [38]` | `ablations/743-anyone-provisions` |
| a derived child gets what its parent may hand on, and no more | `cap_test [39]`, `[42]` | `ablations/743-child-gets-what-it-asks` |
| a per-task port given back gives its slot back | `cap_test [40]` | `ablations/743-no-senders-frees-nothing`, `743-cap-server-keeps-a-copy` |
| only cap_server gives bootstrap the provisioning port | `cap_test [41]` | `ablations/743-provisioner-from-anyone` |
| exec_server keeps neither the program's port nor its caller's | `cap_test [42]` | `ablations/743-exec-server-keeps-the-programs-port`, `743-exec-server-keeps-the-callers-port` |
| bootstrap keeps no task's port | `bootstrap: … still holds a send right to its TASK_CAP_PORT …` | `ablations/743-bootstrap-keeps-its-copy` |
| a no-senders notification forged by a holder ends nothing | `cap_test [43]` | `ablations/743-forged-no-senders-taken` |
| exec_server holds nothing of a spawn once it has answered | `cap_test [44]` | `ablations/926-exec-server-keeps-the-task`, `926-first-thread-copied` |
| proc_server holds a process's reduced port, and it reaches no more | `cap_test [45]` | `ablations/927-proc-server-takes-full-ports`, `927-reduced-port-reaches-memory`, `927-rights-not-checked`, `927-mint-from-a-reduced-port` |
| a reduced port takes the traps itself, not through MIG | `cap_test [45]` | `ablations/927-traps-refuse-a-reduced-port` |
| a derivation that keeps nothing takes no slot, and derived ports meet a family's bound and a quota | `cap_test [46]` | `ablations/937-empty-derivation-takes-a-slot`, `937-family-unbounded`, `937-quota-unbounded` (over the widen `937-widen-family-to-the-quota`) |
| cap_server keeps a task's identity port and no task port, and the kernel's DMA owner check takes the identity port | `cap_test [47]`, `[12]` | `ablations/938-cap-server-takes-full-ports`, `938-cap-server-keeps-task-ports`, `938-dma-owner-wants-the-task-port` |
| a forked child holds no per-task port, and cap_server answers its requests | `fork_cap_test [2]`, `[3]` | `ablations/945-child-keeps-the-parents-name`, `945-cap-server-port-not-copied` |
| a revocation reaches every child, however many | `cap_test [48]` | `ablations/745-cascade-on-a-stack` |
| a revocation the kernel refuses is said, and answered to the requester | `cap: the kernel refused the revocation of cap …`; `cap_test [48]` | `ablations/745-probe-kernel-refuses`, a probe |
| `cap_verify` asks the type as well as the id | `cap_test [49]` | `ablations/745-verify-without-type` |
| a revoked claim takes back its interrupts and its register mappings | `irq_claim_test [18]`, `[19]`, `[20]` | `ablations/745-irq-kept-on-revoke`, `745-mmio-kept-on-revoke` |
| a lifetime asked for is kept, and a child's never passes its parent's | `cap_test [50]` | `ablations/747-deadline-unchecked`, `747-deadline-unchecked-rpc`, `747-child-outlives-parent` |
| every task has an identity of its own, and what it sends carries it | `cap_test [51]` | `ablations/747-identity-inherited`, `747-trailer-says-kernel`, `747-fast-trailer-says-kernel` |
| a capability is issued to the task that asked for it | `cap_test [52]` | `ablations/747-owner-not-written` |
| a capability is good for its owner alone, and is derived to a named task | `cap_test [53]` | `ablations/747-owner-unchecked`, `747-owner-unchecked-rpc`, `747-kernel-port-is-a-delegate`, `747-delegate-ignored` |
| a forked child holding its parent's capability is refused it, and cannot derive from it | `fork_cap_test [4]`, `[5]` | `ablations/747-blk-trusts-the-token`, `747-derive-by-anyone` |
| a capability is revoked when its deadline comes: a claim made with it gives back its page and the card, and a block handle opened with it ends | `irq_claim_test [21]`, `cap_test [54]` | `ablations/747-deadline-not-swept`, `747-deadline-not-told` |
| a task's death ends what was asked on its port and lent to it, and the kernel is told of the first alone | `cap_test [58]` | `ablations/983-death-not-acted-on`, `983-dead-owner-told-to-the-kernel`, `983-owner-watch-not-held`, `983-lender-watch-not-held`, `983-dead-tasks-slots-kept`, `983-task-identity-is-the-callers` |
| a per-task port given up is not a death | `cap_test [59]` | `ablations/983-no-senders-taken-for-a-death`, `983-lender-watch-not-held` |
| a loan to a task with no slot ends with the port it was named by, and with nothing else | `cap_test [60]` | `ablations/983-port-death-taken-for-the-task`, `983-port-watch-tells-the-kernel-nothing`, `983-owner-watch-not-held` |
| a watch goes once nothing holds it, while its task lives | `cap_test [61]` | `ablations/983-watch-never-given-up` |

⚠️ Five guards of #747 have no arm yet. The deadline on #537's cache path
(`cap_check_in_kernel()`): [50] asks the trap, which has no cache, and no arm
presents a token with a deadline to the kernel's own checks. The port in transit
in `urmach_port_receiver()`: a port is in transit only while a message carries
it, which no arm holds still. cap_server's refusal of a request without its
sender: every request arrives with one, since cap_server asks for it. The
waiter's ring again every 100 ms: a ring is lost only to a full queue or to no
memory, which no arm brings about. And a lifetime refused when the waiter did
not start: it always starts.

⚠️ `cap_test [54]` alone can pass with the sweep taken out: any other
revocation moves the epoch, and the block server then asks the handle's token
again and is told it is past its deadline -- in three ablated boots on 19/11,
2 092, 2 095 and 2 134 ms after it, where eleven fixed ones said 0 to 37 ms.
`irq_claim_test [21]` cannot: only the revocation of that token takes the
claim's page back, and it caught both of the sweep's ablations. [21] waits for
the HAL's name on its check-in (`netname_wait()`, #696); it waited two hundred
yields, and in 19 of 60 boots on 19/11 it did not run.

⚠️ One guard has no arm: bootstrap refusing cap_server a second provisioning
port. Only cap_server can offer one, on its own bootstrap port, and it offers
one; the rule is there for the loader's unlocked read, not against another
task. What `[41]` needs from it is its different answer, `KERN_NAME_EXISTS`, so
that the rule against other tasks is seen to refuse on its own.

⚠️ `[11]` asks `device_dma_owned` *before* trying, because a test written as
"try it and see" would claim any device nobody had claimed yet — a self-test
breaking the thing it checks. And the revocation arm proves itself by being
**refused a DMA buffer**, not by asking who owns the device: the block server
*was* the owner, so that question answered "no" either way and read as a pass
for one run.
