<!--
  Uros native filesystem — design reference.
  Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
  Licensed under the MIT License.
-->

# Uros native filesystem — design reference

**Status:** design reference from a discussion (13 November 2026). Nothing
here is implemented or scheduled. The work starts after v0.3.0, which ends
when the x86-64 port is complete. Section 12 lists the questions still open.
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
5. **Aging is measured from the first day** (section 11): after N GB of use
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

### What contains aging, concretely

No filesystem stops aging under churn. What can be done is to keep it
bounded, by four layers, and to measure it.

1. **Place well when writing,** since aging is made by bad placement:
   - **Delayed allocation and transaction groups.** Data is allocated when
     the group commits, seconds after it was written. A file rewritten at
     random in that time gets one contiguous extent, not many small ones.
   - **Locality.** A file's data near its metadata, and the files of one
     directory in the same open segment, as FFS's cylinder groups and XFS's
     allocation groups do.
   - **Temperature.** Metadata, new data, and data the cleaner moves (cold
     by definition) go to different segments. Segments then end up either
     nearly all live or nearly all dead, and a nearly dead segment is cheap
     to clean: LFS's central observation, and F2FS's hot and cold logs.
   - **Size.** Tiny files inside the tree, small files packed together,
     large files in large extents, never interleaved.
   - **Files written at random** (databases, VM images) are recognised by
     their pattern, and their writes are gathered per file in each group.
2. **Repair in the background,** within a budget:
   - **The segment cleaner**, by cost-benefit, moves the live blocks of
     nearly empty segments into new ones, grouped by file and temperature:
     it compacts free space and defragments as it goes.
   - **Defragmentation where measured:** a file whose extents per MiB pass a
     limit is rewritten contiguously when it is idle, not on every write as
     btrfs's `autodefrag` does.
   - **Metadata compaction:** the write-optimized tree's large nodes flush
     their buffers in batches, and the cleaner rewrites scattered tree and
     directory nodes into metadata segments, which is what keeps a
     `git status` on an aged volume fast.
   - **A budget:** a share of the device's bandwidth, larger when the system
     is idle, so that repairing never costs the foreground its latency.
3. **Never run full.** A reserve of free segments the user cannot fill (5–10%
   of the volume, to be measured), and the volume says it is full before it
   is. Aging accelerates sharply near full, where no contiguous space is
   left: ZFS's cliff above about 80%.
4. **Keep snapshots and history from pinning it,** through the birth-time
   rule above: recent data, where fragmentation is, stays movable; old
   shared data stays put until its snapshot goes; history is thinned and
   kept in a log whose blocks have one owner each.

**And measure it, live:** the filesystem keeps the numbers that say how
aged it is (extents per file, a histogram of segment use, tree nodes read
per lookup, the cleaner's backlog), shows them to the user, and starts the
cleaner and the defragmentation from them. Rule 5's benchmark then checks,
before each release, that the four layers hold it within the threshold.

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

**FLIPC v2 carries the data, in batches.** Uros already has the transport for
this: FLIPC v2's shared-memory channels, which move a 4 KiB descriptor in
about 86 ns with no kernel crossing. #232 measured how to use them for reads
(libvfs to the ext2 server, KVM):

| | 4 KiB | 16 KiB | 64 KiB |
| --- | --- | --- | --- |
| a synchronous RPC per `read()` over FLIPC, against a Mach RPC | 0.19× | 0.50× | 1.28× |
| read-ahead over the same channel (a 192 KiB window per descriptor), against a Mach RPC | up to about 50× | 2.8× | 2.7× |

- **One small request, one answer, loses.** The request–reply ping-pong
  between two threads costs two context switches, about 53 µs, against about
  1.4 µs for a Mach RPC with direct thread switch and the port cache.
- **FLIPC wins where work is batched:** read-ahead, write-behind, the
  submission rings above.
- **So:** a single small request stays a Mach RPC, or a page already mapped.
  Bulk data moves over FLIPC.

Which of the two serves a `read()` that hits the cache — the mapped memory
object, or a read-ahead window filled over FLIPC — is decided by measuring
both, not by preference.

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

## 7. On-disk layout: a proposal

A first proposal, to be argued with and measured, not a decision.

**Units.**

- Blocks of 4 KiB.
- Segments of 8 MiB by default, a power of two chosen when the volume is
  made, aligned to the device's erase blocks or zones.
- An extent never crosses a segment, so the cleaner moves extents whole.

**Objects and keys.** Every file, directory and symlink is an object with a
64-bit id. All metadata lives in the one write-optimized tree of rule 3,
keyed by (object id, item type, offset). The item types:

- **object header:** type, sizes, times, flags, the object's key slot, its
  feature bits;
- **directory entry**, keyed by a 64-bit hash of the name, with a counter for
  collisions;
- **directory order item**, which gives `readdir()` cookies that stay stable
  while the directory changes;
- **extent record**;
- **inline data:** files up to about 2–4 KiB live in the tree, with no
  extent;
- **extended attribute**;
- **snapshot record:** a root pointer, a transaction group, a dead list.

**The extent record.**

| field | what it holds |
| --- | --- |
| logical offset, length | the file range, capped (say at 1 MiB) so that moving or rewriting one stays cheap |
| physical address | (segment, offset in blocks) |
| birth | the transaction group that wrote it |
| compression | the algorithm and the compressed length |
| checksums | an algorithm id, and one checksum per checksum unit |
| writer | who wrote it, for section 8; zero until used |
| key, nonce | for encryption, section 8; zero until used |
| flags | `nocow` and the like |

**Checksum units.** A checksum per extent would make a 4 KiB read fetch the
whole extent to verify it. A checksum per 4 KiB costs 8 bytes in 4 KiB with
xxHash64 (0.2%), and 32 with BLAKE3 (0.8%). The proposal:

- extents are cut into checksum units of up to 64 KiB, compressed one by one,
  so that a small read reads and decompresses one unit;
- a file written at random gets smaller units, down to 4 KiB.

**Tree nodes.** Large nodes, 64 to 256 KiB, in which updates are appended as
in bcachefs, or buffered as in a Bε-tree. A node pointer carries the address,
the child's checksum, its birth and its level.

**Commit.**

- Transaction groups close every few seconds, or after so many MiB.
- A ring of superblocks at fixed places holds the root pointer, the group's
  number and a checksum; mounting takes the newest valid one.
- An intent log in segments of its own makes `fsync` fast: it records
  operations, and mounting replays the ones after the last group.

**Space.**

- A segment usage table, in a tree of its own: for each segment its live
  bytes, and its youngest and oldest birth.
- Open segments are kept by stream, so that data of one temperature shares a
  segment, as in F2FS: metadata; new data; data the cleaner moves, which is
  cold by definition. There is one set per processor, against contention.
- A pool of free segments, and rule 2's reserve.

**The cleaner.**

- It picks segments by LFS's cost-benefit, (1 − u) · age / (1 + u), with u
  the fraction still live.
- It moves only blocks born after the most recent snapshot, or owned by the
  history log of section 9, found through the segment summaries.
- It runs when free segments fall below a threshold or the measure of
  fragmentation asks, and it yields to foreground I/O.

**Snapshots** are a root pointer, a transaction group and a dead list.
Deleting one merges its dead list into the next, as in ZFS, and says exactly
what it freed.

**Zoned devices** (ZNS SSDs, SMR disks) come almost free: a segment is a
zone, and segments are written by appending only.

**The superblock** holds a format version and three sets of feature flags,
as ext4 does: compatible, read-only compatible, incompatible. A feature
added later says by its flag what an older server may do with the volume.

## 8. Features for the user

Ideas from the discussion. All of them come from three things that do not
exist together elsewhere:

- copy-on-write with birth times, so every transaction group is a version;
- capabilities, so every write reaches the server with the identity of its
  writer, and an identity nobody can forge;
- the per-process namespace.

1. **Undo what one program did.** "Undo what this installer, script or
   ransomware changed since ten this morning", leaving everyone else's
   changes alone, and showing a conflict where someone else touched the same
   file afterwards. Research systems did this (Taser, Retro); no filesystem
   one can install does. Here the attribution is free: the server knows which
   capability wrote each extent.
2. **A continuous history, and the past as a mount.** Every transaction group
   kept for some hours, then thinned to one an hour, then one a day, and
   browsable as `~/doc.txt@10:42`. NILFS2 keeps a continuous history; what is
   ours is giving a process the past as its root: "run this program in the
   world as it was yesterday", to reproduce a bug or recover a file without
   touching the present.
3. **Who changed this file.** Each file's history says which program changed
   it, and when.
4. **Writes as proposals.** An application the user has not consented to
   writes to a copy-on-write branch of the real filesystem. The user sees the
   differences, and accepts or rejects them, like a pull request on their own
   files. It is the promotion of the namespace design's overlay (its section
   6), made persistent and cheap instead of living in RAM.
5. **Folders encrypted for a capability.** The tax folder is decrypted only
   for the application that holds its capability; anyone else sees encrypted
   bytes, even with the right user. iOS ties its protection classes to the
   device's lock state, not to applications.
6. **Sealed folders.** A root hash one can print or publish, and later the
   check "this folder is as it was that day", from any copy of it. fs-verity
   does this for single read-only files; here it covers live folders.
7. **Transactions over several files.** A package manager updates `/usr` all
   or nothing; an office program saves a document made of several files
   atomically. Microsoft's Transactional NTFS tried, on a filesystem that
   updates in place, and was deprecated for its complexity. Under
   copy-on-write a transaction is a private branch, committed in one group.
8. **Space explained.** "Deleting this snapshot frees exactly 3.2 GB",
   computed from the dead lists. btrfs cannot say it cheaply.

And one good idea nobody kept: **BeOS's live queries**, a directory that is a
saved search and updates itself. Here it would be a server in the namespace.

## 9. What the features cost, and how they are added

**The method.**

1. The filesystem is built and measured without the features: the baseline.
2. Each feature is then added as an A/B on the same build, switched on and
   off at run time, against a threshold set before the measurement.

**The format hooks are reserved from the first version:** the writer field in
the extent record, the feature flags, the key slot of each object, the
checksum algorithm id in each pointer. Without them a feature added later
would mean a new format, and its A/B would measure the migration instead of
the feature. Empty, they are in the baseline, so their own cost is measured
from the first day.

**The expected costs:**

- **Nothing on the fast path:**
  - **Writes as proposals and transactions over several files** cost only
    whoever uses them: one more level of indirection on the files a branch
    touched, and a commit that is a pointer switch in one group.
  - **Space explained** is computed when asked, from the dead lists.
  - **Sealed folders** cost nothing if the cryptographic hash is computed
    when the folder is sealed. Everyday checksums stay fast (xxHash64,
    CRC32C, over 10 GB/s per core); BLAKE3 on every block would cost CPU on
    every write.
- **Small and constant:**
  - **Writer attribution** (features 1 and 3): a few bytes per extent, beside
    its birth and checksum. The undo itself is rare and may be slow.
  - **Encryption for a capability:** CPU on disk I/O only, since the page
    cache holds plaintext and only capability holders can map the object.
    AES-GCM with AES-NI or VAES runs at several GB/s per core: a few percent
    on NVMe, and opt-in per folder.
  - **Live queries:** only for indexed attributes. Full-text indexing belongs
    to a separate, asynchronous server.
- **The real risk: the continuous history.** It does not touch the fast path,
  but it touches space and the cleaner, which is the aging rule 5 guards
  against:
  - **Every kept version is live data**, so the cleaner copies more and
    write amplification grows.
  - **It switches off the birth-time trick.** If every transaction group is
    in effect a snapshot, no block is born after the last snapshot except
    those of the last seconds, and the cleaner cannot move recent data
    without back-references.
  - **The way out:** old versions go to a **history log** in which each old
    block has exactly one owner, so that moving it updates one entry. The
    retention is bounded and the thinning firm.

  Long transactions do the same on a small scale: they pin old versions until
  they end.

**What a threshold is.** A number decided before the measurement, which
says whether the result is accepted. Decided afterwards, it moves to wherever
the result fell ("82%? close enough"); decided before, the number decides.
The C-versus-Rust measurement was set up the same way. An example for rule
5, with numbers made up for the illustration:

| metric | fresh volume | aged volume | aged / fresh | threshold: at least 80% |
| --- | --- | --- | --- | --- |
| sequential read of a 1 GB file | 1.80 GB/s | 1.50 GB/s | 83% | passes |
| random 4 KiB writes | 120,000 IOPS | 110,000 IOPS | 92% | passes |
| `git status` on a tree of 50,000 files | 0.30 s | 0.45 s | 67% | fails |

The aging between the two columns is the benchmark of section 11: a hundred
`git pull`s of a large repository, 20 GB of random rewrites of a database,
and a round of snapshots made and deleted. Here the release stops until it is
understood why an aged directory costs half as much again. For a feature the
same shape: "writer attribution may cost at most 2% on the fast path" means,
on a baseline of 120,000 random 4 KiB writes a second, at least 117,600 with
the feature on. The measurements are taken as the rest of the project's are:
one machine, A and B alternated, several runs, medians, the clock in the log,
nothing else running.

The thresholds are to be set before the first measurement, for example under
2% on the fast path for everything but encryption, and the aging benchmark
within rule 5's threshold for the history.

**When a threshold fails**, the threshold does not move. In order:

1. **Confirm the measurement.** Run it again, A and B alternated, with more
   runs, and check what else could differ: the machine, the accelerator, the
   clock, the load, the power. A failure that does not repeat is noise to
   explain, not a result.
2. **Open an issue** with the numbers. It blocks the release, or the
   feature's merge.
3. **Find where the time goes,** from the filesystem's own counters, fresh
   against aged. For the example's `git status`: nodes read per lookup,
   segments touched, cache hits, the depth of the tree.
4. **Fix the cause, and measure everything again** against the same
   thresholds, not only the metric that failed, since a fix moves others.
   Then take the fix out and show the metric falls below the threshold
   again: a fix is verified by removing it, as an ablation is.
5. **If the cause is the design itself,** a trade-off with no fix inside it,
   the user decides. Either the design changes, or that one threshold
   changes for a reason written down beside the numbers, never quietly.

A feature that fails its threshold stays out of the default: off behind its
flag until it meets it, opt-in if it cannot, or dropped.

## 10. Lineage, and what may be taken

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

## 11. How it would be built

1. **This design, and the reading** in the references.
2. **A core library over an abstract block device that also runs on
   Linux**, without the features of section 8 but with their hooks in the
   format, where it can be tested quickly:
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
4. **The baseline measured**, on the same machine, then each feature of
   section 8 added as an A/B against its threshold (section 9).

## 12. Open questions

- **Workloads and media.** A desktop or development machine on NVMe first?
  Several devices (mirrors, RAID) or one with backups?
- **The language.** Rust for this server, which would be the first, once the
  C-versus-Rust measurement is done?
- **What version 1 holds.** Snapshots and encryption from the start, or first
  copy-on-write and checksums only?
- **The aging threshold** of rule 5, to be set before the first measurement.
- **The features' thresholds** of section 9, likewise.
- **The layout's numbers:** the segment size, the cap on an extent, the
  checksum unit and its algorithm, the node size — each to be measured, not
  guessed.
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
- R. Konishi et al., "The Linux implementation of a log-structured file
  system" (NILFS), ACM SIGOPS Operating Systems Review 40(3), 2006.
- A. Goel, K. Po, K. Farhadi, Z. Li, E. de Lara, "The Taser Intrusion
  Recovery System", SOSP 2005.
- T. Kim, X. Wang, N. Zeldovich, M. F. Kaashoek, "Intrusion Recovery Using
  Selective Re-execution" (Retro), OSDI 2010.
- D. Giampaolo, "Practical File System Design with the Be File System",
  Morgan Kaufmann, 1999 — BFS's indexed attributes and live queries.
- Microsoft, "Alternatives to using Transactional NTFS".
- The Linux kernel's documentation, "fs-verity: read-only file-based
  authenticity protection".
- In this repository: `filesystem_namespace_design.md`,
  `capability_system.md`; issues #623, #724, #728, #773, #906.
