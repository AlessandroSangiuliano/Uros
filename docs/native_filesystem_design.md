<!--
  Uros native filesystem — design reference.
  Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
  Licensed under the MIT License.
-->

# Uros native filesystem — design reference

**Status:** design reference from a discussion (13 November 2026). Nothing
here is implemented or scheduled. The work starts after v0.3.0, which ends
when the x86-64 port is complete. Section 9 lists the questions still open.
When a piece is decided or built, say so here.

Uros needs a filesystem of its own: modern, fast, secure and robust. This
document records what that means for Uros in particular, the one failure it
must not repeat, and the design those two lead to. The filesystem *namespace*
— the hierarchy, per-process roots, the virtual filesystem for unconsented
code — is in `filesystem_namespace_design.md`. This document is about what
sits under a mount point: the on-disk format and the server that keeps it.

---

## 1. Where it sits in Uros

A filesystem here is not a kernel module, and that changes the design.

- **A user-space server per volume.** It sits above the block layer
  (`block_device_server` today, the volume server of #773 later) and below
  the namespace, where a process's root is an inherited capability. A crash
  of the server takes down that volume, not the kernel. Each volume can have
  its own server task, so a USB stick nobody trusts is parsed in a task of
  its own.
- **The kernel's VM is the page cache.** The pagers of files stay: on 2
  November 2026 the default pager went from x86-64, and anonymous memory to
  the kernel, but files keep their external pagers. The filesystem server is
  the pager of its files, and the kernel's VM is the one cache of their
  pages. Section 5 is built on this.
- **Copy-on-write already has consumers.** The virtual filesystem for
  unconsented code (namespace design, section 6) wants a copy-on-write
  overlay. So does `/etc` with a free factory reset over a read-only `/usr`.

## 2. The basic choice: copy-on-write, with each checksum in the parent

The design is copy-on-write: the family of ZFS, APFS, HAMMER2 and bcachefs,
not the update-in-place-with-a-journal family of ext4 and XFS.

- **Robust by construction.** A live block is never overwritten. A commit
  writes a new tree and then moves the root, so a crash at any moment leaves
  the previous tree intact. No fsck in the ordinary course.
- **End-to-end integrity.** A block's checksum is kept in its parent, which
  makes the tree a Merkle tree. That catches lost writes and writes that went
  to the wrong place. A checksum inside the block itself cannot: an old block
  that is valid on its own checks out.
- **Transaction groups.** Writes are gathered and committed together, so the
  copy up to the root is paid once per group, not once per write.
- **Snapshots and clones nearly free**, which the consumers of section 1
  need.
- **A restart is a power cut, and power cuts cost nothing.** A server that
  crashes starts again from the last committed root. A microkernel makes the
  crash survivable; copy-on-write makes the restart trivial.

The costs, said plainly:

- random overwrites fragment files (databases, VM images, browser profiles,
  logs);
- each write copies its path up to the root, which transaction groups soften;
- counting free space under snapshots is hard;
- a fast `fsync` needs a small intent log beside the tree.

The first and third are where btrfs went wrong, which is the next section.

## 3. What must not happen: getting slow with age

btrfs is known to slow down after a while of use. It does so for specific
reasons, each of which another design avoids.

1. **Random overwrites fragment files.** Under copy-on-write every small
   overwrite lands elsewhere, and a file rewritten at random ends up in
   thousands of extents: a sequential read becomes a random one. btrfs's
   remedies are worse than the disease. `autodefrag` rewrites on every
   write; `nodatacow` gives up checksums and compression.
2. **Reference counts with back-references on every extent.** Each
   copy-on-write update changes the extent tree, and with many snapshots
   sharing blocks those updates multiply. Deleting a snapshot means walking
   and dropping references, with heavy background I/O. Quotas (qgroups)
   over many snapshots are notorious.
3. **Space split into data chunks and metadata chunks.** A disk can be
   "full" with free space in it, until a manual `balance`, which is slow
   and heavy.
4. **Small, scattered metadata updates.** A classic B-tree with small nodes,
   updated copy-on-write, spreads over the disk with time.

How others avoid them:

- **ZFS: snapshots by birth time.** No per-block reference counts. Each
  block records the transaction group it was born in, and each snapshot a
  list of the blocks that died under it. Taking a snapshot is O(1), and
  deleting one costs what changed, not what is shared. The price: ZFS cannot
  tell who points to a block, so it cannot move one (the "block pointer
  rewrite" never came), hence no defragmentation and no shrinking.
- **LFS, F2FS, bcachefs: segments and a cleaner.** Allocation in large
  segments, and a background cleaner that moves live data out of segments
  that are mostly empty, so free space stays contiguous. The cleaning is
  done when a measure of fragmentation asks for it, not on every write.
- **BetrFS: a write-optimized metadata tree.** A Bε-tree, large nodes with
  message buffers: random inserts are cheap and scans stay sequential.

Conway et al. measured exactly this (FAST 2017). They aged filesystems with
repeated `git pull`s: btrfs, ext4, XFS and ZFS all slowed down markedly,
while BetrFS stayed close to its fresh speed.

## 4. The rules

1. **Snapshots by birth time and dead lists**, never reference counts
   updated on every write.
2. **Allocation in large segments**, aligned to SSDs' erase blocks, with a
   background cleaner driven by a measure of fragmentation. A space reserve
   for the cleaner: the volume says it is full before it is. Never a manual
   `balance`, and one space for data and metadata.
3. **Metadata in a write-optimized tree**: a Bε-tree, or nodes structured as
   logs as in bcachefs, with large nodes.
4. **Random overwrites are a first-class case.** They are gathered in the
   transaction groups, and hot files are defragmented when they are idle,
   not on every write. A voluntary per-file `nocow` that still keeps
   checksums: a torn write is then detected, though not prevented.
5. **Aging is measured from the first day** (section 8): after N GB of use
   the volume's speed must stay within a threshold of its fresh speed. The
   threshold is set before the first measurement, not after.

### How rules 1 and 2 fit together

The two pull against each other, and this is where the others ran aground.
ZFS's snapshots are cheap because it keeps no back-references, and for the
same reason it cannot move a block. btrfs moves blocks, and pays for its
back-references on every write. bcachefs keeps back-pointers in a tree of
their own: it works, but the bookkeeping is there.

Birth time gives a way out almost for free. **A block born after the most
recent snapshot belongs to the live tree alone**, and comparing two numbers
says so.

- **What the cleaner moves:** those blocks, by rewriting their path as an
  ordinary copy-on-write write. No references to update.
- **Where the trouble is:** the fragmentation of random overwrites is almost
  all in recent, live data — exactly those blocks.
- **Old shared data stays put.** It is usually cold, and was written in large
  pieces when it was new. When the snapshot that holds it is deleted, it can
  be moved again.
- **Finding the live blocks of a segment:** LFS's segment summary. For each
  block it says whose it is; it is written once, with the data, and never
  updated. The cleaner asks the tree whether the block is still live.

## 5. Fast

In a microkernel the enemy is **one IPC per `read()`**. Cached data must not
go through the server.

- **The kernel's VM is the only page cache.** libvfs serves `read()` and
  `write()` from the file's mapped memory object. The server sees metadata
  and cache misses only.
- **Shared submission rings** between client and server and between server
  and block layer, so work moves in batches, down to NVMe's per-processor
  queues (#623).
- **A modern layout:** extents, delayed allocation, allocation groups per
  processor, small files inside the metadata, a B+tree keyed by (object,
  type, offset), per-extent compression (LZ4 or zstd).

## 6. Secure

- **The disk is untrusted input.** On a USB stick a filesystem's parsing bugs
  become exploits; ext2 today takes its geometry from the disk on trust
  (#724). This is the strongest argument for a memory-safe language, which
  matches the direction for servers in Rust — after the C-versus-Rust
  measurement decided earlier. And fuzzing of images from the start.
- **One server task per volume**, so a hostile image reaches one task.
- **Encryption at rest with AEAD.** Copy-on-write makes it clean: a block is
  never rewritten in place, so every write gets a fresh nonce. MACs in the
  tree make tampering visible too.
- **A sealed `/usr`:** its root hash verified at boot, as APFS's signed
  system volume does. It matches the merged, read-only `/usr` of the
  namespace design.
- **Capabilities.** Rights travel with the file handle, as in the rest of the
  system; the Unix personality maps POSIX permissions onto them.

## 7. Lineage, and what may be taken

| rule | where it comes from |
| --- | --- |
| checksums in the parent, transaction groups | ZFS (and HAMMER2) |
| snapshots by birth time, dead lists | ZFS |
| segments and a background cleaner, segment summaries | LFS (Sprite), F2FS, bcachefs's copygc |
| a write-optimized metadata tree | BetrFS (Bε-tree), bcachefs's log-structured nodes |
| sealed system volume, per-file encryption | APFS |
| a server per volume, capabilities, per-process namespace | Uros |

In one line: **LFS's segments and cleaner, ZFS's snapshots and checksums,
BetrFS's metadata, on a capability microkernel.** As a whole it would look
most like bcachefs, with ZFS's snapshots in place of its versioned keys.

Licences decide what may be taken. ZFS is CDDL, and btrfs and bcachefs are
GPL: they are studied, never copied. The LFS and BetrFS papers, and
HAMMER2's design (BSD), may be used with citation.

## 8. How it would be built

1. **This design, and the reading** in the references.
2. **A core library over an abstract block device that also runs on
   Linux**, where it can be tested quickly:
   - a crash simulator that cuts the write stream at every point and checks
     the tree;
   - injected bit flips, torn writes and writes that land elsewhere;
   - fuzzing of images;
   - the aging benchmark of rule 5: Conway et al.'s `git pull` method, plus
     a database, a VM image and a round of snapshots.

   It is the discipline of the ablations, applied to a filesystem: each
   guarantee has a test that is shown to fail without it.
3. **The Uros server on top of the library:** the block layer below, pager to
   the VM, libvfs above. Then the boot volume.

## 9. Open questions

- **Workloads and media.** A desktop or development machine on NVMe first?
  Several devices (mirrors, RAID) or one with backups?
- **The language.** Rust for this server, which would be the first, once the
  C-versus-Rust measurement is done?
- **What version 1 holds.** Snapshots and encryption from the start, or first
  copy-on-write and checksums only?
- **The aging threshold** of rule 5, to be set before the first measurement.
- **The name.** Servers here have Greek names; *Mnemosyne*, memory, was
  proposed.

## References

- M. Rosenblum, J. K. Ousterhout, "The Design and Implementation of a
  Log-Structured File System", SOSP 1991; ACM TOCS 10(1), 1992.
- O. Rodeh, "B-trees, Shadowing, and Clones", ACM Transactions on Storage
  3(4), 2008 — the copy-on-write B-trees btrfs is built on.
- Sun Microsystems, "ZFS On-Disk Specification", 2006 draft.
- D. Hitz, J. Lau, M. Malcolm, "File System Design for an NFS File Server
  Appliance" (WAFL), USENIX Winter 1994.
- C. Lee, D. Sim, J. Hwang, S. Cho, "F2FS: A New File System for Flash
  Storage", FAST 2015.
- W. Jannen et al., "BetrFS: A Right-Optimized Write-Optimized File System",
  FAST 2015.
- A. Conway et al., "File Systems Fated for Senescence? Nonsense, Says
  Science!", FAST 2017.
- S. Kadekodi, V. Nagarajan, G. Ganger, G. Gibson, "Geriatrix: Aging what you
  see and what you don't see", USENIX ATC 2018.
- T. S. Pillai et al., "All File Systems Are Not Created Equal: On the
  Complexity of Crafting Crash-Consistent Applications", OSDI 2014.
- J. Mohan et al., "Finding Crash-Consistency Bugs with Bounded Black-Box
  Crash Testing" (CrashMonkey), OSDI 2018.
- K. Overstreet, "bcachefs: Principles of Operation".
- M. Dillon, HAMMER2 design document, DragonFly BSD
  (`sys/vfs/hammer2/DESIGN`).
- Apple, "Apple File System Reference".
- In this repository: `filesystem_namespace_design.md`,
  `capability_system.md`; issues #623, #724, #728, #773, #906.
