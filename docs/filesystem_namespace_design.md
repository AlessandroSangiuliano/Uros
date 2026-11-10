<!--
  Uros filesystem namespace and hierarchy — design direction.
  Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
  Licensed under the MIT License.
-->

# Uros filesystem namespace and hierarchy — design direction

**Status:** design direction / proposal (10 November 2026). Parts of
section 1 describe what exists; sections 2–6 are the agreed direction, not
yet implemented. When a piece lands, move it from "proposed" to "exists"
here, the way `capability_system.md` records what is built.

This document records a design decision taken in discussion, so that it is
not lost under the day-to-day porting work. It answers two questions the
system does not yet answer: **what the directory hierarchy is**, and
**what the namespace model is** (global vs per-process). Section 6 adds the
security use the model makes possible: code the user did not consent to
runs in an entirely virtual filesystem.

---

## 1. What exists today

There is a mount *table*, not a *tree*.

- **`name_server` holds the authoritative mount table**: absolute path
  prefix → fs-server send right, matched longest-prefix at segment
  boundaries (`mount_prefix_match`, `nprocs.c`). Since #716 each mount
  carries a **capability**, and since #744 a signer; since #911 a client
  connects to a server with a token of its own rather than by handing over
  its task port. Registered today: `"/"` (ext2_server) and `"/proc"`
  (proc_server).
- **`libvfs` is a client-side VFS** (`uros/src/lib/libvfs/libvfs.c`): no
  central VFS server, no kernel VFS. On each `open`/`stat` it calls
  `netname_look_up_mount()` on name_server to resolve the owning mount
  (~1.4 µs RPC, deliberately always fresh so a cached `"/"` cannot beat a
  later-registered `"/proc"`), then sends the operation **directly** to the
  chosen fs server. The resulting fd's `read`/`write` go straight to that
  server; name_server is not on the data path. A dead send right evicts the
  entry and the next call re-resolves.
- **A server mounts itself.** "Mounting /proc" is not a kernel `mount()`:
  proc_server calls `netname_check_in("/proc", its_port, signature)` at
  startup. The kernel is never in path resolution — it only carries the IPC.
- **`/proc` is already a translator** in the Plan 9 / Hurd sense: a point in
  the namespace that, when opened, is served by a task. We get this for free
  from the mount model.

So Uros is **already structurally a per-process-namespace system** and has
not been named as one: resolution is client-side, and a mount is visible to
a process according to the capability it holds. "Global" is only the policy
of handing every process the full set.

### The two gaps

1. **Composition.** Mounts are routed overlays, not grafted into one
   walkable tree. `readdir("/")` shows ext2's entries; a mount point does
   not appear in its parent's `readdir` unless the parent fs has a stub
   directory. A real hierarchy needs the mount point to appear in the
   parent's listing and the walk to cross it cleanly.
2. **Layout.** Nothing defines *where* things live — no decided `/bin`,
   `/etc`, `/usr`, `/dev` tree. The root ext2 image is essentially empty,
   and the userland bring-up (BusyBox → BSD) needs a home.

---

## 2. The hierarchy (FHS)

Separate directories by **what a thing is** — OS / config / state / user /
virtual — not by historical accident. Do the `/usr` merge from day one: the
`/bin` vs `/sbin` vs `/usr/bin` vs `/lib` split comes from a full disk in
the 1970s and has no reason to exist in 2026.

```
/
├── usr/          the OS. read-only, the shippable image. This IS the system.
│   ├── bin/      every executable (no sbin: privilege is a capability, not a path)
│   ├── lib/      shared objects and the dynamic loader
│   ├── servers/  the server tasks (ext2_server, proc_server, name_server, ...)
│   └── share/    arch-independent data
├── etc/          configuration (defaults from /usr/share/factory, writable overlay)
├── var/          mutable state (logs, spool, databases)
├── home/         users
├── run/          tmpfs: sockets, pids, runtime state (ephemeral)
├── tmp/          tmpfs: scratch
├── dev/          VIRTUAL → device server
├── proc/         VIRTUAL → proc_server (exists)
└── sys/          VIRTUAL → kernel/hardware-state server (when needed)
```

**Why this fits a microkernel.** The split falls exactly on the **disk vs
server** boundary. `/usr /etc /var /home` are served by an on-disk fs
(ext2). `/dev /proc /sys /run /tmp` are *servers*, not disk files — and in a
microkernel each of these subtrees **is** a mount → a task. In a monolith
this separation is a convention; here it is the real structure.

**What we drop, and why.**

- `/sbin`, `/usr/sbin` — gone. Privilege is a **capability**, not "it lives
  in sbin". A binary is a binary.
- Separate `/bin /lib /usr/local /opt` — gone. Everything under `/usr`.
- `/boot` — not part of the live system; it is GRUB / boot-device material
  and stays out of the namespace.

**BSD/BusyBox compatibility** (the one small cost of pragmatism): `/bin`,
`/sbin`, `/lib` remain as **symlinks into `/usr/...`**, so `#!/bin/sh` and
hard-coded binary paths in ported software work untouched. That is the whole
debt — a handful of symlinks.

### Open decisions in the layout

1. **Where the server tasks live.** Proposed: `/usr/servers`, part of the OS
   image, with a fallback to the boot device for the servers needed *before*
   a filesystem is mounted (block_device_server, ext2_server itself).
   Bootstrap today reads `/mach_servers/` from the boot device; that path can
   remain for the pre-fs servers and be symlinked.
2. **Factory + overlay for `/etc`.** Proposed: ship defaults in
   `/usr/share/factory` and overlay a writable `/etc` on top — systemd's
   "stateless system", which gives a clean factory reset for free.

---

## 3. The namespace model

**Decision: build the per-process-native foundation now; default to global
during bring-up.** We do *not* build a global-only system and retrofit
per-process namespaces later — that is Linux's path (mount namespaces bolted
on, with the shared/private/slave propagation edge cases and the
`/proc/PID/ns` machinery). We avoid it.

"Global" is not a different architecture; it is a **default policy**. The
machinery — client-side resolution, capability-gated mounts, a per-process
root — is the same either way. Global means *fork hands the child the same
capability set as the parent*, so everyone sees the same tree. Per-process
means *fork can hand the child a different set*. Same machine, different
distribution.

### The one decision that makes it per-process-native, taken now

**A process's root is a capability it inherits** — from its parent, and
ultimately from bootstrap — **not a well-known port anyone looks up.**

- Root inherited → per-process falls out for free: fork passes a modified
  capability and the child has a different view.
- Root as a global well-known name → ambient authority is nailed into the
  foundation and peeling the global model off costs years.

This costs very little now (we are already structurally there: client-side
libvfs + capability mounts) and avoids the retrofit entirely.

---

## 4. The per-process cache = the namespace state

When the per-process resolution cache is built (it removes the per-`open`
name_server lookup), it is **not a mirror of the global table — it is the
process's namespace state.** So, decided together with the cache:

- the namespace **lives client-side, in libvfs**, not in a per-namespace
  server;
- it is **inherited across fork** (serialized and passed down, or
  re-derived);
- its **data model is a *modifiable set*** of `(mount-point, capability)`,
  able to represent "this set, with this added / masked / replaced" — **not**
  a dumb copy of name_server's table.

The last point is free and decisive: if the cache can already represent a
modified namespace, the manipulation operations are a **pure addition**, not
a change to the foundation. Build it as a global mirror and a later `bind`
forces a format change.

---

## 5. The manipulation API — when

The *foundation* of the API (where the namespace lives, fork inheritance,
the modifiable-set data model) is decided **with the per-process cache**
(section 4), because the cache is the namespace.

The *operations* (`bind`, union, "spawn a child with a reduced root") are
decided **at the first real consumer**, and that is **not a bring-up
blocker**: up to a shell and the coreutils everyone uses the default — fork
passes the full root, the identity case, no API needed. The first thing to
force the operations will be one of:

- the **sandbox for unconsented code** (section 6) — the strongest
  candidate, with a precise threat model;
- another **sandbox / container** ("run this with a reduced root");
- a **chroot** for a build or installer;
- a Plan 9 **union `/bin` per architecture**, if we want it;
- a display server (Arcan / Odin) wanting a restricted view.

None are near; the operations land incrementally, well inside a later
v0.3.x or beyond. The only discipline up front is section 4's modifiable
set.

---

## 6. Security: unconsented code runs in a virtual filesystem

### 6.1 The idea

*Proposed by Alessandro in discussion, 10 November 2026.*

Suppose ransomware enters the system without the user noticing. A process
that runs **without the user's explicit consent** — the user never gave it
permission to execute, but it obtained or holds that permission some other
way — is given a **virtual filesystem that lives only in RAM**. It cannot
touch the real filesystem: whatever it encrypts, deletes or writes, it does
to its own copy, which disappears with it.

The idea has precedents, bolted on elsewhere. Sandboxie on Windows redirects
an untrusted program's writes into a sandbox. `firejail --overlay-tmpfs` on
Linux lays a tmpfs over the real filesystem and discards it at exit.
Windows' Controlled Folder Access lets only allow-listed programs write to
protected folders. In Uros no dedicated subsystem is needed: it is a
different capability set handed to the process when it is created.

### 6.2 Consent is a capability

In a capability system the ransomware does not have to be *detected*.
Consent and capability are the same thing: a process touches the real
filesystem only if something handed it the capability to.

- The real root is handed out only by a **trusted path**: the user's
  explicit action, through the launcher or a file chooser. Capability
  systems call this a **powerbox**.
- Everything else receives the virtual root **by default**.
- Capabilities cannot be forged, so code that got in unnoticed has no way to
  obtain the real one.

"Unconsented" stops being a detection problem: it is the starting state.

### 6.3 What the process sees: an entirely virtual filesystem

From the inside it looks like a complete, real filesystem — `/usr`, `/etc`,
`/home` and the rest — and nothing the process does reaches the disk. Each
part of its tree is one of three things:

- a **read-only window** onto what is safe to show (`/usr`, so the program
  runs);
- a **decoy** (a fake `/home` holding canary files);
- **RAM** (everything it writes).

The illusion is coherent. The program cannot tell it is confined, and when
it ends the illusion ends with everything it "did".

**Why it is nearly free in Uros.** Here every filesystem is *already*
virtual in the sense that matters: a filesystem is always just a server the
process's namespace points to. There is no mechanism-level difference
between "real" and "virtual". An ordinary process's namespace mounts
ext2_server and proc_server; a suspect process's namespace mounts a tmpfs
server, a read-only view and a decoy server. Same machinery, different map.
On Linux the same thing takes mount namespaces, overlayfs and policy; here
it follows directly from "the root is an inherited capability" (section 3).

### 6.4 Refinements that make it hold

1. **A copy-on-write overlay, not an empty filesystem.** An empty one breaks
   the program at once (no libraries, no configuration) and tells it it is
   caged. With an overlay it reads what it needs and its writes land in RAM:
   ransomware "encrypts" only copies in RAM, and they vanish at exit.
2. **Reads as well as writes.** Modern ransomware steals the data before it
   encrypts it (double extortion), so the real `/home` must not be visible —
   hidden, or replaced by the decoy. Of the real tree, `/usr` read-only is
   enough.
3. **Canary files.** In the decoy `/home`, a process that starts rewriting
   the canaries en masse — the pattern encryption leaves — is detected
   without risk, since it is working on the decoy, and the user is warned.

### 6.5 Holes to close

The cage holds only as well as its weakest point:

- **Confused deputy.** The process could ask a *server* that holds the real
  filesystem to write on its behalf. Servers must act with the **client's**
  authority, not their own — the direction of #911, and the problem seen in
  #932.
- **The raw disk.** block_device_server must be reachable only by filesystem
  servers; otherwise writes go underneath the filesystem.
- **The network.** Exfiltration goes through it, so the network is a
  capability of its own, not handed out by default.
- **RAM.** The overlay needs a quota, or it becomes a way to exhaust memory.

### 6.6 Promotion

If the user later decides the program is legitimate, its overlay changes can
be **applied** to the real filesystem, like committing a transaction, or the
program can be restarted with the real root. This allows an interaction
like: "this program tried to modify 2000 files in /home — allow, or
discard?"

### 6.7 What it needs

This is the **first real consumer** of the namespace manipulation API
(section 5): an overlay mount and a reduced root at spawn. It needs:

- a tmpfs server (already planned for `/tmp` and `/run`);
- union/overlay semantics in the namespace (section 5's operations);
- the root as an inherited capability (section 3), and a powerbox: a trusted
  launcher that holds the real root and hands it on only on the user's
  explicit action.

It turns the per-process namespace from good architecture into a security
feature with a precise threat model.

---

## 7. Sequencing summary

| Piece | When | Why then |
|---|---|---|
| Root = inherited capability | **now** (foundation) | avoids the global-only retrofit |
| FHS layout decided | **now** (so the userland has a home) | bring-up needs `/usr`, `/etc`, … |
| Composition: readdir crosses mounts | with the hierarchy work | the "tree" gap |
| Per-process cache as a modifiable set | when we cache resolution | the cache *is* the namespace |
| Namespace manipulation operations | at the first sandbox/chroot/union consumer | not a bring-up blocker |
| Virtual filesystem for unconsented code | with those first operations, plus a tmpfs server and a powerbox | the strongest first consumer (section 6) |

---

## References

- Rob Pike et al., *The Use of Name Spaces in Plan 9* — the canonical short
  paper on per-process namespaces, bind and union mounts.
- Thomas Bushnell, *Towards a New Strategy of OS Design* (GNU Hurd) — the
  translator concept (a file that is a server).
- Linux, as the bolted-on version of the same ideas: `man 7
  mount_namespaces`, `man unshare`; `overlayfs` (union), `mount --bind`.
- `docs/uros_design.md` §8 (the current client-side libvfs sketch),
  `docs/capability_system.md` (the capability model the mounts rest on).
- For section 6: `man firejail` (`--overlay-tmpfs`) and Sandboxie, the
  bolted-on precedents; the **powerbox** pattern from capability-secure user
  interfaces (CapDesk and the E language, Mark S. Miller et al.), whose
  modern descendants are the Flatpak portals and macOS's sandbox file
  chooser.
