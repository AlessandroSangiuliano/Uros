# OSFMK Mach Kernel CMakeLists.txt
# Target: i386 (AT386)
#
# Build order:
# 1. Build migcom (tool) - done in parent
# 2. Generate assym.S from genassym.c
# 3. Generate MIG stubs from .defs files  
# 4. Compile C and assembly sources
# 5. Link kernel with kernel.ld

cmake_minimum_required(VERSION 3.21)

# Kernel is freestanding - no standard C library
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

# =============================================================================
# Build Type Configuration
# =============================================================================
# Usage: cmake -DCMAKE_BUILD_TYPE=Debug ..   (or Release)
# Debug:   -O0 -g3 (full debug info, no optimization)
# Release: -O2 -g  (optimized with basic debug info)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING 
        "Build type: Debug or Release" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Set optimization and debug flags based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(KERNEL_OPT_FLAGS -O0 -g3 -DDEBUG=1)
    message(STATUS "Debug build: -O0 -g3 (no optimization, full debug)")
else()
    set(KERNEL_OPT_FLAGS -O2 -g)
    message(STATUS "Release build: -O2 -g (optimized)")
endif()

# ⚠️ The block above sits ahead of the x86-64 branch on purpose: that branch
# returns before the rest of this file runs, so anything set after it applies
# to i386 alone.  The optimisation flags were in that position and the x86-64
# kernel was built at -O0 by omission — which for #410 in particular is the
# one configuration where a missing volatile or a missing compiler barrier
# cannot show itself.

# ------------------------------------------------------------------ #406
# x86-64 kernel: build the machine-dependent boot stub and stop.
#
# The i386 machinery below assembles the whole SMP/pmap/trap kernel, none
# of which compiles for x86-64 until the MD contracts (#407-#411) land.
# This branch builds contract 1/6 — the boot entry in x86_64/ — as the
# `mach_kernel` output, and returns before the i386 code runs.  The stub
# grows into the real kernel as the other contracts fill x86_64/ in.
#
# The image is ELF64: GRUB boots it via multiboot2 (`multiboot2` loads an
# ELF64 kernel; qemu's `-kernel` multiboot1 loader rejects ELF64 — "give a
# 32bit one" — so the dev loop is a GRUB rescue ISO, `qemu -cdrom`).  The
# object is assembled -m64; the entry itself is still 32-bit code (the
# bootloader hands off in protected mode) via a `.code32` section, and the
# 64-bit half begins after the long-mode transition — one file, two modes.
# The x86-64 tree is one directory per MD contract, so a contract's code,
# its headers and its build inputs sit together and nothing has to be
# untangled later:
#   boot/     #406 long-mode entry, first page tables, descriptor tables
#   pmap/     #407 four-level paging
#   cpu/      shared: control registers, MSRs, CPUID (no single owner)
#   thread/   #408 context switch and thread state
#   trap/     #409 trap and interrupt entry
#   sync/     #410 atomics, locks, barriers
#   syscall/  #411 SYSCALL/SYSRET
# None of those names collide with a top-level kernel directory (vm/, kern/,
# ipc/ ...), so <pmap/pte.h> can never resolve to the wrong header once the
# MI tree is on the include path too.

# #482 copy-on-write phase breakdown: a timestamp at the top of trap_dispatch()
# and twelve more down the fast fault path, kept per processor and printed
# every sixteen faults.  It widens the fault it measures -- the dump says by
# how much -- so it is a measuring build, not a normal or a perf one.
#
# 🔴 DECLARED HERE, above the branch, and appended inside BOTH arms.  The two
# targets do not share a define list: x86-64 builds KERNEL_DEFINES_BARE in its
# own arm and i386 builds KERNEL_DEFINES in the other, so an option declared in
# one arm is not merely absent from the other -- it is accepted on the command
# line and silently does nothing there.  That is how a measuring build comes to
# look exactly like a measuring build and behave like an ordinary one.
option(UROS_FAULT_PROFILE "Build the #482 copy-on-write fault phase profile" OFF)
option(UROS_SYSCALL_PROFILE "Build the #411 Mach trap phase profile, which #392 consumes" OFF)
# #537, both targets: the capability check hashes the HMAC key's two blocks
# again on every token, as it did before the key was made ready once.
option(UROS_ABLATE_537_HMAC_FULL "Build the #537 ablation: every capability check hashes the HMAC key's padded blocks again" OFF)
option(UROS_ABLATE_537_MAC_EVERY_TIME "Build the #537 ablation: the in-kernel capability check never answers from the verified-token cache" OFF)
option(UROS_ABLATE_537_EPOCH_KEPT "Build the #537 ablation: a revocation leaves the cache epoch, so the cache outlives it (the self-test must say WRONG)" OFF)
option(UROS_ABLATE_537_PAGE_WALK "Build the #537 ablation: a DMA page is found by walking its region, not by the sorted index" OFF)
option(UROS_ABLATE_537_IX_UNSORTED "Build the #537 ablation: the page index is left unsorted (the self-test must say WRONG)" OFF)
option(UROS_ABLATE_537_HMAC_LOCKED "Build the #537 ablation: the capability MAC is computed with cap_lock held, as before" OFF)
option(UROS_PROBE_537_CAP_CONCURRENCY "Build the #537 probe: capability checks timed alone and on every processor at once" OFF)
# #615, both targets: thread_setrun()'s unbound branch puts the bound thread it
# displaces from a processor's next_thread on the processor set's queue again.
option(UROS_ABLATE_615_DISPLACED_TO_SET "Build the #615 ablation: a bound thread displaced from its processor's next_thread goes to the set's run queue, where any processor may take it (pmap_bench, -M, must say WRONG)" OFF)
# #616, x86_64: every thread is born with a user frame again, kernel threads too.
option(UROS_ABLATE_616_KERNEL_FRAME "Build the #616 ablation: a kernel thread gets a zeroed user frame, so thread_get_state answers for it (state_test, entry 7, must say WRONG)" OFF)
# #612, x86_64: the clock burn-in returns with interrupts off again, whatever it was given.
option(UROS_ABLATE_612_BURNIN_CLI "Build the #612 ablation: the clock burn-in ends with interrupts off whatever its caller had (entries 3, 4, 20: the line after the burn-in must say WRONG, and at more than one processor the boot stops in setup_main)" OFF)
# #612, x86_64: the boot self-tests that map in the VM's range leave their pages there again.
option(UROS_ABLATE_612_LEAVE_MAPPED "Build the #612 ablation: map_selftest and tlb_shootdown_selftest leave their pages in the VM's range (every x86-64 boot: pmap_virtual_space() must say WRONG and name them)" OFF)
# #608, both targets: the RCU drain thread is not started, so only the idle loop runs callbacks.
option(UROS_ABLATE_608_NO_DRAIN_THREAD "Build the #608 ablation: no RCU drain thread, only the idle loop runs callbacks (x86-64 -R: with every processor busy the queue must grow and the bench say WRONG)" OFF)
# #526, both targets: the idle loop restores the level it found again, instead of waiting at zero.
option(UROS_ABLATE_526_IDLE_RESTORES "Build the #526 ablation: the idle loop restores the level it found (a raised one is held for ever; entry 22 under TCG at 1.4 GHz stalls about once in 30 boots)" OFF)
# #617, x86_64: a TEST option, not an ablation -- the window in thread_return_ast() widened, so
# that the test can show what happens inside it with the fix and without.
option(UROS_WIDEN_617_WINDOW "Build for #617's test: a thread making its first return to ring 3 spins for 50 us between reading its processor's number and reading that processor's AST word (with the window open, pthread_test [31] loses its embryo threads to ring 3)" OFF)
# #617, x86_64: thread_return_ast() takes its last look with interrupts on again.
option(UROS_ABLATE_617_RETURN_AST "Build the #617 ablation: thread_return_ast() checks need_ast with interrupts on, so a thread preempted between reading its processor's number and that processor's AST word reads another's (with UROS_WIDEN_617_WINDOW, pthread_test [31] must lose embryos to ring 3 and the return check say WRONG)" OFF)
# #526, x86_64: splx() finds the per-CPU block before it turns interrupts off again.
option(UROS_ABLATE_526_SPLX_WINDOW "Build the #526 ablation: splx() finds this processor's block with interrupts still on, so a thread preempted at level zero finishes its raise in the block of the processor it left (x86-64 -E, GRUB entry 30: must say WRONG)" OFF)
# #638, x86_64: a TEST option, not an ablation -- the shootdown's two windows widened, so that
# the test can show what a thread moved inside them does, with the fixes and without.
option(UROS_WIDEN_638_WINDOW "Build for #638's test: while -J runs, a shootdown spins for 50 us between deciding which processor it is on and acting on it (a thread moved inside the spin: -J arm [1] must find a stale translation, arm [2] a cross-call to its own sender)" OFF)
# #639, x86_64: the application processors keep the control registers the trampoline left them again.
option(UROS_ABLATE_639_AP_CONTROL "Build the #639 ablation: the application processors keep CD and NW set and run without CR0.WP, SMEP and SMAP, as every boot did before (the boot self-test must name them on every application processor, in the census and in the probes)" OFF)
# #638, x86_64: tlb_flush_range() flushes this processor and cross-calls the others with preemption on again.
option(UROS_ABLATE_638_FLUSH_UNPINNED "Build the #638 ablation: tlb_flush_range() flushes this processor and cross-calls the others with preemption on, so a thread moved between the two leaves the processor it arrived at unflushed (with UROS_WIDEN_638_WINDOW, -J arm [1] must say WRONG)" OFF)
# #638, x86_64: ipi_call_mask() strikes its own bit with preemption on again.
option(UROS_ABLATE_638_STRIKE_UNPINNED "Build the #638 ablation: ipi_call_mask() strikes this processor's bit with preemption on, so a thread moved before it sends sends to itself (with UROS_WIDEN_638_WINDOW, -J arm [2] must end in the panic that names it)" OFF)
# 🔴 How often the #561 vector-state exemption fires.  OFF, and off is the
# shipping answer: counting it is three increments on EVERY context switch, for
# ever, to learn something that is learned once -- on a path whose saving is
# about 114 ns, so the instrument can outweigh its subject.  It was on while
# #561 was written, it found that the first version was a no-op, and it came
# off.  What stays on is free: the line kern/startup.c prints once and the
# `fpu=` on each thread in quiet_census.
option(UROS_CONTEXT_FPU_COUNT "Count how often the #561 vector-state exemption fires" OFF)
# 🔑 WHICH trap the profile follows (#554).  It filters on one slot of the table
# in kern/syscall_sw.c: 33 is urmach_msg, #392's subject; 42 is urmach_futex,
# #554's.  A cache variable and not a boot argument because the trap under study
# is a property of the EXPERIMENT, and this build already carries the switch that
# compiles the instrument in -- a run whose subject is not visible in its
# configuration is a run whose log cannot be re-read a month later.
set(UROS_SYSCALL_PROFILE_TRAP 33
    CACHE STRING "Which Mach trap the phase profile follows (33=urmach_msg, 42=urmach_futex)")

# #385 layer four: poison a page on its way into the free list and verify it on
# the way out.  Forensic, not an invariant: it fingerprints whoever wrote a page
# while it was free.  #482 measured the read side at 44% of a copy-on-write
# fault, so it no longer rides on MACH_ASSERT -- which is ON in Release.
# Same both-arms rule as above: declared here, appended in each arm.
option(UROS_VM_PAGE_POISON "Build the #385 free-page poison (expensive: two page traversals per recycle)" OFF)

# #485: the global list of every allocated port, and the global mutex that
# maintains it on each port alloc and free.  It buys the debugger's port
# enumeration; it was filed under MACH_ASSERT, which is ON in Release.
# Same both-arms rule as above.
option(UROS_IPC_PORT_TRACK "Build the port allocation tracking (a global mutex per port alloc/free)" OFF)

# #486: count, per processor, how many simple locks are held when the MP
# inter-processor interrupt lands, and from which acquisition site.  It is the
# observable ipi_mp_handler()'s sched_safe guard needs and does not have -- on
# i386 get_preemption_level() is the constant 0, so that guard folds away.
# A measuring build: the hooks sit on every simple lock in the kernel.
# Same both-arms rule as above.
option(UROS_USLOCK_CENSUS "Build the #486 simple-lock census (hooks on every simple lock)" OFF)

# #558: take the per-page pv lock away again, leaving the walks in pmap.c
# running with nothing held, as they were before the lock existed.  The
# general protection fault that lock was written for has never been
# reproduced, and a fix is verified by REMOVING it: this is the arm that has
# to show the fault coming back.  The free list keeps its lock in both arms,
# so the ablated arm is about the WALK.  pv_bootstrap() names the arm in the
# boot log.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_558_PVLOCK "Build the #558 ablation: pv walks with no per-page lock" OFF)
# The other half of the same ablation, and they are separate because the two
# races have different symptoms: an unlocked FREE LIST hands two processors the
# same entry, so one page's list grafts onto another's and a walk reaches a
# pv->pmap belonging elsewhere -- a wild pointer, which is #558's fault; an
# absent PER-PAGE lock lets a walk miss an entry, so a mapping is never torn
# down -- which is #531's panic.  With both off, 18 boots gave six of the
# second and none of the first, and could not say which race they were.
option(UROS_ABLATE_558_PVFREELOCK "Build the #558 ablation: pv free list with no lock" OFF)
# #563: do not open the copy's window before the probe that tests whether a
# trap inherits it.  One switch, two outcomes, and that is the point: on a
# processor WITH SMAP the self-test must go WRONG, because there the window is
# the thing under test; on one WITHOUT it the boot must stay green and keep
# saying the processor has no SMAP, because there nothing changed.  A third
# answer that could not be told from the other two by taking it away would not
# be worth having.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_563_WINDOW "Build the #563 ablation: the copy's window is never opened" OFF)
# #551: put the unbounded UART spin back.  A bound nothing has ever hit is
# untested code; scripts/uart-stall.py stops the transmitter on purpose, and
# this is the kernel that has to be seen NOT coming back for the bounded one's
# coming back to mean anything.  Never ON in a kernel anyone boots for anything
# else.
option(UROS_ABLATE_551_UNBOUNDED "Build the #551 ablation: cons_putc waits for the transmitter for ever" OFF)
# #566: remove the grace period from pmap_destroy, to find out whether it is
# what the trap sweep pays a clock tick for on more than one processor.  UNSAFE
# -- a reader may still hold the pmap.  Measurement only.
option(UROS_ABLATE_566_NO_GRACE "Build the #566 ablation: pmap_destroy does not wait a grace period" OFF)
# #566: and the other wait a processor can spend a trap inside -- the TLB
# shootdown standing still until every other processor has acknowledged.
# UNSAFE: the caller goes on with stale translations elsewhere.
option(UROS_ABLATE_566_NO_IPI_WAIT "Build the #566 ablation: ipi_call_others does not wait for the acknowledgements" OFF)
# #567: one byte per look at the transmitter, which is what this tree did
# before the FIFO was used.  The other arm of that measurement.
option(UROS_ABLATE_567_NO_FIFO "Build the #567 ablation: one byte per look at the transmitter" OFF)
# #567: refuse to arm the console ring, so every byte goes to the port in the
# caller's thread under printf_lock, which is the console this target had
# before.  The other arm of the hold measurement, in a binary that differs in
# nothing else.
option(UROS_ABLATE_567_NO_RING "Build the #567 ablation: the console writes the wire in the caller's thread" OFF)
# #570: make the device master REFUSE the holder of a PCI I/O BAR its own
# registers, after N allowed accesses, so that a driver's refusal path is asked
# on a real boot instead of argued.  0 refuses the probe; a larger N lets the
# probe through and refuses a request.  Empty = off.  Never set in a kernel
# anyone boots for anything else.
set(UROS_ABLATE_570_REFUSE_IOBAR "" CACHE STRING "Build the #570 ablation: refuse a PCI I/O BAR to its holder after N accesses (empty = off)")
# #577: make the device master REFUSE exactly one configuration access -- the
# NTH read (0) or write (1) of register REG of device BDF that comes through the
# device_pci_config_read/write RPCs -- so that each step a caller has to check
# is refused on a real boot.  "BDF,WRITE,REG,NTH", e.g. "0x28,1,0x04,2" for the
# second write of 00:05.0's command register.  A fifth field "zero" answers
# that READ with 0 instead of refusing it: a device that decodes nothing there,
# which is how a region that MEASURES zero is made on demand.  Empty = off.
# Never set in a kernel anyone boots for anything else.
#
# 🔴 Parsed HERE, above the branch, and appended in BOTH arms (see the note on
# UROS_FAULT_PROFILE above): the code it builds is machine-independent, and
# pci_scan and ahci run on i386 too.  ⚠️ The same BDF is a different device on
# each board -- 0x20 is virtio-blk on x86-64's and ahci on i386's.
set(UROS_ABLATE_577_REFUSE_CFG "" CACHE STRING "Build the #577 ablation: refuse one config access, BDF,WRITE,REG,NTH[,zero] (empty = off)")
set(UROS_ABLATE_577_DEFS "")
if(NOT UROS_ABLATE_577_REFUSE_CFG STREQUAL "")
    string(REPLACE "," ";" _a577 "${UROS_ABLATE_577_REFUSE_CFG}")
    list(LENGTH _a577 _a577n)
    if(NOT (_a577n EQUAL 4 OR _a577n EQUAL 5))
        message(FATAL_ERROR "UROS_ABLATE_577_REFUSE_CFG is BDF,WRITE,REG,NTH[,zero] -- got '${UROS_ABLATE_577_REFUSE_CFG}'")
    endif()
    list(GET _a577 0 _a577bdf)
    list(GET _a577 1 _a577w)
    list(GET _a577 2 _a577reg)
    list(GET _a577 3 _a577nth)
    set(UROS_ABLATE_577_DEFS ABLATE_577_BDF=${_a577bdf} ABLATE_577_WRITE=${_a577w}
                             ABLATE_577_REG=${_a577reg} ABLATE_577_NTH=${_a577nth})
    set(_a577what "refused")
    if(_a577n EQUAL 5)
        list(GET _a577 4 _a577mode)
        if(NOT _a577mode STREQUAL "zero" OR NOT _a577w EQUAL 0)
            message(FATAL_ERROR "UROS_ABLATE_577_REFUSE_CFG: the fifth field is 'zero', and only on a read -- got '${UROS_ABLATE_577_REFUSE_CFG}'")
        endif()
        list(APPEND UROS_ABLATE_577_DEFS ABLATE_577_ZERO=1)
        set(_a577what "answered with 0")
    endif()
    message(STATUS "#577 ablation: config access ${_a577nth} (write=${_a577w}) of register ${_a577reg} on BDF ${_a577bdf} is ${_a577what} (${UROS_TARGET_ARCH})")
endif()
# #563: make the five tests that need a second processor believe they are alone,
# so the line that DECLINES the question can be executed.  There is no boot flag
# capping the processor count on this target and an AP cannot be made to fail to
# arrive on demand, so without this the correction would ship unexercised --
# which is the objection #561 was built on.  Never ON in a kernel anyone boots
# for anything else.
option(UROS_ABLATE_563_ALONE "Build the #563 ablation: every test believes it is alone" OFF)
# #586: make tsc_calibrate() decline, the way it does on its own about once in
# 280 boots under TCG -- the first of its two runs reads high and they disagree.
# Every consumer of tsc_hz() then has to answer a missing rate, and they are all
# asked at once, which waiting for the natural refusal would take hundreds of
# boots to do.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_586_TSC_DISAGREE "Build the #586 ablation: no two of the TSC calibration's runs agree, so it declines" OFF)
# #586: the tick self-test's handler acknowledges each tick and counts none of
# them, so the period line meets a calibrated TSC with no gap to measure -- the
# branch that must stay WRONG, which the calibration ablation cannot reach.
# Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_586_NO_TICKS "Build the #586 ablation: the tick self-test counts no ticks" OFF)
# #508: the kernel keeps none of its rulers' ports, so a claim takes the 8254,
# its gate and the PM timer -- and io_claim_race's arm [5] and the boot's
# "legacy ports the kernel keeps" line must both say WRONG.  Never ON in a
# kernel anyone boots for anything else.
# #597: i386's two-port PCI configuration access without its lock, as it was
# before #597, and/or with its window widened by N reads of port 0x80 between
# the address and the data (each one a VM exit under KVM).  NO_LOCK alone is
# the old kernel; NO_LOCK with WIDEN makes the race frequent at -smp 4, and
# WIDEN alone shows the lock closing even a wide window.  i386 only: x86-64
# has had its own lock since 1b4c138d.  Never ON in a kernel anyone boots for
# anything else.
option(UROS_ABLATE_597_NO_LOCK "Build the #597 ablation: i386's PCI configuration access takes no lock" OFF)
set(UROS_ABLATE_597_WIDEN "" CACHE STRING "Build the #597 ablation: N port 0x80 reads between the PCI address and data (empty = off)")
# #599: stop processor 0's tick on purpose, 3000 ticks into the boot, to show
# that the clock watch (i386/clock_watch.c) sees it and what the NMI's dump
# says.  MASK_PIT masks the 8254's pin with nothing pending to unmask it, the
# shape a lost I/O APIC read-modify-write leaves; CLI_SPIN takes the timeout
# list's lock and spins processor 0 with interrupts off; SPIN_ONLY keeps the
# watch off the tick, so that with CLI_SPIN only a spin can say so; SPL_LEAK
# has a thread_switch on processor 0 return to user mode at splsched, the
# second hypothesis, which return_to_user's check must say.  i386 with
# UROS_NCPUS > 1 only: the watch runs on the other processors.  Never ON in a
# kernel anyone boots for anything else.
option(UROS_ABLATE_599_MASK_PIT "Build the #599 ablation: processor 0 masks the 8254's pin once, with nothing pending" OFF)
option(UROS_ABLATE_599_CLI_SPIN "Build the #599 ablation: processor 0 takes the timeout list's lock and spins with interrupts off once" OFF)
option(UROS_ABLATE_599_SPIN_ONLY "Build the #599 ablation: the clock watch runs only from inside spins, not from the tick" OFF)
option(UROS_ABLATE_599_SPL_LEAK "Build the #599 ablation: a thread_switch on processor 0 returns to user mode at splsched once" OFF)
# #599: the I/O APIC's select/window pair widened on the other processors by
# N port 0x80 reads in each of its three gaps (i386/ioapic.c).  If the pair's
# race is what stops processor 0's tick, this makes it frequent at four
# processors and absent at one.  i386 SMP only; never ON otherwise.
option(UROS_ABLATE_599_NESTED_PRINTF "Build the #599 ablation: processor 0 raises an unhandled line inside consolewrite's printf_lock hold once" OFF)
option(UROS_ABLATE_599_PRINTF_WAITS "Build the #599 ablation: a printf nested inside printf_lock's holder waits for it, as before the fix" OFF)
option(UROS_ABLATE_599_NO_LOCK "Build the #599 ablation: i386's I/O APIC select/window pair takes no lock, as before the fix" OFF)
option(UROS_ABLATE_599_I8042_SHARED "Build the #599 ablation: i386's break-key reader keeps reading the 8042 while a task holds its ports" OFF)
option(UROS_ABLATE_599_COMPROBE_OLD_EXIT "Build the #599 ablation: i386's comprobe/comattach clobber unit 0 on boots without -r, as before the fix" OFF)
option(UROS_ABLATE_599_COM_NO_LOCK "Build the #599 ablation: i386's com_bank_lock is not taken -- THR writes and the divisor sequence race, as before the fix" OFF)
set(UROS_ABLATE_599_WIDEN_DIVISOR "" CACHE STRING "Build the #599 ablation: N port 0x80 reads between DLL and DLM in i386's divisor sequence (empty = off)")
set(UROS_ABLATE_599_WIDEN "" CACHE STRING "Build the #599 ablation: N port 0x80 reads in each gap of the I/O APIC pair, other processors only (empty = off)")
# #599: x86-64's I/O APIC window (x86_64/cpu/ioapic.c, the lock #593 put
# there), for the -Y test that races it from two processors.  WINDOW_OPEN uses
# the window without the lock -- interrupts are still masked, so what is left
# is the race between processors -- and WINDOW_WIDEN puts N port 0x80 reads
# between selecting a register and using it.  x86-64 only: i386's pair is
# UROS_ABLATE_599_NO_LOCK and _WIDEN above.  Never ON in a kernel anyone boots
# for anything else.
option(UROS_ABLATE_599_WINDOW_OPEN "Build the #599 ablation: x86-64's I/O APIC window is used without its lock" OFF)
set(UROS_ABLATE_599_WINDOW_WIDEN "" CACHE STRING "Build the #599 ablation: N port 0x80 reads between selecting an x86-64 I/O APIC register and using it (empty = off)")
# #599: x86-64's halt_cpu in the order it had before the review rounds, for
# the -Z test: the console's final copy first, the halt broadcast before the
# wait for the panicking processor's message, interrupts left as they were.
# x86-64 only.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_599_HALT_ORDER "Build the #599 ablation: x86-64's halt_cpu in its old order -- the copy first, the stop before the panic's message ends" OFF)
option(UROS_ABLATE_508_LEND_RULERS "Build the #508 ablation: the kernel keeps none of its rulers' ports" OFF)
# #508: an MMIO mapping is checked on its first page only, as it was before the
# span was bounded, so irq_claim_test's arm [10] -- one page past the owned
# card's region -- must say WRONG.  Never ON in a kernel anyone boots for
# anything else.
# #508: one of the TSC calibration's three runs reads one part in thirty-two
# high, the shape of every refusal the old calibration took on its own, so the
# median can be seen setting it aside.  Never ON in a kernel anyone boots for
# anything else.
option(UROS_ABLATE_508_ONE_RUN_OUT "Build the #508 ablation: one TSC calibration run reads 1/32 high" OFF)
# #508: the end of the first run of every calibration attempt is read ~3 ms
# late, so its bracket is wider than the tolerance and the run must be set
# aside before the vote -- for the TSC and the LAPIC timer alike, since they
# share the rule.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_508_END_DELAYED "Build the #508 ablation: one calibration run's end is read ~3 ms late" OFF)
# #508: the PM timer's rate is taken 1/32 high, a ruler that lies, so the vote
# must name it and nothing measure against it.  Never ON otherwise.
option(UROS_ABLATE_508_RULER_LIES "Build the #508 ablation: the PM timer's rate is taken 1/32 high" OFF)
# #508: the ACPI HPET table is withheld, a board without an HPET, so two
# rulers vote and the refinement uses the PM timer.  Never ON otherwise.
option(UROS_ABLATE_508_NO_HPET "Build the #508 ablation: the ACPI HPET table is withheld" OFF)
# #508: no exact source is seen, so the measured path runs on a machine that
# has one.  Never ON otherwise.
option(UROS_ABLATE_508_EXACT_HIDDEN "Build the #508 ablation: the exact TSC sources are not seen" OFF)
# #508: the first exact source that states a rate states it a quarter high, so
# the rulers contradict it and it is WRONG -- a quarter because under a
# hypervisor a contradiction has to exceed the host's NTP slew, 12.5% (#594).
# Never ON otherwise.
option(UROS_ABLATE_508_EXACT_LIES "Build the #508 ablation: the first exact TSC source states a quarter high" OFF)
# #508: the processor states CPUID 0x15 as OMEGA's does, so the path that
# adopts it -- which no guest can reach -- runs.  Never ON otherwise.
option(UROS_ABLATE_508_CPUID_15 "Build the #508 ablation: CPUID 0x15 states OMEGA's crystal and ratio" OFF)
option(UROS_ABLATE_508_SPAN_UNBOUNDED "Build the #508 ablation: an MMIO mapping is checked on its first page only" OFF)
# #594: CPUID 0x15 states the crystal as the timing leaf's bus clock and the
# TSC's ratio to it, as a hypervisor following the SDM would -- no QEMU does --
# so the LAPIC timer adopts 0x15's crystal in a guest.  Boot it with +invtsc.
# Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_594_CRYSTAL_IS_BUS "Build the #594 ablation: CPUID 0x15 states the timing leaf's bus as the crystal" OFF)
# #594: from the watchdog's second window, the TSC as it reads it runs 1/32
# fast, so both rulers disagree with it: the watchdog must name the TSC, move
# the tick to the local APIC timer and withdraw the TSC's rate -- and the boot
# must go on.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_594_TSC_SKEWS "Build the #594 ablation: the TSC runs 1/32 fast as the watchdog reads it" OFF)
# #594: from the watchdog's second window, the HPET as it reads it stops, so the
# watchdog must name the HPET and go on with the PM timer.  Never ON otherwise.
option(UROS_ABLATE_594_HPET_STOPS "Build the #594 ablation: the HPET stops as the watchdog reads it" OFF)
# #594: the rulers carry a host's NTP slew -- in the watchdog, 0.4 s offsets
# drained at an eighth a second, the sign alternating every 32 windows; at
# boot, the rulers' answer 5% low -- which is what a host converging after a
# reboot does to a guest.  Nothing may be named, and the exact source must
# still be adopted.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_594_NTP_SLEW "Build the #594 ablation: the rulers carry a host's NTP phase slew" OFF)
# #593: icr_send() leaves the interrupt command register open to an interrupt
# between its wait and its second write, as it did before #593, and counts the
# sends that began on a processor already between the two halves of another.
# Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_593_ICR_OPEN "Build the #593 ablation: an IPI send may be interrupted between its two register writes" OFF)
# #593: NO_APIC_TIMER makes the local APIC's timer unusable in either mode, so
# the tick must run on the HPET's broadcast and the boot lines must say so.
# BEHIND lands every 128th HPET comparator write after the counter has passed
# the deadline it carries; UNCHECKED takes away the read-back that notices.
# BEHIND alone must cost late ticks and a count; with UNCHECKED, every
# processor's clock stops until the counter's low half wraps.  Never ON in a
# kernel anyone boots for anything else.
option(UROS_ABLATE_593_NO_APIC_TIMER "Build the #593 ablation: the local APIC timer is unusable in both modes" OFF)
# #593: only the APIC timer's countdown is unusable; with UROS_ABLATE_594_TSC_SKEWS
# the watchdog names the TSC, and the tick must move from the TSC deadline to the
# HPET while the system runs.  Never ON in a kernel anyone boots otherwise.
option(UROS_ABLATE_593_NO_COUNTDOWN "Build the #593 ablation: only the local APIC timer's countdown is unusable" OFF)
# #593: ISA 8's input unmasked before the HPET chooses its route, as a driver's
# claim would leave it: LegacyReplacement must be refused, and with
# UROS_ABLATE_594_TSC_SKEWS and UROS_ABLATE_593_NO_COUNTDOWN the tick must stay
# on the named TSC and say so.  Never ON in a kernel anyone boots otherwise.
option(UROS_ABLATE_593_LINE_TAKEN "Build the #593 ablation: the RTC's input is in use when the HPET chooses its route" OFF)
option(UROS_ABLATE_593_BEHIND "Build the #593 ablation: every 128th HPET comparator write lands behind the counter" OFF)
option(UROS_ABLATE_593_UNCHECKED "Build the #593 ablation: an HPET comparator write is not read back" OFF)
# #593: the local APIC timer's two modes count each deadline from now again, as
# before #593, so the tick runs slow by its own delivery time and the kernel's
# time of day with it.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_593_FROM_NOW "Build the #593 ablation: the APIC timer's deadlines are counted from now, not from the last one" OFF)
# #593: the HPET block as a firmware that drove its own tick with it would
# leave it -- LegacyReplacement on, the last comparator interrupting -- so that
# the boot line saying what was found and silenced is printed at all.
option(UROS_ABLATE_593_FIRMWARE_LEFT "Build the #593 ablation: the firmware left the HPET's LegacyReplacement on and a comparator interrupting" OFF)
# #593: the last processor to leave the HPET's broadcast leaves the comparator
# interrupting, so that the line read back after the move says WRONG.
option(UROS_ABLATE_593_LEFT_ON "Build the #593 ablation: the HPET's comparator is left interrupting when the tick leaves it" OFF)
# #593: the HPET's counter is named before the clock is chosen, so that its
# probe must refuse it.
option(UROS_ABLATE_593_HPET_NAMED "Build the #593 ablation: the HPET's counter is named before the clock is chosen" OFF)
# #593: processor 1's tick is never armed, so that the per-processor window
# lines must say a processor never ticked.
option(UROS_ABLATE_593_NEVER_TICKS "Build the #593 ablation: processor 1's tick is never armed" OFF)
# #603: the ways ASTs reach a return to ring 3, each with a way back out.
option(UROS_ABLATE_603_IPI_EMPTY "Build the #603 ablation: the AST interrupt is an EOI and does not run ast_check()" OFF)
option(UROS_ABLATE_603_ONE_PASS "Build the #603 ablation: a return to ring 3 takes ASTs once, not until none is pending" OFF)
option(UROS_ABLATE_603_FAULT_AST "Build the #603 ablation: a page fault resolved for ring 3 returns without taking ASTs" OFF)
# #604: the x86-64 pmap's rules, each with a way back out.
option(UROS_ABLATE_604_UPDATE_GONE "Build the #604 ablation: pmap_pte_update() writes into an entry that is gone" OFF)
option(UROS_ABLATE_604_FORGET_TWO_WALKS "Build the #604 ablation: pmap_forget() resolves outside a read section, then removes in a second walk" OFF)
option(UROS_ABLATE_604_UNMAP_STORE "Build the #604 ablation: the unmap reads the entry and stores zero, and two removers can both get through" OFF)
option(UROS_ABLATE_604_SPLIT_DROPS "Build the #604 ablation: a split keeps only the permission bits, and its interior entry has no USER" OFF)
option(UROS_ABLATE_604_COLLECT_MASKED "Build the #604 probe: the -M bench collects once with interrupts off, and the check in collect_flush() must stop the boot" OFF)
# #606: what a page's mappings saw, kept when they go.
option(UROS_ABLATE_606_FORGET_BITS "Build the #606 ablation: a removed or replaced entry keeps nothing of its ACCESSED and DIRTY bits" OFF)
option(UROS_PROBE_606_PAGEOUT "Build the #606 probe: once a second, the counters vm_pageout_scan() keeps, printed when they move" OFF)
option(UROS_ABLATE_606_PAGEOUT_WAIT "Build the #606 ablation: vm_pageout() asserts nobody is waiting after a scan, and sleeps with no end" OFF)
# #605: the cross-call that times out names who did not answer.
option(UROS_PROBE_605_SILENT "Build the #605 probe: halt one processor and cross-call it, so the panic has to name it" OFF)
option(UROS_PROBE_605_CALL_COST "Build the #605 probe: time broadcast cross-calls at boot" OFF)
option(UROS_PROBE_605_PADDED "Build the #605 probe: one cache line per answer-counter slot, to measure what sharing one costs" OFF)
# #607: the futex hand-off wakes a waiter the way every other wakeup does.
option(UROS_ABLATE_607_SWAPPED_PARKED "Build the #607 ablation: a swapped-out waiter counts as parked and the hand-off switches onto it, which -Q's first arm must catch" OFF)
option(UROS_ABLATE_607_OLD_WAKE "Build the #607 ablation: the hand-off's old wake, which leaves a stopped waiter TH_RUN on no run queue, which -Q's second arm must catch" OFF)
option(UROS_ABLATE_607_DTS_SWAPPED "Build the #607 ablation: the IPC direct thread switch switches onto a swapped-out receiver, which the -ON storm must catch" OFF)

# #598: interrupt remapping, each check with a way to see it catch.
option(UROS_ABLATE_598_INTCTL_FORWARD "Build the #598 ablation: an AMD device table entry forwards interrupts unmapped beside a valid table, and the interrupt check must count the entry as wrong" OFF)
option(UROS_ABLATE_598_IIDX_HIGH "Build the #598 ablation: an interrupt entry cache invalidation puts its index at 63:48, where a fault record keeps one, and the interrupt check must count the two index-selective cases as wrong" OFF)
option(UROS_ABLATE_598_QI_TAIL_UNSHIFTED "Build the #598 ablation: the invalidation queue's tail is written as an index and not in bits 18:4, and the fabricated queue check must count one wrong while a real Intel engine never sees its descriptors" OFF)
option(UROS_ABLATE_598_QI_FLUSH_BY_REGISTER "Build the #598 ablation: a flush goes through the IOTLB register although the invalidation queue is on, and entry 16's grants must fail on an Intel engine" OFF)
# #594, i386 only: the two halves of closing i386's other doors to the 8254.
# REOPEN puts 0x40, 0x42 and 0x61 back in the iopl device's port list, so
# i386_io_port_add() must refuse the device whole and kernel242_test's control
# port must fault.  UNCHECKED stops i386_io_port_add() and the iopl #GP
# emulation asking device_md_io_reserved(), and brings back the emulated
# channel-2 write to 0x43, so the emulated read of 0x40 must succeed.  Both
# together are the kernel before #594: the bitmap grants the 8254.  Never ON
# in a kernel anyone boots for anything else.
option(UROS_ABLATE_594_IOPL_REOPEN "Build the #594 ablation (i386): the iopl device lists the 8254 and its gate again" OFF)
option(UROS_ABLATE_594_UNCHECKED "Build the #594 ablation (i386): the bitmap and the iopl emulation do not ask device_md_io_reserved" OFF)
# #563: withhold the MADT's I/O APIC entries while leaving the processor census
# alone, so the tests that route through one see the board a firmware without an
# I/O APIC describes -- and the discriminator they use, the other half of the
# same table, still answers.  qemu offers no machine that gives one without the
# other.  Never ON in a kernel anyone boots for anything else.
option(UROS_ABLATE_563_NO_IOAPIC "Build the #563 ablation: the MADT lists no I/O APIC" OFF)

if(UROS_TARGET_ARCH STREQUAL "x86_64")
  set(X86_64_BOOT_LD ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot.ld)

  add_executable(mach_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/ap_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot_c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot_modules.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/bootarg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/printf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/klog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/cpu_data.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/lib/bitmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/lib/copy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/lib/string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/multiboot2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/acpi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/cons.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/cons_cost.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/fbcons.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ddb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/cont_probe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/disasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ksym.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/desc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/idle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ipi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ioapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ioapic_race_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/lapic.c
    # Configuration space: ECAM where the MCFG describes one, the port
    # pair where it does not -- two boards, not a bridge (#457).
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/pci_cfg.c
    # What polices DMA, if anything: one vendor-neutral description and one
    # reader per vendor, so the second is a backend and not a rewrite (#432).
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/iommu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/iommu_fault.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/iommu_vtd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/iommu_amd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/conf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/console_cdev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/machdep.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/halt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/percpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/pic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/startup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/spl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/spl_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/smp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/bootmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/collect_bench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/rcu_bench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/shootdown_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/direct.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/phys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/pmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/pv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/tlb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/vminit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/walk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/probe.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/syscall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/clock_dev.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/exact.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/freq_source.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/hpet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/hpet_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/pmtimer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/ruler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/rulers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/tsc_refine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/tsc_watch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/pit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/tsc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/clock_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/preempt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/rcu_tick_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/context.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/context.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/fpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/fpu_stress.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/fpu_stress.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/trap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/ast_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/wait_preempt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/handoff_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/swap_disable_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/entry_test.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/entry_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/sync/lock.c)
  # sync/mutex.c, thread/machine.c and kern/lock.c are not in that list, and
  # the boundary is worth stating exactly rather than as "not yet" (#453).
  #
  # ── Measured 2026-08-02, and the shape of it changed ─────────────────
  #
  # thread/machine.c is the seventeen-name thread and activation surface the
  # machine-independent tree calls.  It compiles.  Adding it to this list
  # leaves FIVE undefined symbols -- Assert, printf, kalloc, kfree,
  # usimple_lock_init -- from four files that also compile: kern/debug.c,
  # kern/printf.c, kern/kalloc.c, kern/lock.c.
  #
  # Adding those four leaves TWENTY-EIGHT, and they are a different kind of
  # thing: assert_wait, thread_block, thread_wakeup_prim, kmem_alloc,
  # kmem_suballoc, zinit, zalloc, kernel_map, page_size.  That is the
  # scheduler and the virtual memory system, together, and neither closes
  # without the other.
  #
  # ⚠️ The generated MIG user stubs are not a way round this, and it is worth
  # recording that they were tried.  memory_object_user.c and its four
  # neighbours have been generated since #416 and compiled by nobody, and on
  # paper they define 22 of the missing symbols for no new code.  Compiling
  # them leaves three others -- mach_msg_rpc_from_kernel and
  # mach_msg_send_from_kernel from kern/ipc_mig.c, and NDR_record, which is
  # instantiated by whichever translation unit includes <machine/ndr_def.h>
  # and on i386 is ipc/ipc_init.c.
  #
  # Three symbols, and behind them the IPC subsystem.  The offline symbol
  # closure had said the stubs were free; the linker says they are not, and
  # the linker is the one that decides.  They come in with the bring-up.
  #
  # So the door is one file wide and the room behind it is the rest of the
  # kernel.  That is why #453's rule is that the kernel links at the end of
  # every commit: the next step is not another name, it is the bring-up, and
  # it wants to be attempted as one deliberate thing rather than discovered
  # halfway through a commit about thread state.
  #
  # Both COMPILE.  kern/lock.c was one missing header away and #428 supplied
  # it; the mutex has been compiling since it was written.  What stops them
  # is the LINK, and the list is short and known:
  #
  #   assert_wait, thread_block, thread_wakeup_prim,
  #   thread_will_wait_with_timeout        kern/sched_prim.c   (compiles)
  #   Assert, db_printf, db_error          kern/debug.c        (compiles)
  #   cpu_data, interlock_unlock           kern/ipc_kobject.c  (does not:
  #                                        wants device_pager_server.h, which
  #                                        i386 generates from
  #                                        memory_object.defs with a bespoke
  #                                        mig rule this target has not got)
  #   memset                               no MI provider yet
  #
  # Ten symbols, three files. That is a different kind of blocker from the
  # ones this session cleared: not a header anybody can write, but the point
  # where the machine-independent kernel starts pulling in the scheduler --
  # and the scheduler pulls in more. It is the beginning of bring-up rather
  # than the end of header work, and it wants its own issue.
  # <mach/...> and the per-target <mach/machine/...> behind it (#413).
  #
  # The first thing this target has ever taken from outside x86_64/, and it
  # is deliberately the interface headers rather than any of the code that
  # uses them: the message layout is a decision, and a decision is worth
  # nothing until the target that has to live with it compiles the header
  # and checks it.  The MI tree follows later and follows this.
  #
  # ⚠️ Ordered as the i386 target orders them — uapi, then the arch links —
  # so <mach/machine/vm_types.h> resolves to mach/x86_64/ here for exactly
  # the same reason it resolves to mach/i386/ there.
  set(KERNEL_DEFINES_BARE
      KERNEL MACH_KERNEL KERNEL_PRIVATE KERNEL_SERVER=1
      # ⚠️ MACH_ASSERT is NOT here (#485).  It comes from the knob table in
      # cmake/kernel-config.cmake, through <mach_assert.h>, driven by
      # -DUROS_MACH_ASSERT.  Repeating it here at the same value overrode
      # nothing and made the switch unsettable from either place.
      NCPUS=64 MACH_DEBUG=1 MACH_KDB=0 MACH_DEBUGGER=1 MACH_HOST=1
      STAT_TIME TASK_SWAPPER=1 __NO_UNDERSCORES__
      # This machine supplies its own usimple_lock (x86_64/sync/lock.c), so
      # kern/lock.c leaves them out.  See the guard there: the rest of that
      # file is the mutex slow path and the read/write locks, which reach
      # into the scheduler (#453).
      USIMPLE_LOCK_CALLS=1
      # This machine supplies the preemption level, without asking for the
      # whole MACH_RT configuration (#461).  <kern/cpu_data.h> otherwise
      # defines disable_preemption() as nothing and get_preemption_level() as
      # the constant 0 -- and since #459 this target preempts on the way out
      # of every trap, so the spans that say "not here" have to be able to
      # mean it.  See x86_64/cpu_data.h.
      MACHINE_PREEMPTION_LEVEL=1
      # Zero, and said rather than left undefined: this kernel is built
      # -mgeneral-regs-only so that entry from user mode never has to save
      # vector state, which means no floating point anywhere in it (#453).
      KERNEL_FLOAT_OK=0)
  if(UROS_CONTEXT_FPU_COUNT)
      list(APPEND KERNEL_DEFINES_BARE CONTEXT_FPU_COUNT=1)
      message(STATUS "#561 vector-state exemption counters: ON (x86_64)")
  endif()
  if(UROS_SYSCALL_PROFILE)
      list(APPEND KERNEL_DEFINES_BARE SYSCALL_PROFILE=1
           SYSCALL_PROFILE_TRAP=${UROS_SYSCALL_PROFILE_TRAP})
      message(STATUS "#411 trap phase profile: ON, following trap "
                     "${UROS_SYSCALL_PROFILE_TRAP} (x86_64)")
  endif()
  if(UROS_FAULT_PROFILE)
      list(APPEND KERNEL_DEFINES_BARE FAULT_PROFILE=1)
      message(STATUS "#482 copy-on-write fault phase profile: ON (x86_64)")
  endif()
  if(UROS_ABLATE_537_HMAC_FULL)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_537_HMAC_FULL=1)
      message(STATUS "#537 ablation: the HMAC key's blocks hashed on every check (x86_64)")
  endif()
  foreach(_a MAC_EVERY_TIME EPOCH_KEPT PAGE_WALK IX_UNSORTED HMAC_LOCKED)
      if(UROS_ABLATE_537_${_a})
          list(APPEND KERNEL_DEFINES_BARE ABLATE_537_${_a}=1)
          message(STATUS "#537 ablation: ${_a} (x86_64)")
      endif()
  endforeach()
  if(UROS_PROBE_537_CAP_CONCURRENCY)
      list(APPEND KERNEL_DEFINES_BARE PROBE_537_CAP_CONCURRENCY=1)
      message(STATUS "#537 probe: capability checks alone and on every processor at once (x86_64)")
  endif()
  if(UROS_ABLATE_615_DISPLACED_TO_SET)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_615_DISPLACED_TO_SET=1)
      message(STATUS "#615 ablation: a displaced bound thread goes to the set's run queue (x86_64)")
  endif()
  if(UROS_ABLATE_616_KERNEL_FRAME)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_616_KERNEL_FRAME=1)
      message(STATUS "#616 ablation: kernel threads are born with a user frame (x86_64)")
  endif()
  if(UROS_ABLATE_612_BURNIN_CLI)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_612_BURNIN_CLI=1)
      message(STATUS "#612 ablation: the clock burn-in returns with interrupts off (x86_64)")
  endif()
  if(UROS_ABLATE_612_LEAVE_MAPPED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_612_LEAVE_MAPPED=1)
      message(STATUS "#612 ablation: boot self-tests leave pages in the VM's range (x86_64)")
  endif()
  if(UROS_ABLATE_608_NO_DRAIN_THREAD)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_608_NO_DRAIN_THREAD=1)
      message(STATUS "#608 ablation: no RCU drain thread (x86_64)")
  endif()
  if(UROS_ABLATE_526_IDLE_RESTORES)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_526_IDLE_RESTORES=1)
      message(STATUS "#526 ablation: the idle loop restores the level it found (x86_64)")
  endif()
  if(UROS_ABLATE_617_RETURN_AST)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_617_RETURN_AST=1)
      message(STATUS "#617 ablation: thread_return_ast() looks with interrupts on (x86_64)")
  endif()
  if(UROS_WIDEN_617_WINDOW)
      list(APPEND KERNEL_DEFINES_BARE WIDEN_617_WINDOW=1)
      message(STATUS "#617 test: the return-to-user AST window widened (x86_64)")
  endif()
  if(UROS_ABLATE_526_SPLX_WINDOW)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_526_SPLX_WINDOW=1)
      message(STATUS "#526 ablation: splx() finds the per-CPU block with interrupts on (x86_64)")
  endif()
  if(UROS_WIDEN_638_WINDOW)
      list(APPEND KERNEL_DEFINES_BARE WIDEN_638_WINDOW=1)
      message(STATUS "#638 test: the shootdown's windows widened while -J runs (x86_64)")
  endif()
  if(UROS_ABLATE_639_AP_CONTROL)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_639_AP_CONTROL=1)
      message(STATUS "#639 ablation: the application processors keep the trampoline's control registers (x86_64)")
  endif()
  if(UROS_ABLATE_638_FLUSH_UNPINNED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_638_FLUSH_UNPINNED=1)
      message(STATUS "#638 ablation: tlb_flush_range() flushes and cross-calls with preemption on (x86_64)")
  endif()
  if(UROS_ABLATE_638_STRIKE_UNPINNED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_638_STRIKE_UNPINNED=1)
      message(STATUS "#638 ablation: ipi_call_mask() strikes its own bit with preemption on (x86_64)")
  endif()
  if(UROS_ABLATE_558_PVLOCK)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_558_PVLOCK=1)
      message(STATUS "#558 ablation: the pv per-page lock is OFF (x86_64)")
  endif()
  if(UROS_ABLATE_558_PVFREELOCK)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_558_PVFREELOCK=1)
      message(STATUS "#558 ablation: the pv free list lock is OFF (x86_64)")
  endif()
  if(UROS_ABLATE_563_WINDOW)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_563_WINDOW=1)
      message(STATUS "#563 ablation: the copy's window is never opened (x86_64)")
  endif()
  if(UROS_ABLATE_551_UNBOUNDED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_551_UNBOUNDED=1)
      message(STATUS "#551 ablation: cons_putc waits for the transmitter for ever (x86_64)")
  endif()
  if(UROS_ABLATE_566_NO_GRACE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_566_NO_GRACE=1)
      message(STATUS "#566 ablation: pmap_destroy does not wait a grace period (x86_64)")
  endif()
  if(UROS_ABLATE_566_NO_IPI_WAIT)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_566_NO_IPI_WAIT=1)
      message(STATUS "#566 ablation: ipi_call_others does not wait for acks (x86_64)")
  endif()
  if(UROS_ABLATE_567_NO_FIFO)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_567_NO_FIFO=1)
      message(STATUS "#567 ablation: one byte per look at the transmitter (x86_64)")
  endif()
  if(UROS_ABLATE_567_NO_RING)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_567_NO_RING=1)
      message(STATUS "#567 ablation: the console writes the wire in the caller's thread (x86_64)")
  endif()
  if(NOT UROS_ABLATE_570_REFUSE_IOBAR STREQUAL "")
      list(APPEND KERNEL_DEFINES_BARE ABLATE_570_REFUSE_IOBAR=${UROS_ABLATE_570_REFUSE_IOBAR})
      message(STATUS "#570 ablation: a PCI I/O BAR is refused to its holder after ${UROS_ABLATE_570_REFUSE_IOBAR} accesses (x86_64)")
  endif()
  list(APPEND KERNEL_DEFINES_BARE ${UROS_ABLATE_577_DEFS})
  if(UROS_ABLATE_597_NO_LOCK OR NOT UROS_ABLATE_597_WIDEN STREQUAL "")
      message(FATAL_ERROR "UROS_ABLATE_597_* ablate i386's PCI configuration lock; x86-64's is in x86_64/cpu/pci_cfg.c and is not touched by them")
  endif()
  if(UROS_ABLATE_599_MASK_PIT OR UROS_ABLATE_599_CLI_SPIN OR UROS_ABLATE_599_SPIN_ONLY OR UROS_ABLATE_599_SPL_LEAK OR UROS_ABLATE_599_NO_LOCK OR UROS_ABLATE_599_NESTED_PRINTF OR UROS_ABLATE_599_PRINTF_WAITS OR NOT UROS_ABLATE_599_WIDEN STREQUAL "")
      message(FATAL_ERROR "UROS_ABLATE_599_* stop i386's 8254 tick for i386's clock watch; x86-64 has neither")
  endif()
  if(UROS_ABLATE_599_I8042_SHARED OR UROS_ABLATE_599_COMPROBE_OLD_EXIT OR UROS_ABLATE_599_COM_NO_LOCK OR NOT UROS_ABLATE_599_WIDEN_DIVISOR STREQUAL "")
      message(FATAL_ERROR "UROS_ABLATE_599_I8042_SHARED, _COMPROBE_OLD_EXIT, _COM_NO_LOCK and _WIDEN_DIVISOR ablate i386's 8042 reader and com.c; x86-64 has neither")
  endif()
  if(UROS_ABLATE_599_WINDOW_OPEN)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_599_WINDOW_OPEN=1)
      message(STATUS "#599 ablation: the I/O APIC window is used without its lock (x86_64)")
  endif()
  if(NOT UROS_ABLATE_599_WINDOW_WIDEN STREQUAL "")
      list(APPEND KERNEL_DEFINES_BARE ABLATE_599_WINDOW_WIDEN=${UROS_ABLATE_599_WINDOW_WIDEN})
      message(STATUS "#599 ablation: ${UROS_ABLATE_599_WINDOW_WIDEN} port 0x80 reads between an I/O APIC select and its use (x86_64)")
  endif()
  if(UROS_ABLATE_599_HALT_ORDER)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_599_HALT_ORDER=1)
      message(STATUS "#599 ablation: halt_cpu in its old order (x86_64)")
  endif()
  if(UROS_ABLATE_586_NO_TICKS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_586_NO_TICKS=1)
      message(STATUS "#586 ablation: the tick self-test counts no ticks (x86_64)")
  endif()
  if(UROS_ABLATE_508_ONE_RUN_OUT)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_ONE_RUN_OUT=1)
      message(STATUS "#508 ablation: one TSC calibration run reads 1/32 high (x86_64)")
  endif()
  if(UROS_ABLATE_508_END_DELAYED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_END_DELAYED=1)
      message(STATUS "#508 ablation: one calibration run's end is read ~3 ms late (x86_64)")
  endif()
  if(UROS_ABLATE_508_RULER_LIES)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_RULER_LIES=1)
      message(STATUS "#508 ablation: the PM timer's rate is taken 1/32 high (x86_64)")
  endif()
  if(UROS_ABLATE_508_NO_HPET)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_NO_HPET=1)
      message(STATUS "#508 ablation: the ACPI HPET table is withheld (x86_64)")
  endif()
  if(UROS_ABLATE_508_EXACT_HIDDEN)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_EXACT_HIDDEN=1)
      message(STATUS "#508 ablation: the exact TSC sources are not seen (x86_64)")
  endif()
  if(UROS_ABLATE_508_EXACT_LIES)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_EXACT_LIES=1)
      message(STATUS "#508 ablation: the first exact TSC source states a quarter high (x86_64)")
  endif()
  if(UROS_ABLATE_508_CPUID_15)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_CPUID_15=1)
      message(STATUS "#508 ablation: CPUID 0x15 states OMEGA's crystal and ratio (x86_64)")
  endif()
  if(UROS_ABLATE_594_TSC_SKEWS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_594_TSC_SKEWS=1)
      message(STATUS "#594 ablation: the TSC runs 1/32 fast as the watchdog reads it (x86_64)")
  endif()
  if(UROS_ABLATE_594_NTP_SLEW)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_594_NTP_SLEW=1)
      message(STATUS "#594 ablation: the rulers carry a host's NTP phase slew (x86_64)")
  endif()
  if(UROS_ABLATE_594_HPET_STOPS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_594_HPET_STOPS=1)
      message(STATUS "#594 ablation: the HPET stops as the watchdog reads it (x86_64)")
  endif()
  if(UROS_ABLATE_593_NO_APIC_TIMER)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_NO_APIC_TIMER=1)
      message(STATUS "#593 ablation: the local APIC timer is unusable in both modes (x86_64)")
  endif()
  if(UROS_ABLATE_593_NO_COUNTDOWN)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_NO_COUNTDOWN=1)
      message(STATUS "#593 ablation: only the local APIC timer's countdown is unusable (x86_64)")
  endif()
  if(UROS_ABLATE_593_LINE_TAKEN)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_LINE_TAKEN=1)
      message(STATUS "#593 ablation: the RTC's input is in use when the HPET chooses its route (x86_64)")
  endif()
  if(UROS_ABLATE_593_BEHIND)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_BEHIND=1)
      message(STATUS "#593 ablation: every 128th HPET comparator write lands behind the counter (x86_64)")
  endif()
  if(UROS_ABLATE_593_UNCHECKED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_UNCHECKED=1)
      message(STATUS "#593 ablation: an HPET comparator write is not read back (x86_64)")
  endif()
  if(UROS_ABLATE_593_FROM_NOW)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_FROM_NOW=1)
      message(STATUS "#593 ablation: the APIC timer's deadlines are counted from now, not from the last one (x86_64)")
  endif()
  if(UROS_ABLATE_593_ICR_OPEN)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_ICR_OPEN=1)
      message(STATUS "#593 ablation: an IPI send may be interrupted between its two register writes (x86_64)")
  endif()
  if(UROS_ABLATE_593_FIRMWARE_LEFT)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_FIRMWARE_LEFT=1)
      message(STATUS "#593 ablation: the firmware left the HPET's LegacyReplacement on and a comparator interrupting (x86_64)")
  endif()
  if(UROS_ABLATE_593_LEFT_ON)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_LEFT_ON=1)
      message(STATUS "#593 ablation: the HPET's comparator is left interrupting when the tick leaves it (x86_64)")
  endif()
  if(UROS_ABLATE_593_HPET_NAMED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_HPET_NAMED=1)
      message(STATUS "#593 ablation: the HPET's counter is named before the clock is chosen (x86_64)")
  endif()
  if(UROS_ABLATE_593_NEVER_TICKS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_593_NEVER_TICKS=1)
      message(STATUS "#593 ablation: processor 1's tick is never armed (x86_64)")
  endif()
  if(UROS_ABLATE_603_IPI_EMPTY)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_603_IPI_EMPTY=1)
      message(STATUS "#603 ablation: the AST interrupt does not run ast_check() (x86_64)")
  endif()
  if(UROS_ABLATE_603_ONE_PASS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_603_ONE_PASS=1)
      message(STATUS "#603 ablation: a return to ring 3 takes ASTs once (x86_64)")
  endif()
  if(UROS_ABLATE_603_FAULT_AST)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_603_FAULT_AST=1)
      message(STATUS "#603 ablation: a page fault resolved for ring 3 returns without taking ASTs (x86_64)")
  endif()
  if(UROS_ABLATE_604_UPDATE_GONE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_604_UPDATE_GONE=1)
      message(STATUS "#604 ablation: pmap_pte_update() writes into an entry that is gone (x86_64)")
  endif()
  if(UROS_ABLATE_604_FORGET_TWO_WALKS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_604_FORGET_TWO_WALKS=1)
      message(STATUS "#604 ablation: pmap_forget() walks twice, the first outside a read section (x86_64)")
  endif()
  if(UROS_ABLATE_604_UNMAP_STORE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_604_UNMAP_STORE=1)
      message(STATUS "#604 ablation: the unmap stores zero instead of exchanging it (x86_64)")
  endif()
  if(UROS_ABLATE_604_SPLIT_DROPS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_604_SPLIT_DROPS=1)
      message(STATUS "#604 ablation: a split drops what it does not know, and its interior entry has no USER (x86_64)")
  endif()
  if(UROS_ABLATE_604_COLLECT_MASKED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_604_COLLECT_MASKED=1)
      message(STATUS "#604 probe: the -M bench collects once with interrupts off (x86_64)")
  endif()
  if(UROS_ABLATE_606_FORGET_BITS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_606_FORGET_BITS=1)
      message(STATUS "#606 ablation: a removed or replaced entry keeps nothing of its ACCESSED and DIRTY bits (x86_64)")
  endif()
  if(UROS_PROBE_606_PAGEOUT)
      list(APPEND KERNEL_DEFINES_BARE PROBE_606_PAGEOUT=1)
      message(STATUS "#606 probe: the pageout's counters, printed when they move (x86_64)")
  endif()
  if(UROS_ABLATE_606_PAGEOUT_WAIT)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_606_PAGEOUT_WAIT=1)
      message(STATUS "#606 ablation: vm_pageout() asserts nobody is waiting, and sleeps with no end (x86_64)")
  endif()
  if(UROS_PROBE_605_SILENT)
      list(APPEND KERNEL_DEFINES_BARE PROBE_605_SILENT=1)
      message(STATUS "#605 probe: one processor halted and cross-called (x86_64)")
  endif()
  if(UROS_PROBE_605_CALL_COST)
      list(APPEND KERNEL_DEFINES_BARE PROBE_605_CALL_COST=1)
      message(STATUS "#605 probe: broadcast cross-calls timed at boot (x86_64)")
  endif()
  if(UROS_PROBE_605_PADDED)
      list(APPEND KERNEL_DEFINES_BARE PROBE_605_PADDED=1)
      message(STATUS "#605 probe: answer-counter slots one cache line each (x86_64)")
  endif()
  if(UROS_ABLATE_607_SWAPPED_PARKED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_607_SWAPPED_PARKED=1)
      message(STATUS "#607 ablation: a swapped-out waiter counts as parked (x86_64)")
  endif()
  if(UROS_ABLATE_607_OLD_WAKE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_607_OLD_WAKE=1)
      message(STATUS "#607 ablation: the hand-off's old wake of a waiter it does not switch to (x86_64)")
  endif()
  if(UROS_ABLATE_607_DTS_SWAPPED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_607_DTS_SWAPPED=1)
      message(STATUS "#607 ablation: the IPC direct switch takes a swapped-out receiver (x86_64)")
  endif()
  if(UROS_ABLATE_598_INTCTL_FORWARD)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_598_INTCTL_FORWARD=1)
      message(STATUS "#598 ablation: AMD IntCtl 01b, interrupts forwarded unmapped (x86_64)")
  endif()
  if(UROS_ABLATE_598_IIDX_HIGH)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_598_IIDX_HIGH=1)
      message(STATUS "#598 ablation: IEC invalidation index at 63:48 (x86_64)")
  endif()
  if(UROS_ABLATE_598_QI_TAIL_UNSHIFTED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_598_QI_TAIL_UNSHIFTED=1)
      message(STATUS "#598 ablation: the queue's tail written as an index (x86_64)")
  endif()
  if(UROS_ABLATE_598_QI_FLUSH_BY_REGISTER)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_598_QI_FLUSH_BY_REGISTER=1)
      message(STATUS "#598 ablation: a flush through the IOTLB register with the queue on (x86_64)")
  endif()
  if(UROS_ABLATE_594_CRYSTAL_IS_BUS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_594_CRYSTAL_IS_BUS=1)
      message(STATUS "#594 ablation: CPUID 0x15 states the timing leaf's bus as the crystal (x86_64)")
  endif()
  if(UROS_ABLATE_508_SPAN_UNBOUNDED)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_SPAN_UNBOUNDED=1)
      message(STATUS "#508 ablation: an MMIO mapping is checked on its first page only (x86_64)")
  endif()
  if(UROS_ABLATE_508_LEND_RULERS)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_508_LEND_RULERS=1)
      message(STATUS "#508 ablation: the kernel keeps none of its rulers' ports (x86_64)")
  endif()
  if(UROS_ABLATE_586_TSC_DISAGREE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_586_TSC_DISAGREE=1)
      message(STATUS "#586 ablation: each TSC calibration run reads a different 1/32 high, so none agree and it declines (x86_64)")
  endif()
  if(UROS_ABLATE_563_ALONE)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_563_ALONE=1)
      message(STATUS "#563 ablation: every test believes it is alone (x86_64)")
  endif()
  if(UROS_ABLATE_563_NO_IOAPIC)
      list(APPEND KERNEL_DEFINES_BARE ABLATE_563_NO_IOAPIC=1)
      message(STATUS "#563 ablation: the MADT lists no I/O APIC (x86_64)")
  endif()
  if(UROS_VM_PAGE_POISON)
      list(APPEND KERNEL_DEFINES_BARE VM_PAGE_POISON=1)
      message(STATUS "#385 free-page poison: ON (x86_64)")
  endif()
  if(UROS_IPC_PORT_TRACK)
      list(APPEND KERNEL_DEFINES_BARE IPC_PORT_TRACK=1)
      message(STATUS "#485 port allocation tracking: ON (x86_64)")
  endif()
  # The other arm of the #486 option.  Nothing on x86-64 feeds the census --
  # this target supplies its own usimple_lock (x86_64/sync/lock.c, #453), so
  # kern/lock.c's hooks are not compiled here at all.  What the switch buys on
  # this target is that the facility COMPILES, and cannot rot behind a switch
  # only the other machine can turn on.
  if(UROS_USLOCK_CENSUS)
      list(APPEND KERNEL_DEFINES_BARE USLOCK_CENSUS=1)
      message(STATUS "#486 simple-lock census: ON (x86_64, storage only)")
  endif()
  list(TRANSFORM KERNEL_DEFINES_BARE PREPEND -D OUTPUT_VARIABLE KERNEL_DEFINES)

  # ⚠️ #452 added the machine-independent tree to this list, and it is the
  # first time this target has seen it.  sync/mutex.c implements the mutex
  # kern/lock.h declares, so it has to read that header -- a machine-dependent
  # file cannot implement a machine-independent contract it cannot see.
  #
  # Ordered as the i386 target orders them, and as scripts/mi-x86_64-probe.sh
  # does: uapi, the arch links, x86_64/, then the MI root.  x86_64/ comes
  # first so <sync/lock.h> is this target's own; that is also why the x86-64
  # directories are named after the MD contracts rather than after kern/,
  # vm/, ipc/ -- none of those names can ever collide.
  # Where the generated MIG output goes, and where its headers are found.
  #
  # ⚠️ Set HERE, before target_include_directories() below, and not beside
  # the rules that fill it.  It used to be assigned ninety lines further
  # down, so the include path was built from an empty variable and carried
  # `-I/mach' -- a directory in the root of the filesystem (#453).
  #
  # Nothing noticed for as long as no source needing a generated header was
  # compiled.  The first four that were -- ipc_kmsg.c, ds_routines.c,
  # dev_pager.c, device/subrs.c -- all stopped on <mach/mach_server.h>, and
  # the path in the error message is what gave it away.  cmake does not warn
  # about an -I that does not exist, and an empty variable in a path
  # concatenation is silent everywhere.
  set(X86_64_MIG_DIR ${CMAKE_CURRENT_BINARY_DIR})
  file(MAKE_DIRECTORY ${X86_64_MIG_DIR}/mach ${X86_64_MIG_DIR}/mach_debug
                      ${X86_64_MIG_DIR}/device)

  target_include_directories(mach_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/uapi
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${UROS_CONFIG_INCLUDE}
    ${X86_64_MIG_DIR}
    ${X86_64_MIG_DIR}/mach
    # #457: pci_bar.h, beside the decode it declares -- the stubs include it
    # by quoted name from its own directory.
    ${CMAKE_SOURCE_DIR}/src/lib/libpci
    )
  # -nostdinc drops the compiler's own freestanding headers too (stdint.h,
  # stddef.h); add just that directory back with -isystem so we get the
  # fixed-width types without pulling in the hosted system headers.
  execute_process(COMMAND ${CMAKE_C_COMPILER} -m64 -print-file-name=include
    OUTPUT_VARIABLE X86_64_GCC_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
  # The C is bare-metal higher-half: -mcmodel=kernel so absolute references
  # land in the top-2 GiB where the image is linked; -mno-red-zone because
  # the red zone is unsafe once interrupts land on these stacks; SSE off
  # (general regs only) to match the kernel's SSE-free discipline.
  #
  # -fno-omit-frame-pointer is kept for the same reason the i386 kernel keeps
  # it: the backtrace walks the frame chain, and a register is a cheaper
  # price than not knowing how the machine got where it died.
  target_compile_options(mach_kernel PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-m64 -ffreestanding -nostdinc>
    $<$<COMPILE_LANGUAGE:C>:${KERNEL_OPT_FLAGS}
      -m64 -mcmodel=kernel -mno-red-zone
      -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-pic
      -fno-pie -fno-stack-protector -fno-builtin -nostdinc
      -fno-omit-frame-pointer
      # ⚠️ Three flags i386 carries and this target did not (#453).
      #
      # -fno-strict-aliasing is the one that matters.  This kernel casts
      # between unrelated struct types constantly -- a queue_entry_t into an
      # ipc_port_t, a vm_map_copy_t into a page list -- and with strict
      # aliasing on at -O2 the compiler is entitled to assume writes through
      # one type cannot be seen through the other, and to keep a stale value
      # in a register across them.  That is a miscompilation, it is silent,
      # and it would land on the target that has never executed an
      # instruction.
      #
      # -fno-optimize-sibling-calls keeps the frame chain intact.  A tail
      # call reuses the caller's frame, so the walker in x86_64/trap/trap.c
      # loses a level -- and halt_cpu() runs that walker on the panic path,
      # which is exactly when a missing level is the one you needed.
      #
      # -fno-common is the GCC 10 default now, and is stated rather than
      # assumed: a tentative definition landing in COMMON would let two
      # translation units share a variable they each believe they own.
      -fno-common -fno-strict-aliasing -fno-optimize-sibling-calls
      -isystem ${X86_64_GCC_INC} -std=gnu11 -Wall
      #
      # The same six suppressions i386 carries, and for the same reason.
      #
      # ⚠️ Measured before adopting, because "the same noise" is a claim.
      # Built i386 once with this list removed: 356 warnings, and every one
      # of these six classes is LARGER there than here -- 205 unused
      # variables against 128, 80 parentheses against 44, 12 missing braces
      # against none.  i386 compiles more files, so it has more of the same
      # heritage noise; not one of them is a 64-bit finding.
      #
      # What that leaves is the point: the classes i386 reports ZERO of --
      # pointer-to-int-cast, int-to-pointer-cast, format, overflow -- are
      # exactly the ones this port exists to find, and they stay on.
      #
      # ⚠️ -Wdangling-else is part of -Wparentheses, so it goes quiet with
      # it.  i386 has five and this target one; worth knowing that turning
      # -Wparentheses back on is what would show them.
      -Wno-pointer-sign
      -Wno-unused-variable
      -Wno-unused-function
      -Wno-unused-but-set-variable
      -Wno-missing-braces
      -Wno-parentheses>)
  # The same configuration the mig rules generate against, now also for the
  # code (#452).  Without NCPUS the config header defaults it to 1, and
  # kern/lock.h compiles every mutex operation away to an empty macro -- so
  # sync/mutex.c defined functions whose declarations had become `#define
  # _mutex_lock(l)`, and the error was a syntax error a hundred lines from
  # the cause.  A kernel built for one processor is a real configuration;
  # silently becoming one is not.
  target_compile_definitions(mach_kernel PRIVATE ${KERNEL_DEFINES_BARE})

  target_link_options(mach_kernel PRIVATE
    -m64 -static -nostdlib -nostartfiles -no-pie
    -T ${X86_64_BOOT_LD}
    -Wl,--build-id=none)
  set_target_properties(mach_kernel PROPERTIES
    LINK_DEPENDS ${X86_64_BOOT_LD}
    OUTPUT_NAME "mach_kernel"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/uros/boot")

  # GRUB2 multiboot2 rescue ISO — the dev loop for the x86-64 kernel.
  # `ninja uros_iso` stages the kernel + a one-entry grub.cfg (timeout 0)
  # and calls grub-mkrescue; boot with `qemu-system-x86_64 -cdrom`.
  set(X86_64_ISO_DIR ${CMAKE_BINARY_DIR}/iso-x86_64)
  set(X86_64_ISO ${CMAKE_BINARY_DIR}/uros-x86_64.iso)
  #
  # ⚠️ Which menu entry boots is written at BUILD time, from the environment
  # (#458).
  #
  # grub.cfg has three entries and `timeout=0', so whichever `default' names
  # is the one an unattended run gets -- and until this existed, that was
  # always entry 0 and the other two could not be reached by any automated
  # boot at all.  The `-r' debugger entry had been in that position since it
  # was written, under a comment saying "a flag that cannot be given is a flag
  # that is never used", which was true of itself.
  #
  # It matters more now: the terminal double-fault self-test moved to entry 2
  # when the ordinary boot went on into the machine-independent kernel, and a
  # test nothing can select is a test that has stopped running.
  # 🔴 NO VECTOR INSTRUCTION IN UNDECLARED KERNEL CODE (#561).
  #
  # The switch stopped carrying a thread's vector state for threads of the
  # kernel task, which is worth 114 ns a switch and is correct only while
  # kernel code contains no vector instruction outside the places that declare
  # one.  -mgeneral-regs-only stops the COMPILER; it does not stop hand-written
  # asm, and this tree has some on purpose.
  #
  # ⚠️ POST_BUILD on the kernel itself, not a target somebody has to remember:
  # #549 found three checkers in scripts/ that nothing ran -- zero call sites in
  # CMake, zero in any script -- and a check nobody runs is a check that is not
  # there.  This one fails the link that produced the binary it read.
  add_custom_command(TARGET mach_kernel POST_BUILD
    COMMAND ${CMAKE_SOURCE_DIR}/../scripts/kernel-vector-check.py
            $<TARGET_FILE:mach_kernel>
    COMMENT "#561: checking no undeclared kernel code uses vector registers"
    VERBATIM)

  add_custom_target(uros_iso
    COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_64_ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mach_kernel>
            ${X86_64_ISO_DIR}/boot/mach_kernel
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
            ${X86_64_ISO_DIR}/boot/grub/grub.cfg
    # The first 64-bit user image, as module 0 (#422).  Every entry loads it:
    # without a module the kernel panics in bootstrap_create(), so an entry
    # that omitted it would be an entry that cannot reach the end of boot.
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin/boot_probe
            ${X86_64_ISO_DIR}/boot/boot_probe
    # And the name server (#426), for the entry that boots it in place of
    # boot_probe.  A server that is built and never started is collaudated by
    # nothing -- and what it exercises is not what boot_probe does: MIG in both
    # directions, a message loop, and printf through the device port.
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin/name_server
            ${X86_64_ISO_DIR}/boot/name_server
    # And bootstrap with its bundle, for the entry that boots the system the
    # way the system is meant to boot (#422): module 0 is init and module 1 is
    # what init reads.  ⚠️ Two files and not one -- a bootstrap staged without
    # its bundle starts, finds nothing to start, and says so in a sentence that
    # sounds like the kernel's fault.
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin/bootstrap
            ${X86_64_ISO_DIR}/boot/bootstrap
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_BINARY_DIR}/bootstrap.bundle
            ${X86_64_ISO_DIR}/boot/bootstrap.bundle
    COMMAND sh -c "echo set default=$\{UROS_X86_64_BOOT_ENTRY:-0\} > ${X86_64_ISO_DIR}/boot/grub/entry.cfg"
    COMMAND grub-mkrescue -o ${X86_64_ISO} ${X86_64_ISO_DIR}
    DEPENDS mach_kernel boot_probe name_server_bin
            bootstrap_server bootstrap_bundle
            ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
    COMMENT "Building GRUB2 multiboot2 rescue ISO: ${X86_64_ISO}"
    VERBATIM)


  # ------------------------------------------------------------------ #416
  # Run mig for this target.
  #
  # The nine machine-independent sources that still fail here all fail the
  # same way: they include a generated *_server.h that has never been
  # generated, because the rules lived below this branch's return() with
  # -I i386 written into them.  They are shared now (cmake/mig.cmake); what
  # was left was to call them.
  #
  # migcom itself needed nothing: run by hand against the x86-64 include path
  # it produced a server and a header with no diagnostics, and the 415 layout
  # assertions it emits into that server all hold when the result is compiled
  # -m64.  So its arithmetic about message layout is already right for an ABI
  # where vm_address_t, vm_offset_t and vm_size_t are 64 bits wide and
  # natural_t and integer_t are not.  That was worth checking before building
  # anything on top of it, and it is what the assertions exist for.
  #
  # ⚠️ mach/i386/mach_i386.defs has no counterpart here.  x86-64 has no
  # mach_x86_64.defs, which is also why <machine/machine_routines.h> is one
  # of the headers #450 left unwritten: it names MACHINE_SERVER, and there is
  # no subsystem to name yet.  That is #413's to decide, not something to
  # invent here.

  set(UROS_MIG_INCLUDES
      -I${UROS_ARCH_INCLUDE}
      -I${CMAKE_CURRENT_SOURCE_DIR}
      -I${CMAKE_CURRENT_SOURCE_DIR}/include
      -I${CMAKE_CURRENT_SOURCE_DIR}/x86_64
      -I${UROS_CONFIG_INCLUDE})

  # ⚠️ Neither UROS_MIG_TARGET_ARGS nor UROS_MIG_DEFS_ARCH is set here any
  # more.  Both said x86_64 in this directory and nothing anywhere else, which
  # is how a rule written in another CMakeLists got i386 layout in silence --
  # and how MIG_PTR_BITS would have failed to reach the userland .defs at all.
  # They come from the top-level CMakeLists now, where the architecture is
  # decided (#426, #422).

  # No AT386, no EISA/HIMEM/NPCI/MP_V1_1: those name i386 hardware, and the
  # generated code is the wire format, which must not depend on them.

  add_mig_server(${UROS_UAPI_DIR}/mach/mach.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/mach_port.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/mach_host.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/mach_klog.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/clock.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/sync.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/bootstrap.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach/ledger.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/mach_debug/mach_debug.defs
                 ${X86_64_MIG_DIR}/mach_debug X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/device/device.defs
                 ${X86_64_MIG_DIR}/device X86_64_MIG)
  add_mig_server(${UROS_UAPI_DIR}/device/device_master.defs
                 ${X86_64_MIG_DIR}/device X86_64_MIG)
  add_mig_user(${UROS_UAPI_DIR}/mach/exc.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${UROS_UAPI_DIR}/mach/memory_object.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${UROS_UAPI_DIR}/mach/memory_object_default.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${UROS_UAPI_DIR}/mach/clock_reply.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${UROS_UAPI_DIR}/device/device_reply.defs
               ${X86_64_MIG_DIR}/device X86_64_MIG)

  # device_pager_server -- the one rule that is not add_mig_server().
  #
  # The device pager answers the memory-object protocol, so its server is
  # generated from mach/memory_object.defs a second time, into a different
  # pair of files.  device/dev_pager.c then #defines memory_object_* to
  # device_pager_* and #includes the generated .c, which is why only the
  # header is named in a source list and the .c never is.
  #
  # It has to be spelled out because add_mig_server() derives its output
  # names from the .defs name, and here the two differ on purpose.  The
  # include path is this target's, not i386's: same generator, same input,
  # different machine_types.defs behind <mach/machine/*> (#416).
  add_custom_command(
    OUTPUT ${X86_64_MIG_DIR}/device/device_pager_server.c
           ${X86_64_MIG_DIR}/device/device_pager_server.h
    COMMAND ${CMAKE_C_COMPILER} -E -x c
            -I${CMAKE_SOURCE_DIR}/uapi
            -I${UROS_ARCH_INCLUDE}
            -I${CMAKE_CURRENT_SOURCE_DIR}/x86_64
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${UROS_CONFIG_INCLUDE}
            ${KERNEL_DEFINES}
            ${UROS_UAPI_DIR}/mach/memory_object.defs
            -o ${X86_64_MIG_DIR}/device/device_pager.mig.i
    COMMAND $<TARGET_FILE:migcom>
            ${UROS_MIG_TARGET_ARGS}
            -sheader ${X86_64_MIG_DIR}/device/device_pager_server.h
            -server ${X86_64_MIG_DIR}/device/device_pager_server.c
            -header /dev/null
            -user /dev/null
            ${X86_64_MIG_DIR}/device/device_pager.mig.i
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS}
            ${UROS_UAPI_DIR}/mach/memory_object.defs
    COMMENT "MIG x86-64: memory_object -> device_pager (server)"
    VERBATIM
  )
  list(APPEND X86_64_MIG_GENERATED
       ${X86_64_MIG_DIR}/device/device_pager_server.c
       ${X86_64_MIG_DIR}/device/device_pager_server.h)

  # The generated stubs are their own target as well as sources, because the
  # HEADERS have readers the .c files do not: ipc_kobject.c wants
  # device_pager_server.h, and nothing compiles that one.  A rule cmake has no
  # dependent for never runs at all.

  # ------------------------------------------------------- #453, the bring-up
  # The machine-independent kernel.
  #
  # 2026-08-03: it links.  Everything below this comment is the shared kernel
  # -- ipc/, kern/, vm/, device/ -- compiled and linked into an x86-64 image
  # for the first time, together with the MIG stubs generated for this target
  # and the two machine-dependent files that only it needs
  # (x86_64/thread/machine.c, x86_64/sync/mutex.c).
  #
  # ⚠️ It has still never executed an instruction.  Linking says every name a
  # caller wanted was defined by somebody; it says nothing about whether the
  # definition is right.
  #
  # ── What is NOT here, and why ────────────────────────────────────────
  #
  #   device/net_io.c        the in-kernel network stack, and the pseudo-
  #   device/net_device.c    device that exists to feed it packets.  Out by
  #   device/test_device.c   design, not by schedule: network cards belong to
  #                          user-space servers and the TCP/IP stack is a
  #                          server too, so there are no producers here.  The
  #                          hooks the stack planted in the core sit behind
  #                          MACH_NET_IN_KERNEL, which is 0 for this machine.
  #
  #   device/device_master.c IN, as of #457.  It was out because it was i386
  #                          code living in device/ -- <i386/ipl.h>,
  #                          <i386/pio.h>, <i386/ioapic.h> outside any
  #                          conditional, so compiling it here pulled
  #                          <mach/i386/vm_types.h> in behind them, where
  #                          vm_offset_t is thirty-two bits.  #457 named the
  #                          six operations it needs from the machine in
  #                          <device/device_machdep.h>; config space is the
  #                          one that genuinely changes mechanism, and does so
  #                          per BOARD rather than per architecture -- ECAM
  #                          where the MCFG describes one, the port pair where
  #                          it does not.  ⚠️ The DMA half is on without an
  #                          IOMMU behind it, which is #432.
  #
  #   ddb/                   no debugger yet (#428): MACH_KDB is 0, and so is
  #                          MACH_TR, whose trace buffer is read back through
  #                          db_show_tr and would otherwise promise a facility
  #                          this tree cannot deliver.
  #
  # ── What the count did ───────────────────────────────────────────────
  #
  #   2026-08-02, first measurement          97 undefined symbols
  #   after the VM bootstrap surface (#407)  89
  #   after mem_size / halt / Debugger       84
  #   after the console and usimple_lock     79
  #   after the device and clock tables      64
  #   after the processor surface            61
  #   after MACH_KDB=0                       44
  #   after the context switch and returns   44, and a different 44
  #   2026-08-03, remeasured                 16
  #   after MACH_TR=0 and the net knob        7
  #   after machine_kernel_ready              3
  #   after MACH_DEVICE_MASTER=0              1
  #   after x86_64/time/rtc.c                 0
  #
  # ⚠️ The jump from 44 to 16 is not work done between the two: it is one file
  # that had never been in the build.  x86_64/sync/mutex.c defines four names
  # with 2002 call sites behind them, and it was out because it calls
  # mutex_lock_wait() in kern/lock.c -- so it could not go in until the
  # machine-independent tree did.  Nothing had ever consumed it, which is
  # exactly why nothing had ever checked it.
  set(X86_64_MIG_C ${X86_64_MIG_GENERATED})
  list(FILTER X86_64_MIG_C INCLUDE REGEX "\\.c$")
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "device_pager_server\\.c$")
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "/device_server\\.c$")
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "device_reply_user\\.c$")
  target_sources(mach_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/sync/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/quiet_census.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/sync/mutex_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/blkio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/chario.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/cirbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/data_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_lookup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_name.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_pager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/device_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/device_master.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/device_machdep.c
    # 🔑 #457: the SAME BAR decode hal_server uses, compiled into the
    # kernel.  It is pure -- six raw slots in, regions out, no RPC and no
    # allocation -- which is what #427 isolated it for and this is the
    # first time both sides have it.  A second decoder here would be the
    # fifth copy of the thing that file exists to have deleted.
    ${CMAKE_SOURCE_DIR}/src/lib/libpci/pci_bar.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/pci_msix.c
    ${X86_64_MIG_DIR}/device/device_reply_user.c
    ${X86_64_MIG_DIR}/device/device_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/device_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/driver_lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/ds_routines.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/io_scatter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/subrs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_entry.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_kmsg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_mqueue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_notify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_object.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_pset.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_radix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_right.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_space.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/ipc_thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/mach_msg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ipc/mach_port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ast.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/boot_script.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/bootstrap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/cap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/counters.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/cpu_data.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/debug.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/etap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/eventcount.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/exception.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/fault_profile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/syscall_profile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/uslock_census.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/hmac_sha256.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_kobject.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_mig.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_subsystem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_sync.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ipc_tt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/kalloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/klog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/ledger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/lock_smoke.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/mach_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/mach_factor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/norma_bootstrap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/posixtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/printf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/priority.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/processor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/rcu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sched_prim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sha256.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sscanf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/startup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/subsystem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sync_lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sync_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/sync_sema.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/syscall_emulation.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/syscall_subr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/syscall_sw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/task_swap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/thread.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/thread_act.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/thread_pool.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/thread_swap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/version.c
    ${CMAKE_CURRENT_SOURCE_DIR}/kern/zalloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/memory_object.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_external.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_fault.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_kern.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_object.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_pageout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_resident.c
    ${CMAKE_CURRENT_SOURCE_DIR}/vm/vm_user.c
    ${X86_64_MIG_C})
  set_source_files_properties(${X86_64_MIG_GENERATED} PROPERTIES COMPILE_OPTIONS "-Wno-overflow")
  # ---------------------------------------------------------------------

  add_custom_target(uros_mig_stubs DEPENDS ${X86_64_MIG_GENERATED})
  add_dependencies(mach_kernel uros_mig_stubs)

  message(STATUS "x86-64 kernel: boot stub (#406) — MD contracts #407-#411 pending")
  return()
endif()

# #599: these ablate x86-64's I/O APIC window and halt_cpu, and have no i386
# half -- i386's pair is ablated by UROS_ABLATE_599_NO_LOCK and _WIDEN, and its
# halt path was not changed.
if(UROS_ABLATE_599_WINDOW_OPEN OR NOT UROS_ABLATE_599_WINDOW_WIDEN STREQUAL "" OR UROS_ABLATE_599_HALT_ORDER)
    message(FATAL_ERROR "UROS_ABLATE_599_WINDOW_OPEN, _WINDOW_WIDEN and _HALT_ORDER ablate x86-64's I/O APIC window and halt_cpu; i386's pair is UROS_ABLATE_599_NO_LOCK and _WIDEN")
endif()

# =============================================================================
# Kernel Configuration
# =============================================================================

set(KERNEL_TARGET "mach_kernel")
set(KERNEL_TEXTORG "0x100000")  # 1MB - physical load address

# #300 — SMP bring-up.  Default 1 keeps the v0.1.0 single-CPU build unchanged.
# Override with -DUROS_NCPUS=2 (or 4, 8) to enable the SMP code paths.
#
# #330 (dynamic per-CPU, option B): for SMP builds this is now the fixed
# *maximum* CPU count (the size of the per-CPU arrays), NOT the number you
# boot.  Set it once to a generous cap (e.g. 64); the real count comes from
# the MADT/MP table at runtime (real_ncpus, clamped to <= NCPUS).  The same
# binary then boots any N <= UROS_NCPUS with no rebuild and a fixed image
# layout.  See i386/AT386/cpus.h.
set(UROS_NCPUS "1" CACHE STRING "Max CPUs supported (per-CPU array size; 1 = UP). Runtime count from MADT.")
if(UROS_NCPUS GREATER 1)
    message(STATUS "SMP build: UROS_NCPUS=${UROS_NCPUS} (experimental, #300 in progress)")
    # MPS 1.x AP-boot path (mp.c, mp_v1_1.c, slave_boot.S).
    # ACPI MADT path will land via #305.
    set(KERNEL_SMP_DEFINES -DMP_V1_1=1)
else()
    message(STATUS "UP build: UROS_NCPUS=${UROS_NCPUS}")
    set(KERNEL_SMP_DEFINES "")
endif()

# Get GCC's internal include path (for stdarg.h, etc.) EARLY so we can use it in C_FLAGS
# Must use -m32 to get 32-bit compatible include path
execute_process(
    COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
    OUTPUT_VARIABLE GCC_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "GCC internal include path: ${GCC_INCLUDE_DIR}")

# Kernel-specific compiler flags
# -isystem is needed with -nostdinc to allow GCC internal headers (stdarg.h, etc.)
set(KERNEL_C_FLAGS
    ${UROS_ARCH_FLAGS}
    -mno-sse
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -nostdlib
    -nostdinc
    -isystem ${GCC_INCLUDE_DIR}
    -fno-builtin
    -fno-common
    -fno-strict-aliasing
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -std=gnu11
    -Wall
    #
    # #415.  Three suppressions used to live here and all three were about
    # width: -Wno-implicit-function-declaration, -Wno-int-conversion and
    # -Wno-format.  They are the two halves of this audit's cheap side, and
    # they were switched off wholesale.
    #
    # implicit-function-declaration is gone: an undeclared function is
    # assumed to return int, so on the next target every call that returns a
    # pointer or a physical address is truncated at the call site, silently
    # and in the caller rather than in the callee where you would look.
    #
    # -Wno-format is gone too.  This note used to say that removing it
    # changed nothing, because the kernel builds -fno-builtin with its own
    # print functions and none of them carried format(printf,...) -- so the
    # warning was guarding an empty room, and annotating them was a step
    # still to come.
    #
    # That step has been taken and the note had not noticed.  Enumerated
    # rather than believed: printf, sprintf, panic, log and sscanf all carry
    # the attribute in <kern/misc_protos.h>, on both targets, and both trees
    # build clean against it.  The room is not empty.
    #
    # The three that do not carry it -- db_printf, kdbprintf, iprintf -- are
    # bare on purpose, and misc_protos.h says why where the conversion parser
    # is defined: the debugger's formatter implements a different grammar
    # (%r, %z, %b, and %n meaning "unsigned in the current radix" rather than
    # C's store-through-a-pointer).  An annotation there would check against a
    # grammar the callee does not implement, and a check against the wrong
    # grammar is worse than none.
    #
    # int-conversion is FATAL again, which is #442's third done-when.
    #
    # #415 left it warning-but-not-fatal, and said why: two sites remained,
    # and they were one fact -- the kernel kept ipc_port_t pointers in
    # message fields typed mach_port_t, pointer-wide on i386 and not on
    # x86-64.  Stopping the build on that would have stopped it on a
    # decision that belonged to the message ABI, not to a compiler flag.
    #
    # #442 made that decision and carried it out: the ports live in the
    # kmsg, the message carries names, and the count is zero.  Fatal is now
    # the point -- it is what makes "by construction" mean something rather
    # than a line in a build log nobody reads.  If this ever fails to
    # compile, something has put an address back in a name field.
    -Wno-pointer-sign
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-missing-braces
    -Wno-parentheses
    ${KERNEL_OPT_FLAGS}
)

set(KERNEL_ASM_FLAGS
    ${UROS_ARCH_FLAGS}
    -DASSEMBLER
    -nostdinc
    -ffreestanding
)

set(KERNEL_DEFINES
    -DKERNEL
    -DMACH_KERNEL
    -DAT386=1
    -DNCPUS=${UROS_NCPUS}
    # ⚠️ MACH_ASSERT is NOT here (#485) — see the note in the x86-64 arm and
    # the knob table in cmake/kernel-config.cmake.
    -DMACH_DEBUG=1
    -DMACH_KDB=1
    -DMACH_HOST=1
    -DKERNEL_PRIVATE
    -DSTAT_TIME
    # Zero, like x86-64, and on this target it is correctness rather than a
    # cost (#560).  The switch carries the FPU state now, so a thread that has
    # trapped into the kernel has its live x87 registers IN the unit -- and
    # _doprnt_ext held 124 floating-point instructions, executed in whatever
    # thread called printf.  Formatting a float there overwrote that thread's
    # own registers, and it returned to user mode with them wrong and nothing
    # reporting it.  (The lazy scheme was no better: the #NM handler loaded
    # the thread's state and the conversion then clobbered it.)
    #
    # Nothing in the kernel asks for %f/%e/%g; the conversion is refused
    # rather than approximated, and prints <no-fp>.  See kern/printf.c.
    -DKERNEL_FLOAT_OK=0
    -DMACH_MACHINE_ROUTINES
    # -DMACH_PROF  # Disabled - requires prof.defs MIG generation
    -DKERNEL_SERVER=1
    -DTASK_SWAPPER=1
    # ODE device configuration options
    -DEISA=1
    -DHIMEM=1
    -DNPCI=1
    # GCC/ELF does not use underscore prefix for C symbols
    -D__NO_UNDERSCORES__
    ${KERNEL_SMP_DEFINES}
)

# #319 scheduler instrumentation: per-CPU counters + rdtsc around the global
# pset->runq.lock acquire in thread_select, dumped from sched_thread.
# Diagnostic tool: flip ON (cmake -DUROS_S319_INSTRUMENT=ON) when measuring
# scheduler/runq behavior, keep OFF for normal and perf builds.
option(UROS_S319_INSTRUMENT "Build the #319 runq instrumentation (s319 counters + dump)" OFF)
if(UROS_S319_INSTRUMENT)
    list(APPEND KERNEL_DEFINES -DS319_INSTRUMENT=1)
    message(STATUS "#319 s319 scheduler instrumentation: ON")
endif()

# The other arm of the #482 option declared above the arch branch.  Nothing on
# i386 opens a sample -- the hook that does lives in the x86-64 trap path -- so
# what this buys here is that the facility is COMPILED on both targets and
# cannot rot behind a switch only one of them can turn on.
if(UROS_SYSCALL_PROFILE)
    list(APPEND KERNEL_DEFINES -DSYSCALL_PROFILE=1
         -DSYSCALL_PROFILE_TRAP=${UROS_SYSCALL_PROFILE_TRAP})
endif()
if(UROS_FAULT_PROFILE)
    list(APPEND KERNEL_DEFINES -DFAULT_PROFILE=1)
    message(STATUS "#482 copy-on-write fault phase profile: ON (i386)")
endif()
if(UROS_ABLATE_537_HMAC_FULL)
    list(APPEND KERNEL_DEFINES -DABLATE_537_HMAC_FULL=1)
    message(STATUS "#537 ablation: the HMAC key's blocks hashed on every check (i386)")
endif()
foreach(_a MAC_EVERY_TIME EPOCH_KEPT PAGE_WALK IX_UNSORTED HMAC_LOCKED)
    if(UROS_ABLATE_537_${_a})
        list(APPEND KERNEL_DEFINES -DABLATE_537_${_a}=1)
        message(STATUS "#537 ablation: ${_a} (i386)")
    endif()
endforeach()
if(UROS_PROBE_537_CAP_CONCURRENCY)
    list(APPEND KERNEL_DEFINES -DPROBE_537_CAP_CONCURRENCY=1)
    message(STATUS "#537 probe: capability checks alone and on every processor at once (i386)")
endif()
if(UROS_ABLATE_615_DISPLACED_TO_SET)
    list(APPEND KERNEL_DEFINES -DABLATE_615_DISPLACED_TO_SET=1)
    message(STATUS "#615 ablation: a displaced bound thread goes to the set's run queue (i386)")
endif()
if(UROS_ABLATE_608_NO_DRAIN_THREAD)
    list(APPEND KERNEL_DEFINES -DABLATE_608_NO_DRAIN_THREAD=1)
    message(STATUS "#608 ablation: no RCU drain thread (i386)")
endif()
if(UROS_ABLATE_526_IDLE_RESTORES)
    list(APPEND KERNEL_DEFINES -DABLATE_526_IDLE_RESTORES=1)
    message(STATUS "#526 ablation: the idle loop restores the level it found (i386)")
endif()

foreach(_d IN LISTS UROS_ABLATE_577_DEFS)
    list(APPEND KERNEL_DEFINES -D${_d})
endforeach()
if(UROS_ABLATE_594_IOPL_REOPEN)
    list(APPEND KERNEL_DEFINES -DABLATE_594_IOPL_REOPEN=1)
    message(STATUS "#594 ablation: the iopl device lists the 8254 and its gate again (i386)")
endif()
if(UROS_ABLATE_594_UNCHECKED)
    list(APPEND KERNEL_DEFINES -DABLATE_594_UNCHECKED=1)
    message(STATUS "#594 ablation: the bitmap and the iopl emulation do not ask device_md_io_reserved (i386)")
endif()
if(UROS_ABLATE_597_NO_LOCK)
    list(APPEND KERNEL_DEFINES -DABLATE_597_NO_LOCK=1)
    message(STATUS "#597 ablation: the PCI configuration access takes no lock (i386)")
endif()
if(NOT UROS_ABLATE_597_WIDEN STREQUAL "")
    list(APPEND KERNEL_DEFINES -DABLATE_597_WIDEN=${UROS_ABLATE_597_WIDEN})
    message(STATUS "#597 ablation: ${UROS_ABLATE_597_WIDEN} port 0x80 reads between the PCI address and data (i386)")
endif()
if(UROS_ABLATE_599_MASK_PIT OR UROS_ABLATE_599_CLI_SPIN OR UROS_ABLATE_599_SPIN_ONLY OR UROS_ABLATE_599_SPL_LEAK OR UROS_ABLATE_599_NO_LOCK OR UROS_ABLATE_599_NESTED_PRINTF OR UROS_ABLATE_599_PRINTF_WAITS OR NOT UROS_ABLATE_599_WIDEN STREQUAL "")
    if(NOT UROS_NCPUS GREATER 1)
        message(FATAL_ERROR "UROS_ABLATE_599_* need UROS_NCPUS > 1: the clock watch runs on the other processors, and without it the ablation would stop the clock with nothing to say so")
    endif()
endif()
if(UROS_ABLATE_599_MASK_PIT)
    list(APPEND KERNEL_DEFINES -DABLATE_599_MASK_PIT=1)
    message(STATUS "#599 ablation: processor 0 masks the 8254's pin at tick 3000, nothing pending (i386)")
endif()
if(UROS_ABLATE_599_CLI_SPIN)
    list(APPEND KERNEL_DEFINES -DABLATE_599_CLI_SPIN=1)
    message(STATUS "#599 ablation: processor 0 takes the timeout list's lock and spins with interrupts off at tick 3000 (i386)")
endif()
if(UROS_ABLATE_599_SPIN_ONLY)
    list(APPEND KERNEL_DEFINES -DABLATE_599_SPIN_ONLY=1)
    message(STATUS "#599 ablation: the clock watch runs only from inside spins (i386)")
endif()
if(UROS_ABLATE_599_SPL_LEAK)
    list(APPEND KERNEL_DEFINES -DABLATE_599_SPL_LEAK=1)
    message(STATUS "#599 ablation: a thread_switch on processor 0 returns to user mode at splsched once (i386)")
endif()
if(UROS_ABLATE_599_NESTED_PRINTF)
    list(APPEND KERNEL_DEFINES -DABLATE_599_NESTED_PRINTF=1)
    message(STATUS "#599 ablation: processor 0 raises an unhandled line inside consolewrite's printf_lock hold once (i386)")
endif()
if(UROS_ABLATE_599_PRINTF_WAITS)
    list(APPEND KERNEL_DEFINES -DABLATE_599_PRINTF_WAITS=1)
    message(STATUS "#599 ablation: a nested printf waits for printf_lock, as before the fix (i386)")
endif()
if(UROS_ABLATE_599_NO_LOCK)
    list(APPEND KERNEL_DEFINES -DABLATE_599_NO_LOCK=1)
    message(STATUS "#599 ablation: the I/O APIC select/window pair takes no lock (i386)")
endif()
# Not with the clock-watch set above: the 8042 window exists at one
# processor too, which is where it shows.
if(UROS_ABLATE_599_I8042_SHARED)
    list(APPEND KERNEL_DEFINES -DABLATE_599_I8042_SHARED=1)
    message(STATUS "#599 ablation: the break-key reader keeps reading the 8042 while a task holds it (i386)")
endif()
if(UROS_ABLATE_599_COMPROBE_OLD_EXIT)
    list(APPEND KERNEL_DEFINES -DABLATE_599_COMPROBE_OLD_EXIT=1)
    message(STATUS "#599 ablation: comprobe/comattach clobber unit 0 without -r (i386)")
endif()
# The divisor race needs two processors (com_divisor_test.c, -U), but the
# lock is also what a uniprocessor boot takes, so neither is refused there:
# a -U boot on one processor answers NOT ASKED by itself.
if(UROS_ABLATE_599_COM_NO_LOCK)
    list(APPEND KERNEL_DEFINES -DABLATE_599_COM_NO_LOCK=1)
    message(STATUS "#599 ablation: com_bank_lock is not taken (i386)")
endif()
if(NOT UROS_ABLATE_599_WIDEN_DIVISOR STREQUAL "")
    list(APPEND KERNEL_DEFINES -DABLATE_599_WIDEN_DIVISOR=${UROS_ABLATE_599_WIDEN_DIVISOR})
    message(STATUS "#599 ablation: ${UROS_ABLATE_599_WIDEN_DIVISOR} port 0x80 reads between DLL and DLM (i386)")
endif()
if(NOT UROS_ABLATE_599_WIDEN STREQUAL "")
    list(APPEND KERNEL_DEFINES -DABLATE_599_WIDEN=${UROS_ABLATE_599_WIDEN})
    message(STATUS "#599 ablation: ${UROS_ABLATE_599_WIDEN} port 0x80 reads in each gap of the I/O APIC pair, other processors (i386)")
endif()
if(UROS_VM_PAGE_POISON)
    list(APPEND KERNEL_DEFINES -DVM_PAGE_POISON=1)
    message(STATUS "#385 free-page poison: ON (i386)")
endif()

if(UROS_IPC_PORT_TRACK)
    list(APPEND KERNEL_DEFINES -DIPC_PORT_TRACK=1)
    message(STATUS "#485 port allocation tracking: ON (i386)")
endif()

if(UROS_USLOCK_CENSUS)
    list(APPEND KERNEL_DEFINES -DUSLOCK_CENSUS=1)
    message(STATUS "#486 simple-lock census: ON (i386)")
endif()

# MIG-specific flags.  TypeCheck is the argument validation in the generated
# server stubs — see the target_compile_definitions below, which has to carry
# the same value; #443 for why it spent the port at 0.
set(MIG_CFLAGS
    -Dmig_internal=
    -DTypeCheck=1
)

# =============================================================================
# Include directories
# =============================================================================

set(KERNEL_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(KERNEL_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Note: GCC_INCLUDE_DIR is already defined above and added via -isystem in KERNEL_C_FLAGS
# Regular -I include paths should NOT include GCC_INCLUDE_DIR (must use -isystem with -nostdinc)
#
# IMPORTANT: Source directories come FIRST so that original headers (e.g. mach/memory_object.h 
# with type definitions) are found before MIG-generated headers with the same name.
# The build directory is included for generated headers like *_server.h and *_user.c

# ODE-style device config headers for PCI sub-drivers (all disabled).
# The main pci.h lives in i386/AT386/pci.h with NPCI=1.
foreach(_dev_hdr ncr810 ncr825 adsl fta tu)
    if(NOT EXISTS "${KERNEL_SRC_DIR}/i386/AT386/${_dev_hdr}.h")
        file(WRITE "${KERNEL_BUILD_DIR}/${_dev_hdr}.h"
             "#ifndef _CONFIG_${_dev_hdr}_H_\n#define _CONFIG_${_dev_hdr}_H_\n"
             "#define N${_dev_hdr} 0\n#endif\n")
    endif()
endforeach()

set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/uapi			# #179 — UAPI split: single source of truth
    ${UROS_ARCH_INCLUDE}
    ${KERNEL_SRC_DIR}
    ${KERNEL_SRC_DIR}/include
    ${KERNEL_SRC_DIR}/i386
    ${KERNEL_SRC_DIR}/i386/AT386
    ${UROS_CONFIG_INCLUDE}			# #450 — the ODE knobs, generated
    ${KERNEL_BUILD_DIR}
    ${KERNEL_BUILD_DIR}/mach
    ${KERNEL_BUILD_DIR}/mach_debug
    ${KERNEL_BUILD_DIR}/device
)

# =============================================================================
# Step 1: Generate assym.S from genassym.c
#
# genassym.c contains offsetof() expressions that are converted to 
# assembly .set directives. The sed script extracts lines with #DEFINITION#
# =============================================================================

set(GENASSYM_INPUT ${KERNEL_SRC_DIR}/i386/genassym.c)
set(GENASSYM_OUTPUT ${KERNEL_BUILD_DIR}/genassym.s)
set(GENASSYM_DEPFILE ${KERNEL_BUILD_DIR}/genassym.d)
set(ASSYM_OUTPUT ${KERNEL_BUILD_DIR}/assym.S)

# Compile genassym.c to assembly (this produces offset definitions).
# -MD -MF generates a depfile so CMake tracks all transitively included
# headers (kern/thread.h, kern/task.h, ipc/ipc_port.h, etc.) automatically.
# Without this, changes to kernel structs silently produce stale offsets.
add_custom_command(
    OUTPUT ${GENASSYM_OUTPUT}
    DEPFILE ${GENASSYM_DEPFILE}
    COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -S
            -nostdinc
            -ffreestanding
            -isystem ${GCC_INCLUDE_DIR}
            -I${CMAKE_SOURCE_DIR}/uapi
            -I${UROS_ARCH_INCLUDE}
            -I${KERNEL_SRC_DIR}
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            -I${UROS_CONFIG_INCLUDE}
            ${KERNEL_DEFINES}
            -MD -MF ${GENASSYM_DEPFILE}
            -o ${GENASSYM_OUTPUT}
            ${GENASSYM_INPUT}
    DEPENDS ${GENASSYM_INPUT}
    COMMENT "Generating genassym.s from genassym.c"
    VERBATIM
)

# Extract #DEFINITION# lines and convert to assym.S
add_custom_command(
    OUTPUT ${ASSYM_OUTPUT}
    COMMAND sed -e "/#DEFINITION#/!d" 
            -e "s/^.*#DEFINITION#//" 
            -e "s/\\$$//"
            -e "s/\\$\\([0-9-]\\)/\\1/g"
            ${GENASSYM_OUTPUT} > ${ASSYM_OUTPUT}
    DEPENDS ${GENASSYM_OUTPUT}
    COMMENT "Generating assym.S"
    VERBATIM
)

add_custom_target(genassym DEPENDS ${ASSYM_OUTPUT})

# =============================================================================
# Step 2: MIG stub generation
#
# Generate server and user stubs from .defs files
# Server stubs: called by kernel to handle incoming messages
# User stubs: called by kernel to send outgoing messages
# =============================================================================

# migcom is built as part of the parent project (OSFMK_BUILD_TOOLS)
# CMake will handle the dependency automatically via $<TARGET_FILE:migcom>

# Create output directories for MIG files
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/mach)
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/mach_debug)
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/device)

# MIG rules: see cmake/mig.cmake (#416), included from the root CMakeLists.
#
# The i386 include path, exactly as the rules carried it before they moved.
set(UROS_MIG_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${KERNEL_SRC_DIR}
    -I${KERNEL_SRC_DIR}/include
    -I${KERNEL_SRC_DIR}/i386
    -I${KERNEL_SRC_DIR}/i386/AT386
    -I${UROS_CONFIG_INCLUDE})

# Said out loud even though it is migcom's default: relying on a default is
# how the x86-64 rules generated i386 layout without anyone noticing.  ⚠️ It
# is said at the TOP LEVEL now and not here -- see UROS_MIG_TARGET_ARGS and
# UROS_MIG_DEFS_ARCH in uros/CMakeLists.txt -- so that it is also said in the
# directories that never had it.  The output here is unchanged, which is the
# point (#453, #426).


# ---- Mach subsystem MIG files ----

# mach_server.c - kernel receives mach messages
add_mig_server(${UROS_UAPI_DIR}/mach/mach.defs 
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_port_server.c - kernel receives mach_port messages  
add_mig_server(${UROS_UAPI_DIR}/mach/mach_port.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_host_server.c - kernel receives mach_host messages
add_mig_server(${UROS_UAPI_DIR}/mach/mach_host.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_klog_server.c - #200, kernel printf ring drain
add_mig_server(${UROS_UAPI_DIR}/mach/mach_klog.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# clock_server.c - kernel receives clock messages
add_mig_server(${UROS_UAPI_DIR}/mach/clock.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# sync_server.c - kernel receives sync messages  
add_mig_server(${UROS_UAPI_DIR}/mach/sync.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# bootstrap_server.c - kernel receives bootstrap messages
add_mig_server(${UROS_UAPI_DIR}/mach/bootstrap.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# ledger_server.c - kernel receives ledger messages
add_mig_server(${UROS_UAPI_DIR}/mach/ledger.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_i386_server.c - kernel receives i386-specific messages
add_mig_server(${KERNEL_SRC_DIR}/mach/i386/mach_i386.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# exc_user.c - kernel sends exception messages
add_mig_user(${UROS_UAPI_DIR}/mach/exc.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# memory_object_user.c - kernel sends memory object messages
add_mig_user(${UROS_UAPI_DIR}/mach/memory_object.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# device_pager_server.c - generated from memory_object.defs for device pager
# The dev_pager.c file includes this with #defines to rename memory_object_* to device_pager_*
add_custom_command(
    OUTPUT ${KERNEL_BUILD_DIR}/device/device_pager_server.c ${KERNEL_BUILD_DIR}/device/device_pager_server.h
    COMMAND ${CMAKE_C_COMPILER} -E -x c
            -I${UROS_UAPI_DIR}
            -I${UROS_ARCH_INCLUDE}
            -I${KERNEL_SRC_DIR}
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            -I${UROS_CONFIG_INCLUDE}
            ${KERNEL_DEFINES}
            ${UROS_UAPI_DIR}/mach/memory_object.defs
            -o ${KERNEL_BUILD_DIR}/device/device_pager.mig.i
    COMMAND $<TARGET_FILE:migcom>
            -sheader ${KERNEL_BUILD_DIR}/device/device_pager_server.h
            -server ${KERNEL_BUILD_DIR}/device/device_pager_server.c
            -header /dev/null
            -user /dev/null
            ${KERNEL_BUILD_DIR}/device/device_pager.mig.i
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${UROS_UAPI_DIR}/mach/memory_object.defs
    COMMENT "MIG: memory_object -> device_pager (server)"
    VERBATIM
)
# NOTE: device_pager_server.c is NOT added to sources because
# dev_pager.c does #include <device/device_pager_server.c>

# memory_object_default_user.c - kernel sends default pager messages
add_mig_user(${UROS_UAPI_DIR}/mach/memory_object_default.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# clock_reply_user.c - kernel sends clock reply messages
add_mig_user(${UROS_UAPI_DIR}/mach/clock_reply.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# ---- Mach debug subsystem ----
add_mig_server(${UROS_UAPI_DIR}/mach_debug/mach_debug.defs
               ${KERNEL_BUILD_DIR}/mach_debug MACH_DEBUG)

# ---- Device subsystem ----
add_mig_server(${UROS_UAPI_DIR}/device/device.defs
               ${KERNEL_BUILD_DIR}/device DEVICE)

# device_reply_user.c - kernel sends async device replies
add_mig_user(${UROS_UAPI_DIR}/device/device_reply.defs
             ${KERNEL_BUILD_DIR}/device DEVICE)

# ---- Device master subsystem (userspace driver primitives) ----
add_mig_server(${UROS_UAPI_DIR}/device/device_master.defs
               ${KERNEL_BUILD_DIR}/device DEVICE_MASTER)

# Collect all generated MIG sources
set(MIG_GENERATED_SOURCES
    ${MACH_GENERATED}
    ${MACH_DEBUG_GENERATED}
    ${DEVICE_GENERATED}
    ${DEVICE_MASTER_GENERATED}
)

# MIG generates (mach_msg_type_name_t)-1 as a sentinel in static port
# descriptor templates; the value intentionally overflows the 8-bit
# disposition bitfield (truncates to 0xFF).  Suppress the noise.
set_source_files_properties(${MIG_GENERATED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-overflow")

# device_pager_server header needs to exist for ipc_kobject.c
set(DEVICE_PAGER_HEADER ${KERNEL_BUILD_DIR}/device/device_pager_server.h)

# =============================================================================
# Step 3: Source file lists
# =============================================================================

# Core kernel subsystem
set(KERN_SOURCES
    kern/ast.c
    kern/bootstrap.c
    kern/boot_script.c
    kern/lock_smoke.c
    kern/clock.c
    kern/counters.c
    kern/cpu_data.c
    kern/debug.c
    kern/exception.c
    kern/eventcount.c
    kern/etap.c
    kern/fault_profile.c
    kern/syscall_profile.c
    kern/uslock_census.c
    kern/lock.c
    kern/host.c
    kern/ipc_clock.c
    kern/ipc_host.c
    kern/ipc_kobject.c
    kern/ipc_mig.c
    kern/ipc_subsystem.c
    kern/ipc_sync.c
    kern/ipc_tt.c
    kern/kalloc.c
    kern/klog.c
    kern/ledger.c
    kern/mach_clock.c
    kern/mach_factor.c
    kern/machine.c
    kern/norma_bootstrap.c
    kern/printf.c
    kern/priority.c
    kern/processor.c
    kern/queue.c
    kern/rcu.c
    kern/rpc.c
    kern/sched_prim.c
    kern/sscanf.c
    kern/startup.c
    kern/subsystem.c
    kern/sync_lock.c
    kern/sync_policy.c
    kern/sync_sema.c
    kern/syscall_emulation.c
    kern/syscall_subr.c
    kern/syscall_sw.c
    kern/cap.c
    kern/sha256.c
    kern/hmac_sha256.c
    kern/task.c
    kern/task_swap.c
    kern/thread.c
    kern/thread_act.c
    kern/thread_pool.c
    kern/timer.c
    kern/posixtime.c
    kern/thread_swap.c
    kern/version.c
    kern/zalloc.c
)

# IPC subsystem
set(IPC_SOURCES
    ipc/ipc_entry.c
    ipc/ipc_hash.c
    ipc/ipc_init.c
    ipc/ipc_kmsg.c
    ipc/ipc_mqueue.c
    ipc/ipc_notify.c
    ipc/ipc_object.c
    ipc/ipc_port.c
    ipc/ipc_pset.c
    ipc/ipc_right.c
    ipc/ipc_space.c
    ipc/ipc_radix.c
    ipc/ipc_table.c
    ipc/ipc_thread.c
    ipc/mach_msg.c
    ipc/mach_port.c
)

# Virtual Memory subsystem
set(VM_SOURCES
    vm/memory_object.c
    vm/vm_external.c
    vm/vm_fault.c
    vm/vm_init.c
    vm/vm_kern.c
    vm/vm_map.c
    vm/vm_object.c
    vm/vm_pageout.c
    vm/vm_resident.c
    vm/vm_user.c
)

# Device subsystem
set(DEVICE_SOURCES
    device/blkio.c
    device/chario.c
    device/cirbuf.c
    device/dev_lookup.c
    device/dev_name.c
    device/dev_pager.c
    device/device_init.c
    device/device_master.c
    device/driver_lock.c
    device/ds_routines.c
    device/io_scatter.c
    device/net_io.c
    device/subrs.c
    device/data_device.c
    device/net_device.c
    device/test_device.c
)

# Kernel debugger
set(DDB_SOURCES
    ddb/db_access.c
    ddb/db_break.c
    ddb/db_command.c
    ddb/db_cond.c
    ddb/db_examine.c
    ddb/db_expr.c
    ddb/db_ext_symtab.c
    ddb/db_input.c
    ddb/db_lex.c
    ddb/db_macro.c
    ddb/db_output.c
    ddb/db_print.c
    ddb/db_run.c
    ddb/db_sym.c
    ddb/db_task_thread.c
    ddb/db_trap.c
    ddb/db_variables.c
    ddb/db_watch.c
    ddb/db_write_cmd.c
    ddb/db_aout.c
    ddb/db_elf.c
    ddb/tr.c
)

# Intel common code
set(INTEL_SOURCES
    intel/pmap.c
    intel/read_fault.c
)

# i386-specific C sources
set(I386_C_SOURCES
    i386/cpuid.c
    i386/db_disasm.c
    i386/db_interface.c
    i386/db_trace.c
    i386/fpu.c
    i386/fpu_sanity.c
    i386/gdt.c
    i386/hardclock.c
    # #358: HWP (hardware P-states) bring-up -- verdict line + enable-if-off,
    # '-E' EPP performance bias.  CPUID-gated, no-op on KVM.
    i386/hwp.c
    i386/io_emulate.c
    i386/io_map.c
    i386/iopb.c
    i386/nmi_watchdog.c
    i386/ktss.c
    i386/ldt.c
    i386/loose_ends.c
    i386/divide64.c
    i386/net_filter.c
    i386/boot_modules.c
    i386/pcb.c
    i386/phys.c
    i386/kmap.c
    i386/pic.c
    i386/i386_rpc.c
    i386/rtclock.c
    # #560 acceptance: does the switch carry a thread's floating-point
    # state?  Armed by the -F boot argument, inert otherwise.
    i386/fpu_stress.c
    # #599: may a console byte land in COM1's divisor latch?  Armed by the
    # -U boot argument, inert otherwise.
    i386/AT386/com_divisor_test.c
    # The six operations device_master.c needs from the machine (#457).
    # NOT an SMP source: it used to sit in the UROS_NCPUS > 1 block, which
    # left the uniprocessor kernel with nineteen undefined device_md_*
    # symbols at link time.
    i386/device_machdep.c
    i386/sysenter.c
    i386/trap.c
    i386/user_ldt.c
)

# AT386 platform-specific C sources
# NOTE: fd.c removed - floppy driver obsolete in 2026
set(AT386_SOURCES
    i386/AT386/autoconf.c
    i386/AT386/bbclock.c
    i386/AT386/bios_label.c
    i386/AT386/com.c
    i386/AT386/conf.c
    i386/AT386/eisa.c
    i386/AT386/himem.c
    i386/AT386/iopl.c
    # In-kernel keyboard/mouse drivers retired in #208 (kd.c, kd_event.c,
    # kd_mouse.c, kd_queue.c gone; ps2.so under char_server owns the
    # keyboard, a future ps2_mouse.so will own the mouse).
    i386/AT386/console_cdev.c
    i386/AT386/ddb_kbd.c
    # Emergency in-kernel framebuffer console (#342): the ONLY way to see
    # boot/panic output on pure-UEFI machines (no serial, no legacy 0xB8000).
    # Not a display driver — the userspace gpu_server still owns the screen.
    i386/AT386/fbcons.c
    i386/AT386/model_dep.c
    i386/AT386/physmem.c
    i386/AT386/pic_isa.c
    # #357: idle HLT (machine_idle + wake doorbell; POWER_SAVE=1 in
    # power_save.h, -S boot flag reverts to the legacy poll idle).
    i386/AT386/power_save.c
    # In-kernel VGA driver removed in #199 (vga.c, vga.h, vgareg.h,
    # vga_entries.h gone; userspace gpu_server owns the display).
)

# #300 — SMP bring-up sources, only when UROS_NCPUS > 1.
# mp.c is the high-level shim (slave_clock, cause_ast_check, cpu_int_word[]).
# mp_stub.c provides the link-time symbols (cpu_start, mp_probe_cpus, ...)
# that mp_v1_1.c would have hosted.  The historical mp_v1_1.c is kept out
# of the build until Increment 3 replaces it with a modern MPS 1.4 parser.
# slave_boot.S (added below to AT386_ASM_SOURCES) is the AP trampoline.
if(UROS_NCPUS GREATER 1)
    list(APPEND AT386_SOURCES
        i386/mp_desc.c
        i386/lapic.c
        i386/clock_watch.c
        i386/ioapic.c
        i386/AT386/mp/mp.c
        i386/AT386/mp/mp_stub.c
        i386/AT386/mp/mp_table.c
    )
endif()

# #309: per-CPU SSE/FPU sanity check is useful both on UP (cheap
# baseline) and SMP (proves AP CR4/FPU init worked).  Goes into the
# common i386 source list.

# Chips/busses
set(CHIPS_SOURCES
    chips/busses.c
)

# PCI subsystem
set(PCI_SOURCES
    busses/pci/pci.c
    i386/pci/pcibios.c
    i386/pci/i386_pci.c
    i386/pci/pci_config.c
)

# SCSI subsystem
set(SCSI_SOURCES
    # scsi/scsi_info.c  # Disabled - too many dependencies
    device/device_stubs.c  # Stub implementations for missing device functions
)

# All C sources
set(KERNEL_C_SOURCES
    ${KERN_SOURCES}
    ${IPC_SOURCES}
    ${VM_SOURCES}
    ${DEVICE_SOURCES}
    ${DDB_SOURCES}
    ${INTEL_SOURCES}
    ${I386_C_SOURCES}
    ${AT386_SOURCES}
    ${CHIPS_SOURCES}
    ${PCI_SOURCES}
    ${SCSI_SOURCES}
)

# =============================================================================
# Step 4: Assembly sources
#
# The kernel has a special locore.S that is built from concatenating:
#   assym.S + start.S + locore.S + cswitch.S
# =============================================================================

# Individual assembly files (not part of locore)
set(I386_ASM_SOURCES
    i386/_setjmp.S
    i386/bcopy.S
    i386/bzero.S
    i386/divide.S
    # #560: the registers are written by hand, which is the point of it.
    i386/fpu_stress.S
    i386/i386_lock.S
    i386/idt.S
    i386/interrupt.S
    i386/ntoh.S
    i386/spl.S
)

set(AT386_ASM_SOURCES
    i386/AT386/kdasm.S
)

# #300 — AP trampoline (real-mode -> 32-bit PM) when UROS_NCPUS > 1.
# #302 — IPI entry stubs (3 vectors).
if(UROS_NCPUS GREATER 1)
    list(APPEND AT386_ASM_SOURCES
        i386/AT386/mp/slave_boot.S
        i386/ipi.S
    )
endif()

# locore.S is special - concatenated from multiple files
set(LOCORE_DEPS
    ${ASSYM_OUTPUT}
    ${KERNEL_SRC_DIR}/i386/start.S
    ${KERNEL_SRC_DIR}/i386/locore.S
    ${KERNEL_SRC_DIR}/i386/cswitch.S
)

set(LOCORE_OUTPUT ${KERNEL_BUILD_DIR}/locore.S)

add_custom_command(
    OUTPUT ${LOCORE_OUTPUT}
    COMMAND cat ${LOCORE_DEPS} > ${LOCORE_OUTPUT}
    DEPENDS ${LOCORE_DEPS}
    COMMENT "Generating locore.S (concatenated startup code)"
    VERBATIM
)

add_custom_target(locore DEPENDS ${LOCORE_OUTPUT})

# =============================================================================
# Step 5: Build kernel executable
# =============================================================================

# Prefix source paths
list(TRANSFORM KERNEL_C_SOURCES PREPEND ${KERNEL_SRC_DIR}/)
list(TRANSFORM I386_ASM_SOURCES PREPEND ${KERNEL_SRC_DIR}/)
list(TRANSFORM AT386_ASM_SOURCES PREPEND ${KERNEL_SRC_DIR}/)

# Generate an empty mach_interface.h stub in the kernel build directory.
# Needed because migcom emits #include <mach/mach_interface.h> in user stubs;
# the kernel doesn't need its contents.  This file lives ONLY in the build dir
# so it doesn't shadow the real generated/include/mach/mach_interface.h
# that the userspace build uses.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mach/mach_interface.h"
    "#ifndef _MACH_MACH_INTERFACE_H_\n"
    "#define _MACH_MACH_INTERFACE_H_\n"
    "/* nothing needed here for in-kernel MIG user stubs */\n"
    "#endif\n"
)

# Create kernel executable
add_executable(${KERNEL_TARGET}
    ${KERNEL_C_SOURCES}
    ${I386_ASM_SOURCES}
    ${AT386_ASM_SOURCES}
    ${LOCORE_OUTPUT}
    ${MIG_GENERATED_SOURCES}
    ${DEVICE_PAGER_HEADER}
)

# Ensure dependencies are built first
add_dependencies(${KERNEL_TARGET} genassym locore)

# Set compile options
target_compile_options(${KERNEL_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${KERNEL_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:ASM>:${KERNEL_ASM_FLAGS}>
)

target_compile_definitions(${KERNEL_TARGET} PRIVATE
    ${KERNEL_DEFINES}
    # MIG server stubs validate the message they are handed; this compiles
    # those blocks in.  It has to be set in both places or it is not set at
    # all: MIG_CFLAGS above reaches the stubs migcom generates, this one
    # reaches everything else the kernel target compiles.
    #
    # #443: it was 0 from the port until 2026-07-30, inherited from the ODE
    # build and then written into this file by d443de8a for a real reason —
    # task_create failed MIG_TYPE_ERROR because ipc_kmsg_copyin did not
    # translate the OOL_PORTS disposition at count==0.  That bug was fixed in
    # the same commit and the flag was never turned back on, so the other 313
    # blocks were never observed either way.  They are now: 314 checks, 4 CPUs,
    # boot + cap/sig/pthread + `ipc_bench --bench all`, nothing fires.
    #
    # Cost, measured rather than argued (#443 part B): below what the
    # instrument can resolve.  The same kernel compared against itself reports
    # -2.4%, the two kernels report +2.8% — the floor is +-3% and the effect is
    # under it.  On task-to-task IPC the cost is zero by construction, since
    # ipc_mqueue_send only runs a server stub when the destination is the
    # kernel's own space.
    TypeCheck=1
    mig_internal=
)

# Include directories
target_include_directories(${KERNEL_TARGET} PRIVATE
    ${KERNEL_INCLUDES}
)

# Linker options
set(KERNEL_LINKER_SCRIPT ${KERNEL_SRC_DIR}/i386/AT386/kernel.ld)

target_link_options(${KERNEL_TARGET} PRIVATE
    ${UROS_ARCH_FLAGS}
    -static
    -nostdlib
    -nostartfiles
    -no-pie
    -T ${KERNEL_LINKER_SCRIPT}
    -Wl,--build-id=none
)

set_target_properties(${KERNEL_TARGET} PROPERTIES
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
    OUTPUT_NAME "mach_kernel"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/uros/boot"
)

# =============================================================================
# Issue #211 — DDB symbol table (debug builds only).
#
# .symtab/.strtab are non-allocated ELF sections, so the multiboot loader
# does not load them.  Generate a self-describing binary (ksyms.bin) from
# the linked kernel and ship it as a separate multiboot module
# (run-qemu.sh appends it to -initrd if present).  The kernel locates
# the module by its magic at boot and feeds it to DDB via db_elf.c.
#
# OSFMK_GEN_KSYMS defaults ON for Debug, OFF for Release — see the
# top-level CMakeLists.  When OFF, the file is simply not produced;
# run-qemu.sh boots without DDB symbol-by-name lookup, exactly as the
# pre-#211 behaviour.
# =============================================================================
# #560: no floating-point instruction in this kernel outside the symbols that
# declare one.  It matters more here than on x86-64: -mno-sse stops the
# compiler reaching for SSE and stops nothing reaching for x87, which is
# i386's default floating point -- and _doprnt_ext held 124 of them, run in
# whatever thread had called printf, overwriting that thread's live registers.
#
# Same script as the x86-64 arm, and for the same reason as #549: a check
# nobody runs is a check that is not there.  This one fails the link that
# produced the binary it read.
add_custom_command(TARGET ${KERNEL_TARGET} POST_BUILD
  COMMAND ${CMAKE_SOURCE_DIR}/../scripts/kernel-vector-check.py
          $<TARGET_FILE:${KERNEL_TARGET}>
  COMMENT "#560: checking no undeclared kernel code uses floating point"
  VERBATIM)

if(OSFMK_GEN_KSYMS)
    set(KERNEL_KSYMS "${CMAKE_BINARY_DIR}/export/uros/boot/ksyms.bin")
    add_custom_command(
        OUTPUT ${KERNEL_KSYMS}
        COMMAND ${CMAKE_SOURCE_DIR}/../scripts/gen-ksyms.py
                $<TARGET_FILE:${KERNEL_TARGET}> ${KERNEL_KSYMS}
        DEPENDS ${KERNEL_TARGET} ${CMAKE_SOURCE_DIR}/../scripts/gen-ksyms.py
        COMMENT "Generating kernel ksyms.bin (DDB symbol table, #211)"
        VERBATIM
    )
    add_custom_target(mach_kernel_ksyms ALL DEPENDS ${KERNEL_KSYMS})

    # Stale ksyms.bin from a previous Debug build would silently mismatch
    # the current kernel — wipe it on Release reconfigures.
else()
    file(REMOVE "${CMAKE_BINARY_DIR}/export/uros/boot/ksyms.bin")
endif()

# =============================================================================
# Info
# =============================================================================

message(STATUS "Configuring Mach Kernel for i386/AT386")
message(STATUS "  Load address: ${KERNEL_TEXTORG}")
message(STATUS "  Source dir: ${KERNEL_SRC_DIR}")
message(STATUS "  Build dir: ${KERNEL_BUILD_DIR}")

# =============================================================================
# Flat-model verification tests (#47/#51)
# =============================================================================
set(KERNEL_BIN "$<TARGET_FILE:${KERNEL_TARGET}>")

# --- ELF property checks ---

# 1. Kernel must be a 32-bit ELF
add_test(NAME kernel_elf32
  COMMAND sh -c "LANG=C readelf -h '${KERNEL_BIN}' | grep -q 'Class:.*ELF32'"
)

# 2. Entry point must be in kernel virtual space (0xC0xxxxxx)
add_test(NAME kernel_entry_in_kernel_space
  COMMAND sh -c "LANG=C readelf -h '${KERNEL_BIN}' | grep 'Entry point' | grep -q '0xc0'"
)

# 3. .text virtual address must start at 0xC0100000+ (KERNELBASE + 1MB)
add_test(NAME kernel_text_vaddr
  COMMAND sh -c "LANG=C readelf -S '${KERNEL_BIN}' | grep '\\.text' | awk 'BEGIN{r=1} {for(i=1;i<=NF;i++) if(\$i ~ /^c010/) r=0} END{exit r}'"
)

# 4. No LINEAR_KERNEL_ADDRESS symbol in kernel (legacy removed)
add_test(NAME kernel_no_linear_kernel_addr_symbol
  COMMAND sh -c "! LANG=C readelf -s '${KERNEL_BIN}' 2>/dev/null | grep -qi 'LINEAR_KERNEL'"
)

# --- QEMU boot smoke test ---
find_program(QEMU_I386 qemu-system-i386)
if(QEMU_I386)
  add_test(NAME kernel_boot_smoke
    COMMAND sh -c "
      LOG=\$(mktemp)
      ${QEMU_I386} -m 128 -no-reboot -display none \
        -serial file:\$LOG \
        -kernel '${KERNEL_BIN}' &
      QPID=\$!
      sleep 8
      kill \$QPID 2>/dev/null; wait \$QPID 2>/dev/null
      PASS=true
      grep -q 'OSFMK'           \$LOG || PASS=false
      grep -q 'sysenter.*fast'  \$LOG || PASS=false
      grep -q 'vm_page_bootstrap' \$LOG || PASS=false
      if grep -q 'panic.*freed zone' \$LOG; then PASS=false; fi
      rm -f \$LOG
      \$PASS
    "
  )
  set_tests_properties(kernel_boot_smoke PROPERTIES
    TIMEOUT 30
    LABELS "boot;smoke"
  )
endif()
