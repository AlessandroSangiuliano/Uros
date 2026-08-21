# Kernel build configuration -- the ODE option knobs, owned by the build.
#
# #450.  The machine-independent tree asks for these by #include: <cpus.h>,
# <mach_rt.h>, <norma_vm.h> and 50-odd more, then tests them with #if.  They
# are a name and a number each -- build configuration, not code.
#
# They used to be tracked files under i386/AT386/, written by
# scripts/generate_ode_headers.py from the heritage conf/ files.  That left
# two sources of truth, and they had drifted: the tracked headers had been
# edited by hand to match a kernel whose drivers moved to userspace (VGA
# deleted in #199, the ISA cards switched off, PCI switched on), while the
# conf/ files still described a 1990s AT386.  Running the generator turned
# PCI off and three obsolete drivers back on -- a build-breaking edit that
# looked like housekeeping.
#
# So the second copy is gone rather than resynchronised.  The values live
# here, next to the -D list that already carried the ones that mattered, and
# the headers are generated into the build directory.  Nothing tracks them,
# so nothing can drift from them.
#
# Adding a knob: one line below.  Changing one: change it here.  There is no
# other place, and that is the point.

# ── Which KIND of kernel this is (#485) ─────────────────────────────────────
#
# 🔑 Optimisation level and assertion policy are two different axes, and
# conflating them in one name is what made this question unanswerable for as
# long as it was.  What this tree builds every day is CMAKE_BUILD_TYPE=Release
# with -O3 -DNDEBUG -- and it is not a release kernel.  It is a development
# kernel that runs fast, which is what measuring anything requires.
#
# That mismatch is why "MACH_ASSERT is on in Release" went unnoticed: what is
# called Release is not release.  So the kind of kernel gets a name of its own
# and the build says which one it is on every configure, rather than leaving a
# reader to infer it from an optimisation flag that answers a different
# question.
#
#   development  assertions on.  Checks that name their cause are worth more
#                than the cycles they cost, because the cause is what is being
#                looked for.  This is what we build.
#   release      assertions off.  panic() and every always-on integrity check
#                remain -- they are not tied to this switch -- so a release
#                kernel still stops and still reports; it reports at the
#                consequence rather than at the cause.
#
# ⚠️ Nothing ships yet, so `release\' exists to be BUILDABLE and measured, not
# because anything is being released.  #485 made it buildable at all; before
# that it had never once compiled.
set(UROS_KERNEL_FLAVOR "development" CACHE STRING
    "Which kind of kernel: development (assertions on) or release (off)")
set_property(CACHE UROS_KERNEL_FLAVOR PROPERTY STRINGS development release)

if(UROS_KERNEL_FLAVOR STREQUAL "release")
    set(_uros_assert_default OFF)
else()
    set(_uros_assert_default ON)
endif()

# 🔥 The flavour has to MOVE the switch, and option() does not: it leaves an
# existing cache entry alone, so the first version of this printed
# "assertions OFF" while <mach_assert.h> carried MACH_ASSERT 1.  A message
# announcing one thing while the compile does another is the exact defect this
# issue is about, reproduced inside its own fix.
#
# So the last flavour is remembered, and a change to it forces the derived
# switch.  An explicit -DUROS_MACH_ASSERT= on the same command line still wins,
# because it is set after this runs; what is gone is the silent disagreement.
if(NOT "${UROS_KERNEL_FLAVOR}" STREQUAL "${UROS_KERNEL_FLAVOR_LAST}")
    set(UROS_MACH_ASSERT ${_uros_assert_default} CACHE BOOL
        "Build the kernel's assert() calls (MACH_ASSERT)" FORCE)
    set(UROS_KERNEL_FLAVOR_LAST "${UROS_KERNEL_FLAVOR}" CACHE INTERNAL
        "flavour this cache was last configured for")
endif()

# ── The one knob with a switch of its own (#485) ────────────────────────────
#
# MACH_ASSERT decides whether every assert() in the kernel is a comparison and
# a branch or is ((void)0).  It was set in TWO places -- this table and each
# target's -D list, at the same value -- so it could not be turned off from
# either: clearing one left the other standing, and clearing the -D left this
# table's default, which the generated header writes back.
#
# 🔥 That is not a tidiness complaint.  #482 measured a single site under this
# switch at 44% of a copy-on-write fault (the #385 free-page poison, a whole
# page read on every allocation, for a hunt that closed months ago), and
# gating it took the fault from 5,790 cycles to 3,390.  The switch has to be
# operable before what it switches can be decided.
#
# Now: one option, feeding the table, feeding the header.  The -D is gone from
# both targets, which is safe and was checked rather than assumed -- every
# compiled unit that tests MACH_ASSERT also includes <mach_assert.h>, 25 of 25
# on i386 and 22 of 22 on x86-64, per scripts/assert-census.py --config.
#
# ⚠️ ON is still the default, and #485 has not decided otherwise.  What this
# buys today is that the question can be ASKED of a build.
option(UROS_MACH_ASSERT "Build the kernel's assert() calls (MACH_ASSERT)" ${_uros_assert_default})

# ⚠️ Printed AFTER the value settles, and it prints the value that will be
# compiled -- not the one the flavour would imply.  The two can differ, when
# somebody overrides the switch on purpose, and the build saying which is which
# is the whole point.
message(STATUS "UrMach kernel flavor: ${UROS_KERNEL_FLAVOR}, "
               "MACH_ASSERT=${UROS_MACH_ASSERT} "
               "(CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} is the optimisation "
               "axis, a different question)")

if(UROS_MACH_ASSERT)
    set(UROS_MACH_ASSERT_VALUE 1)
else()
    set(UROS_MACH_ASSERT_VALUE 0)
    message(STATUS "#485 MACH_ASSERT: OFF — assert() compiles to nothing")
endif()

# name              macro                 value
set(UROS_KERNEL_CONFIG
    advisory_pageout       ADVISORY_PAGEOUT 1
    bootstrap_symbols      BOOTSTRAP_SYMBOLS 1
    cdli                   NCDLI 0
    chained_ios            CHAINED_IOS 1
    db_machine_commands    DB_MACHINE_COMMANDS 0
    dipc                   DIPC 0
    dipc_xkern             DIPC_XKERN 0
    etap_event_monitor     ETAP_EVENT_MONITOR 0
    etap_lock_accumulate   ETAP_LOCK_ACCUMULATE 0
    etap_lock_monitor      ETAP_LOCK_MONITOR 0
    fast_idle              FAST_IDLE 0
    fast_tas               FAST_TAS 0
    fddi                   FDDI 0
    gprof                  NGPROF 0
    hw_footprint           HW_FOOTPRINT 1
    kernel_test            KERNEL_TEST 0
    mach_assert            MACH_ASSERT ${UROS_MACH_ASSERT_VALUE}
    mach_cluster_stats     MACH_CLUSTER_STATS 0
    mach_counters          MACH_COUNTERS 0
    mach_debug             MACH_DEBUG 1
    mach_device_master     MACH_DEVICE_MASTER 1
    mach_host              MACH_HOST 1
    mach_ipc_debug         MACH_IPC_DEBUG 0
    mach_ipc_test          MACH_IPC_TEST 0
    mach_kdb               MACH_KDB 1
    mach_kgdb              MACH_KGDB 0
    mach_kprof             MACH_KPROF 0
    mach_lock_mon          MACH_LOCK_MON 0
    mach_machine_routines  MACH_MACHINE_ROUTINES 1
    mach_net_in_kernel     MACH_NET_IN_KERNEL 1
    mach_pagemap           MACH_PAGEMAP 1
    mach_prof              MACH_PROF 1
    mach_rt                MACH_RT 0
    mach_tr                MACH_TR 1
    mach_vm_debug          MACH_VM_DEBUG 0
    mp_v1_1                MP_V1_1 0
    norma_device           NORMA_DEVICE 0
    norma_ether            NORMA_ETHER 0
    norma_scsi             NORMA_SCSI 0
    norma_task             NORMA_TASK 0
    norma_vm               NORMA_VM 0
    pci                    NPCI 1
    sce                    NSCE 0
    simple_clock           SIMPLE_CLOCK 0
    stack_usage            STACK_USAGE 0
    stat_time              STAT_TIME 1
    task_swapper           TASK_SWAPPER 1
    test_device            NTEST_DEVICE 1
    thread_swapper         THREAD_SWAPPER 1
    time_stamp             TIME_STAMP 0
    vm_cpm                 VM_CPM 0
    xk_debug               XK_DEBUG 0
    xk_proxy               XK_PROXY 0
    xkmachkernel           XKMACHKERNEL 0
    xpr_debug              XPR_DEBUG 0
    zone_debug             ZONE_DEBUG 0
)

# The platform identity, per target.  This is the one configuration value that
# is genuinely not shared: seven places in the MI tree ask `#ifdef i386` or
# `#ifdef AT386` and take a different branch.  On x86-64 they must take the
# other branch -- whether the other branch is right there is a question for
# the port, and it can only be asked once the header stops saying i386.
set(UROS_PLATFORM_i386   AT386 i386)
set(UROS_PLATFORM_x86_64 X86PC x86_64)

# uros_write_config_headers(<dir> <arch>)
#
# Emit one header per entry, plus the handful that have a shape of their own.
# Regenerating on every configure is cheap and keeps the directory honest;
# file(GENERATE) would leave stale headers behind when an entry is removed,
# which is the failure this whole change is about.
#
# Every value is written under #ifndef, so a -D on the command line wins.
# Some of these knobs are also set there -- MP_V1_1 and NCPUS follow
# UROS_NCPUS, TypeCheck follows its own option -- and a header that fights a
# deliberate override is a bug rather than a default.  Two of the tracked
# headers had learned this and carried the guard; the rest had not, and the
# difference was invisible until the values disagreed.  MP_V1_1 was the one
# that did: forced to 0, cpu_number() falls off the APIC path and the build
# stops at "#cpus <= 8".
function(uros_write_config_headers outdir arch)
  file(REMOVE_RECURSE ${outdir})
  file(MAKE_DIRECTORY ${outdir})
  list(LENGTH UROS_KERNEL_CONFIG _n)
  math(EXPR _last "${_n} / 3 - 1")
  foreach(_i RANGE ${_last})
    math(EXPR _h "${_i} * 3")
    math(EXPR _m "${_h} + 1")
    math(EXPR _v "${_h} + 2")
    list(GET UROS_KERNEL_CONFIG ${_h} _hdr)
    list(GET UROS_KERNEL_CONFIG ${_m} _macro)
    list(GET UROS_KERNEL_CONFIG ${_v} _val)
    string(TOUPPER "${_hdr}" _guard)
    file(WRITE ${outdir}/${_hdr}.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_${_guard}_H_
#define _UROS_CONFIG_${_guard}_H_

#ifndef ${_macro}
#define ${_macro} ${_val}
#endif

#endif /* _UROS_CONFIG_${_guard}_H_ */
")
  endforeach()

  # mach_machine_routines -- the one knob whose value is not the same on every
  # machine, and it has to be a rewrite rather than a table entry because the
  # table above is shared (#453).
  #
  # It says whether this machine adds RPCs of its own to the kernel's MIG
  # dispatch: kern/ipc_kobject.c includes <machine/machine_routines.h> and
  # registers MACHINE_SUBSYSTEM when it is on.  i386's are five -- the I/O
  # permission bitmap (i386_io_port_add/remove/list) and the LDT
  # (i386_set_ldt/i386_get_ldt).
  #
  # x86-64 has none of them.  The LDT does not exist on this machine at all,
  # and an I/O permission interface here would be a new one designed against
  # #448, not a port of that one.  So the knob is off, and turning it on is
  # what the first such RPC will do.
  # mach_net_in_kernel -- whether the network stack lives in the kernel.
  #
  # i386's does: device/net_io.c is fed by chips/lance.c, i386/kkt/if_kkt.c,
  # scsi/if_sce.c and device/cdli.c, all of them in-kernel drivers.  x86-64
  # has none and will have none: network cards belong to user-space servers,
  # with the IOMMU making the isolation real, and the TCP/IP stack is a
  # server too.  So net_io.c has no producers here and the three hooks it
  # planted in the core -- net_ast, net_kmsg_collect, net_kmsg_put -- have
  # nothing to call (#453).
  if(NOT arch STREQUAL "i386")
    file(WRITE ${outdir}/mach_net_in_kernel.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_MACH_NET_IN_KERNEL_H_
#define _UROS_CONFIG_MACH_NET_IN_KERNEL_H_

#ifndef MACH_NET_IN_KERNEL
#define MACH_NET_IN_KERNEL 0
#endif

#endif /* _UROS_CONFIG_MACH_NET_IN_KERNEL_H_ */
")
  endif()

  # mach_device_master -- the fourteen RPCs a user-space driver reaches the
  # hardware through: PCI config space, interrupt forwarding, DMA buffers,
  # MMIO windows, port I/O.
  #
  # ⚠️ Off here because device/device_master.c is i386 code that happens to
  # live in device/: it includes <i386/ipl.h>, <i386/pio.h>, <i386/ioapic.h>
  # and <i386/misc_protos.h> outside any conditional.  That is not an
  # oversight to patch around -- almost every one of those RPCs changes
  # mechanism on this machine rather than merely widening.  Config space is
  # ECAM, not ports 0xCF8/0xCFC, which cannot address past bus 255 at all;
  # interrupts come from our own IOAPIC and MSI-X; and the DMA calls must hand
  # out IOMMU addresses (#432) or the isolation this whole design claims is
  # not true.  It is a rewrite, and it has its own issue.
  #
  # With the knob off the subsystem is absent rather than faked: it is not in
  # ipc_kobject.c's mig_e[] table, so a client that calls it gets MIG_BAD_ID.
  # That is the accurate answer -- this kernel does not implement it -- where
  # fourteen entry points returning a plausible code would be a lie a driver
  # could act on.
  if(NOT arch STREQUAL "i386")
    file(WRITE ${outdir}/mach_device_master.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_MACH_DEVICE_MASTER_H_
#define _UROS_CONFIG_MACH_DEVICE_MASTER_H_

#ifndef MACH_DEVICE_MASTER
#define MACH_DEVICE_MASTER 0
#endif

#endif /* _UROS_CONFIG_MACH_DEVICE_MASTER_H_ */
")
  endif()

  # mach_tr -- the circular trace buffer in ddb/tr.c.
  #
  # ⚠️ Off here because the file that implements it is not built.  The buffer
  # is a debugger facility: <ddb/tr.h> pulls in <machine/db_machdep.h> when
  # TRACE_BUFFER is on, and db_show_tr() is how the trace is ever read back.
  # This machine has no DDB yet (#428), and MACH_KDB is 0 here for the same
  # reason.
  #
  # Claiming it would be the same defect as claiming MACH_KDB: ipc_mqueue.c
  # would emit calls to tr() and tr_indent, the link would fail, and the knob
  # would have promised a facility the tree cannot deliver.  It comes back on
  # in #428, with the rest of the debugger, or not at all -- a per-CPU ring
  # written by hand is a 1992 answer to a question that now has better ones.
  if(NOT arch STREQUAL "i386")
    file(WRITE ${outdir}/mach_tr.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_MACH_TR_H_
#define _UROS_CONFIG_MACH_TR_H_

#ifndef MACH_TR
#define MACH_TR 0
#endif

#endif /* _UROS_CONFIG_MACH_TR_H_ */
")
  endif()

  if(NOT arch STREQUAL "i386")
    file(WRITE ${outdir}/mach_machine_routines.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_MACH_MACHINE_ROUTINES_H_
#define _UROS_CONFIG_MACH_MACHINE_ROUTINES_H_

#ifndef MACH_MACHINE_ROUTINES
#define MACH_MACHINE_ROUTINES 0
#endif

#endif /* _UROS_CONFIG_MACH_MACHINE_ROUTINES_H_ */
")
  endif()

  # cpus.h -- NCPUS is the only knob the command line also sets (-DNCPUS from
  # UROS_NCPUS), so the header defers to it and supplies the UP default for
  # translation units compiled outside the kernel build.
  #
  # #330: NCPUS is the fixed *maximum* -- the size of every per-CPU array --
  # not the number of CPUs booted.  The real count comes from the MADT at
  # runtime, so one image boots any N <= NCPUS and the image layout never
  # shifts with the CPU count.  MAX_CPUS is the name to use in new code.
  file(WRITE ${outdir}/cpus.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_CPUS_H_
#define _UROS_CONFIG_CPUS_H_

#ifndef NCPUS
#define NCPUS 1
#endif

#define MAX_CPUS	NCPUS

#endif /* _UROS_CONFIG_CPUS_H_ */
")

  # etap.h and mach_ldebug.h gather several of the knobs above under one
  # include.  Same values, restated -- the MI tree includes them by either
  # name and a redefinition to an identical value is silent.
  file(WRITE ${outdir}/etap.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_ETAP_H_
#define _UROS_CONFIG_ETAP_H_

#define ETAP			0
#define ETAP_LOCK_TRACE		0
#define ETAP_LOCK_ACCUMULATE	0
#define ETAP_EVENT_MONITOR	0

#endif /* _UROS_CONFIG_ETAP_H_ */
")
  file(WRITE ${outdir}/mach_ldebug.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_MACH_LDEBUG_H_
#define _UROS_CONFIG_MACH_LDEBUG_H_

#define MACH_LDEBUG		0
#define ETAP			0
#define ETAP_LOCK_TRACE		0
#define ETAP_LOCK_ACCUMULATE	0
#define ETAP_EVENT_MONITOR	0

#endif /* _UROS_CONFIG_MACH_LDEBUG_H_ */
")

  # Two heritage names that forward rather than configure.
  file(WRITE ${outdir}/types.h "#include <sys/types.h>\n")
  file(WRITE ${outdir}/cputypes.h
"#ifndef _UROS_CONFIG_CPUTYPES_H_
#define _UROS_CONFIG_CPUTYPES_H_
#include <platforms.h>
#endif /* _UROS_CONFIG_CPUTYPES_H_ */
")

  list(GET UROS_PLATFORM_${arch} 0 _platform)
  list(GET UROS_PLATFORM_${arch} 1 _cpu)
  file(WRITE ${outdir}/platforms.h
"/* Generated by cmake/kernel-config.cmake (#450).  Do not edit, do not track. */
#ifndef _UROS_CONFIG_PLATFORMS_H_
#define _UROS_CONFIG_PLATFORMS_H_

#define ${_platform} 1
#define ${_cpu} 1

#endif /* _UROS_CONFIG_PLATFORMS_H_ */
")
endfunction()
