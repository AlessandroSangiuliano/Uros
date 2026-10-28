# Lazy FPU Context Switch — Possible Future Implementation

## Motivation

On every context switch the kernel saves and restores the whole FPU/SSE/AVX
state via FXSAVE/XSAVE. With full XSAVE (AVX-512) the buffer can reach ~2 KB.
For threads that never use the FPU (kernel threads, pure IPC loops) this is
pure waste. The lazy mechanism eliminates save/restore when not necessary.

Reference measurement: on the current ipc_bench (~500k msg/s on KVM) the
FXSAVE alone weighs an estimated 80-150 cycles per switch.

## Mechanism

The processor has the `CR0.TS` flag (Task Switched). When it is set, any
FPU/SSE/AVX instruction raises an `#NM` trap (Device Not Available, vector 7).

Lazy flow:

```
context_switch(old, new):
    save the integer registers of old
    if fpu_owner == old:
        FXSAVE/XSAVE → old->pcb->fps   # saves only if necessary
        fpu_owner = NULL
        CR0.TS = 1                       # arms the trap for new
    restore the integer registers of new
    # new's FPU is not restored here

#NM handler (first FPU use of the current thread):
    clts                                 # disarms CR0.TS
    if current->pcb->fps.initialized:
        FXRSTOR/XRSTOR ← current->pcb->fps
    else:
        FINIT                            # absolute first use
    fpu_owner = current
    iret
```

## Files involved

| File | Change needed |
|------|---------------------|
| `i386/AT386/model_dep.c` | set `CR0.TS` after `fpu_init()` |
| `i386/fpu.c` | `fpu_save()`, `fpu_switch()`, `#NM` handler |
| `i386/fpu.h` | `kernel_fpu_begin()` / `kernel_fpu_end()` |
| `i386/AT386/model_dep.c` | register the vector 7 handler |
| `kern/thread.c` | `thread_dispatch()` → set `CR0.TS` if the thread changes |
| `i386/pcb.c` | `fps.initialized` field in `pcb_t` |

Part of the infrastructure already exists: `pcb->ims.ifps` and `fp_kind` in `i386/fpu.h`.

## Critical problem: FPU in the kernel

Since issue #43, `kern/printf.c` uses FPU arithmetic for the `%f/%e/%g`
conversion. With `CR0.TS` set, a `printf("val=%f", x)` in the kernel causes `#NM`.

Possible solutions (in order of preference):

### Option A — Purely integer float conversion in printf (recommended)
Rewrite the `%f/%e/%g` conversion in `kern/printf.c` without FPU instructions,
using only integer arithmetic (as Linux's `lib/vsprintf.c` does). It removes the
problem at the root. Cost: ~200-300 lines of integer-only code for a simplified
Grisu/Dragon4.

### Option B — kernel_fpu_begin / kernel_fpu_end
Add wrappers that bracket every FPU use in the kernel:

```c
/* i386/fpu.h */
static inline void kernel_fpu_begin(void) {
    clts();               /* CR0.TS = 0 */
    /* possible FXSAVE of the current thread if fpu_owner != NULL */
}
static inline void kernel_fpu_end(void) {
    stts();               /* CR0.TS = 1 */
}
```

It requires discipline: every site in the kernel that uses the FPU must be marked.
Currently only `kern/printf.c` (the float block) needs it.

## SMP problem

With NCPUS > 1, if a thread migrates from CPU-A to CPU-B while it has "dirty" FPU state
on CPU-A, CPU-A must save the state before CPU-B can restore it.
It requires a synchronization IPI (Inter-Processor Interrupt):

```
cpu_A: context_switch(old=T, new=X)
    if fpu_owner[cpu_A] == T && T migrates to cpu_B:
        FXSAVE → T->pcb->fps
        fpu_owner[cpu_A] = NULL
        CR0.TS[cpu_A] = 1
    # cpu_B can now FXRSTOR freely
```

Not relevant as long as Uros runs on a single core.

## Signal delivery

In `sendsig()` (signal delivery to userspace), the FPU state must be included in the
signal frame. With lazy save, if `fpu_owner == current` but the save has not
happened yet, it must be forced before copying `pcb->fps` into the frame:

```c
/* before building the signal frame */
if (fpu_owner == current_thread()) {
    fxsave(&current_thread()->pcb->fps);
    fpu_owner = NULL;
    stts();
}
```

## Expected gain estimate

| Scenario | Cycles saved per switch |
|----------|------------------------------|
| Kernel thread (no FPU) | ~100-200 (FXSAVE eliminated) |
| User thread without active FPU | ~100-200 |
| User thread with active FPU | 0 (save/restore anyway) |
| ipc_bench (pure IPC) | ~15-25% estimated latency |

## Prerequisites before implementing

1. Rewrite the `kern/printf.c` float code without FPU (Option A) or add
   `kernel_fpu_begin/end` (Option B).
2. Read and fully map `i386/fpu.c` — the existing infrastructure
   may already be partially lazy or make incompatible assumptions.
3. Add FPU regression tests (a float computation in userspace verifiable
   after many context switches) before and after the change.

## References

- Intel SDM Vol. 3A, §13.5 — XSAVE/XRSTOR, §2.5 — CR0.TS
- Linux `arch/x86/kernel/fpu/core.c` — lazy reference implementation
- OSF Mach `i386/fpu.c`, `i386/fpu.h` — existing scaffolding
