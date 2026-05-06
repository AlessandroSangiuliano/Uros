# gpu_server — Design Document

Status: **draft for 0.1.0 scoping** — architecture is the long-term target,
0.1.0 ships a minimal subset (see §10).

Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>

---

## 1. Overview

`gpu_server` is the userspace task that owns every display/graphics device
on the system. It replaces the current in-kernel VGA driver
(`osfmk/src/mach_kernel/i386/AT386/vga.c`) and is the only task allowed
to talk to GPU hardware.

It follows the standard Uros driver pattern (see `uros_design.md` §4):

- **One task per device class** (graphics), not per hardware device.
- **Pluggable back-end modules** loaded at runtime: `vga`, `bochs_dispi`,
  `qemu_stdvga`, later `intel_gen`, `amd_gcn`, `nvidia_nv50+`, and a
  `linuxdrm_compat` shim.
- **Two IPC planes**: Mach IPC for control + capability handshake,
  FLIPC v2 for the entire data hot path.
- **Capability-mediated access** via `cap_server`: a client must hold a
  capability for a specific GPU + a specific resource (display, context,
  buffer object) to operate on it.

Goals:

- Move VGA out of the kernel — kernel keeps only the early-boot
  panic/printf path on serial; everything user-visible (console,
  framebuffer, future compositor) goes through `gpu_server`.
- Define the modular ABI so adding a new GPU driver = writing a module
  shipped as `/mach_servers/modules/gpu/<vendor>.so`, no `gpu_server`
  rebuild.
- Architect for performance (gaming-grade target on real HW): zero
  copy command submission, async fences, FLIPC v2 batching for ring
  writes, GPU page tables managed by the module, IRQ delivery via
  Mach msg straight into the module.
- Keep the door open for a Linux DRM compat layer (legal because we
  run in userspace — no GPL viral effect on the kernel).

Non-goals (any of these would block shipping):

- 3D acceleration in 0.1.0.
- Wayland / X11 server in 0.1.0.
- Real Intel/AMD/Nvidia drivers in 0.1.0 — the *ABI* for them is
  designed now, the *implementations* come in later milestones.
- KMS-equivalent multi-display modesetting in 0.1.0 (single
  framebuffer + single output is enough).

---

## 2. Position in the stack

```
┌──────────────────────────────────────────────────────────────┐
│  console_server       compositor (future)      apps          │
│  (replaces in-kernel   (Wayland-style          (OpenGL/      │
│   VGA console)         display server)          Vulkan)      │
├──────────────────────────────────────────────────────────────┤
│  gpu_server  ─────────  Mach IPC (control)                   │
│              ─────────  FLIPC v2 (data: cmd, fence, vsync)   │
│  ┌──────────────────────────────────────────────────────┐    │
│  │  core: capability check, resource registry,          │    │
│  │  scheduler, fence broker, FLIPC endpoint factory     │    │
│  ├────────────┬────────────┬────────────┬───────────────┤    │
│  │  vga       │ bochs_dispi│ intel_gen  │ linuxdrm_     │    │
│  │  module    │ module     │ module     │ compat module │    │
│  └────────────┴────────────┴────────────┴───────────────┘    │
├──────────────────────────────────────────────────────────────┤
│  cap_server ◄─────── HAL ──────► OSF Mach kernel             │
│  (auth)             (PCI         (device_pci_config,         │
│                      discovery,   device_intr_register,      │
│                      hotplug)     device_dma_alloc,          │
│                                   device_map)                │
└──────────────────────────────────────────────────────────────┘
```

`gpu_server` is a HAL client (it discovers VGA/PCI display devices via
HAL's `hal_list_devices(class=DISPLAY)` and subscribes to hotplug). It
talks to the kernel only through `device_master.defs` for MMIO mapping,
DMA buffers, IRQ binding, and PCI config — same as any other userspace
driver.

The kernel does **not** know `gpu_server` exists. Early-boot kernel
output (panic, before `gpu_server` is up) goes to serial only — this is
already true for production runs. The VGA in-kernel code is deleted.

---

## 3. Resource model

Four kinds of resources, all referred to by capability handles.

### 3.1 `gpu_device`

A physical GPU. Created at startup by `gpu_server` from HAL discovery
results, plus on hotplug. Owns:

- a back-end module instance (`gpu_module_t`)
- IRQ binding(s)
- MMIO BAR mappings
- PCI config handle (read/write delegated to module)

Capability `GPU_DEV_OPEN` lets a client query device properties and
allocate child resources on it.

### 3.2 `gpu_display`

A connector + scanout pipeline (CRTC + plane in DRM terms). For VGA
this is a single fixed 80x25 text mode or a single linear framebuffer;
for real GPUs it's any combination of HDMI/DP/eDP outputs.

Capability `GPU_DISPLAY_SCANOUT` lets a client choose a buffer object
to display (page-flip / set_mode).

### 3.3 `gpu_bo` (buffer object)

A chunk of memory the GPU can DMA from / write to. Backed by:

- system RAM (CPU-coherent, mapped to client + GPU)
- video RAM (where applicable; VGA: `0xA0000–0xBFFFF`, `0xB8000+`;
  discrete cards: VRAM allocated by the module)
- scanout-eligible RAM (must satisfy alignment/tiling for the display)

`gpu_bo` is the unit of zero-copy: the module creates the GPU mapping,
the client `mmap`s the same physical pages from the cap-authorized
share-memory port. No copy on submit.

Capability `GPU_BO_RW` for client mmap; `GPU_BO_SCANOUT` separate
because scanout placement is a privileged decision (a hostile client
could place a buffer they control in front of secure UI).

### 3.4 `gpu_context`

A GPU command queue + state. Owns:

- one FLIPC v2 ring (client → module): submits command buffers
- one FLIPC v2 ring (module → client): fence completions, vsync
  callbacks, error events
- a list of `gpu_bo` it has open (so revoke can tear down)

Capability `GPU_CTX_SUBMIT`. Tied to a single `gpu_device`. Multiple
contexts per client allowed (game + compositor would each have their
own).

---

## 4. Module ABI

A back-end module is a userspace shared object loaded into the
`gpu_server` process via `libdl` (`#162` auto-bootstrap). The ABI is
deliberately small and stable.

```c
/* docs/gpu_server_design.md – exemplary, not final code */

typedef struct gpu_module_ops {
    /* Identification */
    const char *name;            /* "vga", "intel_gen", ... */
    uint32_t    abi_version;     /* GPU_MODULE_ABI = 1 */

    /* Probe: HAL hands us a PCI device handle.
     * Return non-NULL on claim, NULL on skip. */
    void *      (*probe)(hal_device_t *dev);

    /* Lifecycle */
    int  (*attach)(void *priv);
    void (*detach)(void *priv);

    /* Resource ops */
    gpu_display_t *(*display_get)(void *priv, uint32_t idx);
    int            (*display_set_mode)(gpu_display_t *, gpu_mode_t *);
    int            (*display_scanout)(gpu_display_t *, gpu_bo_t *);

    gpu_bo_t *(*bo_alloc)(void *priv, size_t size, uint32_t flags);
    void      (*bo_free)(gpu_bo_t *);
    int       (*bo_map)(gpu_bo_t *, void **out_kva); /* for blit/scanout */

    /* Submission: called by gpu_server core when a FLIPC v2 message
     * arrives on a context's submit ring. The module is responsible
     * for translating the command buffer to its hardware ring. */
    int       (*submit)(void *priv, gpu_context_t *,
                        const void *cmdbuf, size_t len,
                        gpu_fence_t *out_fence);

    /* Async events. The module calls these into core. */
    /* (filled in by core at attach time, not by the module:)
     *   void (*core_irq_done)(gpu_context_t *, uint64_t fence_id);
     *   void (*core_vsync)   (gpu_display_t *);
     *   void (*core_hotunplug)(void *priv);
     */
} gpu_module_ops_t;

/* The single exported symbol every module must define. */
extern const gpu_module_ops_t gpu_module_entry;
```

**Module lifecycle**:

1. `gpu_server` starts, asks HAL for `class=DISPLAY` devices.
2. For each device, walks installed modules in priority order and
   calls `probe`. First non-NULL claim wins.
3. Calls `attach`; module sets up MMIO maps, IRQ binding (kernel port
   → core dispatcher → `core_irq_done`), DMA buffers.
4. From then on, all client traffic to that device flows through this
   module instance.
5. On hot-unplug or `detach`, core revokes all open caps via
   `cap_server` and tears down FLIPC rings.

**Module isolation in 0.2+**: today modules run in the same process
as `gpu_server` core. A future refactor (post-0.2) can split each
module into a child task with its own address space, talking to core
via FLIPC; the ABI above already separates "what core asks the module
to do" from "what module asks core to do" so it survives that move.

---

## 5. Capability model

Every operation that touches a GPU resource is gated by a capability
issued by `cap_server`. Tokens are HMAC-signed (see `cap_server`
design); revocation is push-based (`#183` cap_revoke_notify).

| Capability       | Granted to             | Authorizes |
|------------------|------------------------|------------|
| `GPU_DEV_OPEN`     | session manager           | open device handle, query, alloc child resources |
| `GPU_DEV_ADMIN`    | gpu_server, sysadmin only | reset, fw upload, power state, debug registers |
| `GPU_DISPLAY_SCANOUT` | display server (compositor) | flip framebuffer to a connector |
| `GPU_DISPLAY_MODESET` | display server only    | change resolution / refresh / output topology |
| `GPU_BO_RW`        | any client with a context | mmap a buffer object |
| `GPU_BO_SCANOUT`   | display server only       | mark a buffer as scanout-eligible |
| `GPU_BO_EXPORT`    | client that owns the BO   | dup a BO handle to another task (dma-buf style) |
| `GPU_CTX_SUBMIT`   | any cap'd client          | push command buffers to a context's submit ring |
| `GPU_CTX_DEBUG`    | gpu_server, debugger only | inspect ring head/tail, dump GPU state, set breakpoints |
| `GPU_VBLANK_SUB`   | any client                | subscribe to vblank events on a display |

**Why scanout is privileged**: a hostile compositor-impersonator could
place a fake login prompt as the active scanout. Real compositors get
this cap from session manager; everyone else gets `GPU_BO_RW` only and
must compose into a buffer the real compositor reads.

`gpu_server` calls `cap_verify` on every Mach control RPC. FLIPC v2
data path: cap is verified once at *channel binding* (when a
`gpu_context` is opened), then every message on that channel is
implicitly authorized — same trust model as BDS (`#188`). On revoke,
`cap_server` pushes `cap_revoke_notify` and `gpu_server` tears down
the FLIPC ring so no further submissions can land.

---

## 6. IPC architecture

### 6.1 Control plane — Mach IPC

MIG-defined: `gpu_server.defs`. One subsystem per resource class:

```
gpu_device:    open, close, query_caps, query_displays
gpu_display:   set_mode, get_mode, scanout, vblank_subscribe
gpu_bo:        alloc, free, map (returns shmem port), set_tiling
gpu_context:   create (returns FLIPC channel pair),
               destroy, wait_idle
```

These are RPCs — synchronous, capability-checked, low frequency.
Performance budget: same as Mach IPC baseline (~1.5 µs round-trip
per the #55 benchmarks).

### 6.2 Data plane — FLIPC v2

`gpu_context_create` returns a *pair* of FLIPC v2 channels:

- **submit** (client → module): SPSC ring carrying command buffers.
  Each entry: `{ cmdbuf_ptr, cmdbuf_len, fence_id_out, bo_ref_table }`.
  Client writes, module consumer thread drains. No syscall on submit
  in the common case.
- **completion** (module → client): SPSC ring carrying fence signals,
  vsync events, errors. Module writes from IRQ handler thread,
  client drains.

Why FLIPC and not Mach for this:

- **Throughput**: a game submits hundreds of cmdbufs per frame.
  FLIPC v2 batched 60 commands in 0.38 µs per the project benchmarks;
  same workload over Mach RPC would be ~90 µs (60 × 1.5).
- **Fire-and-forget**: submission is async by definition (the
  fence_id is the synchronization handle). Mach wants a reply; FLIPC
  doesn't.
- **Doorbell coalescing** (future, `#124`): the client only writes
  the head pointer; the consumer notices via shared-memory poll and
  optionally a single Mach signal if the ring was empty. One signal
  per N commands instead of one per command.

> **FLIPC v2 is still evolving.** Open extension issues that
> `gpu_server` will adopt as they land: `#122` multi-channel poll
> (single thread waits on submit + completion + vsync rings),
> `#123` consume_wait_timed (bounded fence wait), `#124` doorbell
> coalescing, `#125` adaptive spin tuning, `#126` MPSC channel
> (multiple GPU clients into one module submission thread),
> `#127` channel groups with load balancing.  Design here pins the
> *baseline* (SPSC submit + SPSC completion); extensions are
> backwards-compatible.

### 6.3 Endpoint discovery

`gpu_context_create` returns a *name* (e.g.
`"gpu/ctx/47-3a91"`) registered in the FLIPC v2 endpoint registry
(`#119`).  The client then calls `flipc2_lookup(name, "submit")` and
`flipc2_lookup(name, "compl")` once at context setup to obtain the
two endpoint handles, and uses them directly for every subsequent
submit/recv — no further lookups, no further Mach RPCs on the data
path.

Why name-based registry instead of returning the FLIPC port directly
in the Mach reply: it reuses the same coordination mechanism every
other userspace server uses (BDS, ext_server, future fs/net), keeps
Mach gpu_server.defs free of FLIPC-specific OOL types, and the
two-lookup setup cost is a one-time warmup that doesn't appear in
steady-state benchmarks (data path = `flipc2_send`/`flipc2_recv` only,
no name resolution).

Buffer object handles in commands: cmdbufs reference `gpu_bo` by an
opaque per-context handle. Module looks up the handle in the context's
BO table. Caps are checked at `bo_open(context, bo_cap)` time, not on
every command — same trust model as POSIX file descriptors.

---

## 7. Linux DRM compat layer (tentative, no commitment)

> **Status**: idea for a possible future direction, no timeline, no
> commitment to ever ship it.  Documented here so the §4 module ABI
> stays compatible with such a back-end if we ever decide to build
> one.  Skip this section without losing context for anything else.

The legal angle is real: GPL only constrains the kernel image. A
*userspace* shim that calls into a GPL kernel driver linked into the
shim's process is dual-licensed-permissively (the shim can be MIT, the
shim+driver binary aggregate is GPL — distributable separately or as a
non-derivative aggregate). NetBSD's `drm-kmod`, FreeBSD's `linuxkpi`,
and Asahi's user-mode GPU driver all live in this design space.

Approach for `linuxdrm_compat`:

- **Linker shim** that exposes Linux's `struct drm_driver`,
  `struct file_operations`, `dma-buf`, `dma_fence`, plus the slice
  of `mm` / `iommu` / `mmu_notifier` / `sync_file` that DRM drivers
  actually call.
- **Kernel-API mocks** mapped onto Uros primitives:
  - `kmalloc` → `malloc`
  - `ioremap` → `device_map`
  - `request_irq` → `device_intr_register` + a thread per IRQ
  - `dma_alloc_coherent` → `device_dma_alloc`
  - `mmu_notifier_register` → no-op (we own the GPU page tables in
    userspace; the kernel never modifies the GPU MMU under us)
  - `wait_event_interruptible` → FLIPC v2 wait
  - workqueues, tasklets → pthreads
- **Per-driver glue**: the upstream `i915_drv.c` builds against this
  shim with minimal patches (mostly `#ifdef __UROS__` around things
  that don't make sense in userspace).

Status: design sketch only.  No milestone target.  Worth pursuing only
when *all* of the following are true:

- a working first-party module exists (so we know the FLIPC + cap
  model survives a real driver), AND
- there is concrete demand (external contributor or specific HW we
  cannot easily write a first-party driver for), AND
- we have engineering bandwidth to commit to multi-year LTS-tracking
  of Linux internal APIs.

The ABI (§4) is designed so a `linuxdrm_compat` module would be just
"another back-end" — it doesn't need any special hooks in core.  That
property is the real deliverable of this section: keeping the door
open without committing to walk through it.

---

## 8. Bringup details (0.1.0 scope)

### 8.1 What ships

A `gpu_server` binary loadable from the bootstrap bundle (`#186`)
with one back-end statically linked: `vga`. The minimal feature set:

- HAL probe for class=DISPLAY (or fallback to legacy VGA at
  `0xA0000`/`0xB8000` if HAL doesn't yet enumerate non-PCI VGA).
- Single back-end (`vga`) supporting:
  - 80×25 text mode (replicates current kernel VGA behavior)
  - linear framebuffer if BGA/Bochs DISPI is available (QEMU
    `-vga std`); else stays in text mode.
- A text RPC interface exposed by `gpu_server` directly:
  `gpu_text_puts(buf, len)` (no `putc`, see §11.1).  Defined as a
  MIG **simpleroutine** — fire-and-forget, no reply, so the
  producer (bootstrap, cap_server, ...) never blocks waiting for
  VGA paint.
- The kernel-log forwarder is a small thread inside `bootstrap`
  (~50 lines) that drains the kernel log buffer and calls
  `gpu_text_puts` in chunks.  No separate `console_server` task in
  0.1.0 — see §10.1 for why and when one would appear.
- **cap_server is integrated in 0.1.0**: every Mach control RPC on
  `gpu_server` is gated by `cap_verify`. The forwarder thread in
  bootstrap holds a `GPU_DEV_OPEN + GPU_BO_RW +
  GPU_DISPLAY_SCANOUT` cap set issued at startup. Any other
  prospective client needs caps from cap_server. This avoids the
  "cap-less skeleton that we forget to gate later" trap.

What is **not** in 0.1.0:

- 3D / cmdbuf submission / fence / context (only the text plane).
- Multiple `gpu_context` per client (`console_server` uses exactly
  one; the type exists but only one instance per task).
- Buffer object eviction, tiling formats, cross-task BO export
  (`GPU_BO_EXPORT`) — caps defined, ops stubbed.
- Hotplug (single VGA at boot is enough).
- Linux compat (§7 is sketch only).

What 0.1.0 DOES guarantee:

- The module ABI (§4) compiles and is exercised by `vga`.
- The directory layout `/mach_servers/modules/gpu/` exists and
  `gpu_server` looks for additional modules there (none yet, but
  the loader path is wired so dropping `bochs_dispi.so` later
  works without recompile).
- The in-kernel VGA driver is removed, kernel logs go to serial
  only, userspace logs go through `gpu_server`.

### 8.2 Files added in 0.1.0

```
osfmk/src/mach_services/gpu_server/
    main.c              — task entry, MIG dispatch loop, render thread
    core.c              — module loader, resource registry, cap_verify glue
    text_render.c       — puts → cell grid, scroll via memmove
    module_abi.h        — public §4 ABI
    gpu_server.defs     — MIG: gpu_device_open, gpu_query_displays,
                                gpu_text_puts (simpleroutine)
    modules/
        vga.c           — text mode backend (CRTC ports, font ROM, A0000)

osfmk/src/bootstrap/
    log_forwarder.c     — thread that drains kernel log buffer and
                          calls gpu_text_puts in chunks (~50 lines)

osfmk/export/include/gpu/
    gpu_types.h         — public types (gpu_mode_t, gpu_bo_flags_t,
                          gpu_cap_t, gpu_ctx_id_t, ...)
    gpu_module_abi.h    — module-side ABI (§4)
```

Files **deleted** in 0.1.0:

```
osfmk/src/mach_kernel/i386/AT386/vga.c        — moved to userspace module
osfmk/src/mach_kernel/i386/AT386/vga.h        — gone
   (kernel keeps console_serial.c only for early-boot panic)
```

---

## 9. Roadmap

| Milestone | Scope | Issue cluster |
|-----------|-------|---------------|
| **0.1.0** | gpu_server skeleton + vga module + module ABI; in-kernel VGA removed | (this doc) |
| **0.2.0** | FLIPC v2 data path; gpu_context + cmdbuf submission; `bochs_dispi` module for QEMU framebuffer | TBD |
| **0.3.0** | First first-party real-HW module (likely `intel_gen` for older HD Graphics — we have target HW); cap_server integration; hotplug | TBD |
| **0.4.0** | `linuxdrm_compat` skeleton + first borrowed Linux driver (probably `nouveau`, has the cleanest userspace-friendly design) | TBD |
| **0.5.0** | 3D acceleration via Vulkan-WSI port; compositor-grade synchronization | TBD |
| **future**| `amd_gcn`, `nvidia_nv50+` first-party; HDR; multi-GPU | TBD |

Each milestone's design decisions get pinned in this doc as a §addendum
when the milestone closes, so future readers see what was decided
and why.

---

## 10. Open design questions

Three decisions resolved during the 0.1.0 design discussion are
recorded here for future readers; the rest stay open until the
milestone they impact.

### Resolved (2026-05-06)

1. **Where does `console_server` live in 0.1.0?** — *Resolved: there
   is no `console_server` in 0.1.0.*  `gpu_server` exposes
   `gpu_text_puts` directly; a small forwarder thread inside
   `bootstrap` drains the kernel log buffer.  A separate
   `console_server` task would only make sense if it had real work
   to do (multi-VT switching, session management, input
   multiplexing) — none of which exist in 0.1.0.  When Arcan lands
   as the userspace display server, the "console" concept dissolves
   into "Arcan + a terminal app over shmif", and `gpu_text_puts`
   stays as the early-boot/panic path only.  Either way, no
   `console_server` middle layer is needed in any milestone.

2. **Cap_server integration timing.** — *Resolved: in 0.1.0.*  Every
   `gpu_server` Mach RPC is gated by `cap_verify` from the start.
   `console_server` gets a minimal cap set (`GPU_DEV_OPEN +
   GPU_BO_RW + GPU_DISPLAY_SCANOUT`) issued by bootstrap.  No
   "cap-less skeleton we forget to gate later".

3. **FLIPC v2 endpoint registration.** — *Resolved: name-based via the
   FLIPC v2 endpoint registry (`#119`).*  See §6.3.

### Still open

4. **Does the kernel keep any VGA fallback?**
   Lean: no — kernel goes serial-only for panic; if `gpu_server`
   crashes, the panic is on serial, the screen freezes with the last
   frame. This matches Linux DRM behavior.  Decision before code
   lands.

5. **Multi-context arbitration on a single GPU.**
   When two contexts on the same `gpu_device` both submit, what's
   the scheduling? Round-robin per-context, priority, or pure FIFO?
   For 0.1.0 N/A (one context); decision before 0.2.

6. **If we ever do Linux compat: which DRM API version to target?**
   Pin to an LTS to avoid chasing internal-API churn.  No target
   chosen because §7 has no commitment.

---

## 11. Performance considerations (text path, 0.1.0)

The text path (`gpu_text_puts`) is intrinsically low-bandwidth, but
the wrong API shape can turn it into the dominant cost during a log
dump.  This section pins the rules that keep it cheap.

### 11.1 Cost baselines

Measured / known on KVM (see ipc_bench results in CLAUDE memory):

| Operation                                     | Cost              |
|-----------------------------------------------|-------------------|
| Mach intra-task null RPC (routine, with reply)| ~1.5 µs          |
| Mach intra-task simpleroutine (no reply)      | ~0.7 µs          |
| Mach payload up to ~1 KB                       | "free" (no cost above null) |
| FLIPC v2 send (shared-memory ring)            | ~50 ns           |
| Write to VGA text RAM (0xB8000, MMIO uncached, KVM)| ~80 ns/byte |
| Write to VGA text RAM (real HW, uncached)     | ~150 ns/byte     |

### 11.2 Workload sizing

| Scenario                          | Volume       | per-byte putc | batched puts | FLIPC v2 |
|-----------------------------------|--------------|---------------|--------------|----------|
| Boot log                          | ~5000 chars  | 7.5 ms        | <100 µs      | <10 µs   |
| Verbose runtime log               | 1000 ch/s    | 1.5 ms/s      | trascurabile | trascurabile |
| `dmesg` snapshot of running system| ~50 KB       | 75 ms         | ~150 µs      | ~10 µs   |
| Anomalous dump (core trace)       | 1 MB         | **1.5 s**     | ~375 µs      | ~12 ms   |

The 1.5 s on the per-byte column is the trap to avoid; the puts
column is acceptable for every realistic case; FLIPC v2 would be
overkill at this scale.

### 11.3 Rules pinned for 0.1.0

1. **No `putc` in the Mach API — only `puts(buf, len)`.** A `putc`
   wrapper, if needed, is a libgpu client-side helper that batches
   into a local buffer and flushes via `puts`.  The MIG `.defs` does
   not expose a single-char entry point at all, by design.

2. **`gpu_text_puts` is a MIG `simpleroutine`** (no reply).  Producer
   never blocks waiting for VGA paint.  Cost ≈ 0.7 µs vs ~1.5 µs of
   a routine, *and* zero stall on the producer side.  Lost chars
   during gpu_server overload are accepted as best-effort log
   semantics.

3. **Scroll via `memmove` of VRAM-to-VRAM**, not redraw.  Full scroll
   on 80×25 ≈ 320 µs on KVM, ~600 µs on HW.  Acceptable up to
   ~60 scroll/sec.

4. **Single rendering thread + bounded internal queue** in
   gpu_server.  Producers (bootstrap, cap_server, ext_server, …)
   serialize naturally on the queue; the rendering thread drains
   in order.  Lockless SPSC if a single producer is enforceable;
   short critical-section spinlock otherwise.  No cursor races.

5. **Text mode only in 0.1.0** (VGA 80×25, A0000/B8000).  BGA/Bochs
   DISPI graphics framebuffer support arrives in 0.2 with
   dirty-rect tracking and scroll-by-blit; today's 0.1.0 doesn't
   need rasterized glyphs and shouldn't pay for them.

6. **No FLIPC v2 for the text path in 0.1.0.**  At realistic
   sizes (≤ 50 KB bursts) Mach `puts` simpleroutine is already
   below 200 µs; FLIPC's additional 10× speedup is over a value
   that's already negligible.  FLIPC v2 enters the design where it
   matters: cmdbuf submission in 0.2 (§6.2).

### 11.4 Where performance can still hurt

- **1 MB+ log dumps**: the IPC cost is fine (~375 µs with batched
  puts), but the **VGA scroll** of 25 × ~5000 = ~125000 lines of
  text takes 125 000 × 320 µs ≈ **40 seconds**.  This is a property
  of the VGA hardware, not of our IPC.  Mitigations if it bites:
  rate-limit the renderer to 1 scroll per N puts when backlog grows,
  switch to graphics mode + scroll-by-blit (0.2), or just accept
  that dumping a megabyte of log to the screen is slow on a CRT-era
  text mode and route huge dumps to serial only.

- **Concurrent producers** without serialization would shred the
  cursor.  Rule (4) above prevents it.

- **Real HW VGA** is roughly 2× slower than KVM VGA for the same
  byte count.  Numbers above already account for this in the ~150
  ns/byte column.

- **Future graphics mode (post-0.1.0)**: a naive "rasterize each
  glyph from font ROM" of a 1 MB log into a 1024×768 framebuffer
  is ~128 MB of byte writes ≈ 1.5 s of pure VRAM traffic.  Mandatory
  mitigations before enabling graphics mode console: dirty-rect
  damage tracking, scroll-by-blit (memmove of N rows, then redraw
  only the bottom row), double-buffered atomic page flip.
