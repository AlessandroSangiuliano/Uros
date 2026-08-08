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
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ddb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/cont_probe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/disasm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ksym.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/desc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/idle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ipi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ioapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/lapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/conf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/machdep.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/percpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/pic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/startup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/spl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/smp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/bootmem.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/pit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/tsc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/clock_event.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/preempt_test.c
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
      NCPUS=64 MACH_ASSERT=1 MACH_DEBUG=1 MACH_KDB=0 MACH_DEBUGGER=1 MACH_HOST=1
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
    ${CMAKE_SOURCE_DIR}/export/include)			# #450 — the ODE knobs, generated
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
  add_custom_target(uros_iso
    COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_64_ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mach_kernel>
            ${X86_64_ISO_DIR}/boot/mach_kernel
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
            ${X86_64_ISO_DIR}/boot/grub/grub.cfg
    COMMAND sh -c "echo set default=$\{UROS_X86_64_BOOT_ENTRY:-0\} > ${X86_64_ISO_DIR}/boot/grub/entry.cfg"
    COMMAND grub-mkrescue -o ${X86_64_ISO} ${X86_64_ISO_DIR}
    DEPENDS mach_kernel ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
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

  # Which machine migcom is generating for.  Not optional: it defaults to
  # i386, so a missing flag produces i386 message layout silently -- 12-byte
  # port descriptors where this target's are 16 (#453).
  set(UROS_MIG_TARGET_ARGS -target x86_64)
  set(UROS_MIG_DEFS_ARCH -DMIG_ARCH_X86_64=1)

  # No AT386, no EISA/HIMEM/NPCI/MP_V1_1: those name i386 hardware, and the
  # generated code is the wire format, which must not depend on them.

  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/mach.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/mach_port.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/mach_host.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/mach_klog.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/clock.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/sync.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/bootstrap.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach/ledger.defs
                 ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/mach_debug/mach_debug.defs
                 ${X86_64_MIG_DIR}/mach_debug X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/device/device.defs
                 ${X86_64_MIG_DIR}/device X86_64_MIG)
  add_mig_server(${CMAKE_CURRENT_SOURCE_DIR}/device/device_master.defs
                 ${X86_64_MIG_DIR}/device X86_64_MIG)
  add_mig_user(${CMAKE_CURRENT_SOURCE_DIR}/mach/exc.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${CMAKE_CURRENT_SOURCE_DIR}/mach/memory_object.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${CMAKE_CURRENT_SOURCE_DIR}/mach/memory_object_default.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${CMAKE_CURRENT_SOURCE_DIR}/mach/clock_reply.defs
               ${X86_64_MIG_DIR}/mach X86_64_MIG)
  add_mig_user(${CMAKE_CURRENT_SOURCE_DIR}/device/device_reply.defs
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
            ${CMAKE_CURRENT_SOURCE_DIR}/mach/memory_object.defs |
            $<TARGET_FILE:migcom>
            ${UROS_MIG_TARGET_ARGS}
            -sheader ${X86_64_MIG_DIR}/device/device_pager_server.h
            -server ${X86_64_MIG_DIR}/device/device_pager_server.c
            -header /dev/null
            -user /dev/null
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS}
            ${CMAKE_CURRENT_SOURCE_DIR}/mach/memory_object.defs
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
  #   device/device_master.c the fourteen RPCs a user-space driver reaches
  #                          hardware through.  It is i386 code living in
  #                          device/ -- <i386/ipl.h>, <i386/pio.h>,
  #                          <i386/ioapic.h> outside any conditional -- and
  #                          almost every one of its RPCs changes mechanism
  #                          here rather than merely widening: ECAM instead of
  #                          ports 0xCF8/0xCFC, our own IOAPIC and MSI-X, and
  #                          IOMMU addresses out of the DMA calls.  A rewrite,
  #                          behind MACH_DEVICE_MASTER, which is 0 here.
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
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "device_master_server\\.c$")
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "/device_server\\.c$")
  list(FILTER X86_64_MIG_C EXCLUDE REGEX "device_reply_user\\.c$")
  target_sources(mach_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/sync/mutex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/blkio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/chario.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/cirbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/data_device.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_lookup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_name.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/dev_pager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/device/device_init.c
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
    # -Wno-format is gone too, and removing it changes nothing today: this
    # kernel builds -fno-builtin with its own printf, db_printf and kprintf,
    # none of which carries format(printf,...), so there was never anything
    # for the warning to check.  It was guarding an empty room.  Annotating
    # the print functions is what makes it real, and that is its own step --
    # %x against a vm_offset_t is the thing the issue asks for.
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
    -DMACH_ASSERT=1
    -DMACH_DEBUG=1
    -DMACH_KDB=1
    -DMACH_HOST=1
    -DKERNEL_PRIVATE
    -DSTAT_TIME
    -DKERNEL_FLOAT_OK=1
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
    # Public Uros export headers (LAST so kernel-internal mach/ wins on
    # duplicates).  Lets the kernel pull header-only public headers like
    # <mach/urmach_version.h> from a single source of truth.
    ${CMAKE_SOURCE_DIR}/export/include
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

# Said out loud even though it is migcom's default.  Relying on a default is
# how the x86-64 rules generated i386 layout without anyone noticing; the
# output here is unchanged, which is the point -- the flag states what was
# already true instead of leaving it to be inferred (#453).
set(UROS_MIG_TARGET_ARGS -target i386)
set(UROS_MIG_DEFS_ARCH -DMIG_ARCH_I386=1)


# ---- Mach subsystem MIG files ----

# mach_server.c - kernel receives mach messages
add_mig_server(${KERNEL_SRC_DIR}/mach/mach.defs 
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_port_server.c - kernel receives mach_port messages  
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_port.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_host_server.c - kernel receives mach_host messages
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_host.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_klog_server.c - #200, kernel printf ring drain
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_klog.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# clock_server.c - kernel receives clock messages
add_mig_server(${KERNEL_SRC_DIR}/mach/clock.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# sync_server.c - kernel receives sync messages  
add_mig_server(${KERNEL_SRC_DIR}/mach/sync.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# bootstrap_server.c - kernel receives bootstrap messages
add_mig_server(${KERNEL_SRC_DIR}/mach/bootstrap.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# ledger_server.c - kernel receives ledger messages
add_mig_server(${KERNEL_SRC_DIR}/mach/ledger.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_i386_server.c - kernel receives i386-specific messages
add_mig_server(${KERNEL_SRC_DIR}/mach/i386/mach_i386.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# exc_user.c - kernel sends exception messages
add_mig_user(${KERNEL_SRC_DIR}/mach/exc.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# memory_object_user.c - kernel sends memory object messages
add_mig_user(${KERNEL_SRC_DIR}/mach/memory_object.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# device_pager_server.c - generated from memory_object.defs for device pager
# The dev_pager.c file includes this with #defines to rename memory_object_* to device_pager_*
add_custom_command(
    OUTPUT ${KERNEL_BUILD_DIR}/device/device_pager_server.c ${KERNEL_BUILD_DIR}/device/device_pager_server.h
    COMMAND ${CMAKE_C_COMPILER} -E -x c
            -I${UROS_ARCH_INCLUDE}
            -I${KERNEL_SRC_DIR} 
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            -I${UROS_CONFIG_INCLUDE}
            ${KERNEL_DEFINES}
            ${KERNEL_SRC_DIR}/mach/memory_object.defs |
            $<TARGET_FILE:migcom>
            -sheader ${KERNEL_BUILD_DIR}/device/device_pager_server.h
            -server ${KERNEL_BUILD_DIR}/device/device_pager_server.c
            -header /dev/null
            -user /dev/null
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${KERNEL_SRC_DIR}/mach/memory_object.defs
    COMMENT "MIG: memory_object -> device_pager (server)"
    VERBATIM
)
# NOTE: device_pager_server.c is NOT added to sources because
# dev_pager.c does #include <device/device_pager_server.c>

# memory_object_default_user.c - kernel sends default pager messages
add_mig_user(${KERNEL_SRC_DIR}/mach/memory_object_default.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# clock_reply_user.c - kernel sends clock reply messages
add_mig_user(${KERNEL_SRC_DIR}/mach/clock_reply.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# ---- Mach debug subsystem ----
add_mig_server(${KERNEL_SRC_DIR}/mach_debug/mach_debug.defs
               ${KERNEL_BUILD_DIR}/mach_debug MACH_DEBUG)

# ---- Device subsystem ----
add_mig_server(${KERNEL_SRC_DIR}/device/device.defs
               ${KERNEL_BUILD_DIR}/device DEVICE)

# device_reply_user.c - kernel sends async device replies
add_mig_user(${KERNEL_SRC_DIR}/device/device_reply.defs
             ${KERNEL_BUILD_DIR}/device DEVICE)

# ---- Device master subsystem (userspace driver primitives) ----
add_mig_server(${KERNEL_SRC_DIR}/device/device_master.defs
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
    i386/net_filter.c
    i386/boot_modules.c
    i386/pcb.c
    i386/phys.c
    i386/kmap.c
    i386/pic.c
    i386/i386_rpc.c
    i386/rtclock.c
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
    i386/gcc.S
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
# so it doesn't shadow the real export/include/mach/mach_interface.h
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
