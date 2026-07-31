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
    flipc                  NFLIPC 0
    gprof                  NGPROF 0
    hw_footprint           HW_FOOTPRINT 1
    kernel_test            KERNEL_TEST 0
    mach_assert            MACH_ASSERT 1
    mach_cluster_stats     MACH_CLUSTER_STATS 0
    mach_counters          MACH_COUNTERS 0
    mach_debug             MACH_DEBUG 1
    mach_host              MACH_HOST 1
    mach_ipc_debug         MACH_IPC_DEBUG 0
    mach_ipc_test          MACH_IPC_TEST 0
    mach_kdb               MACH_KDB 1
    mach_kgdb              MACH_KGDB 0
    mach_kprof             MACH_KPROF 0
    mach_lock_mon          MACH_LOCK_MON 0
    mach_machine_routines  MACH_MACHINE_ROUTINES 1
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
