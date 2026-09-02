cmake_minimum_required(VERSION 3.16)

# Questa directory contiene l'intero albero dei sorgenti Mach.

set(OSFMK_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# -ffreestanding for everything built for Uros (#506).
#
# `-fno-builtin' -- which nearly every target below already passes -- says
# "do not assume the builtins are the standard ones".  It never says "there
# is no host C library", and only the second stops GCC's own <stdint.h> from
# ending in `#include_next <stdint.h>' and landing in the build machine's
# glibc.  With __STDC_HOSTED__ at 1 that chain reached 158 of 162 objects on
# x86-64 and 425 of 487 on i386: the integer typedefs of a Mach userland were
# coming from the Linux system that happened to compile it.
#
# 🔑 Set on the DIRECTORY rather than on each target, because the failure is
# one of omission: with 48 targets naming the flag themselves, the forty-ninth
# would be written without it and nothing would say so.  Inherited, a new
# CMakeLists is freestanding before anyone thinks about it, and opting OUT is
# what has to be written down -- which is the direction that leaves a trace.
#
# ⚠️ src/contrib is added from the top-level CMakeLists, not from here, so
# musl does not inherit this.  It is a hosted C library and builds as one.
#
# The kernel already passes -ffreestanding AND -nostdinc; inheriting a flag it
# sets itself is a no-op, and its zero stays zero either way.
add_compile_options(-ffreestanding)

# The ODE export directory uses pseudo-symlinks ("link ppc") for mach/machine.
# Create a real symlink in the build tree instead: mach/machine -> the
# target architecture's mach headers (#404 made it per-target; it was
# hardcoded to i386 behind a never-refreshed EXISTS guard before).
# This is shared by all subprojects via ${CMAKE_BINARY_DIR}/generated/include.
set(GEN_MACH_MACHINE "${CMAKE_BINARY_DIR}/generated/include/mach/machine")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/mach")
file(REMOVE "${GEN_MACH_MACHINE}")
file(CREATE_LINK
  "${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/${UROS_TARGET_ARCH}"
  "${GEN_MACH_MACHINE}"
  SYMBOLIC)

# uros_musl_dispatcher — kept as a no-op INTERFACE for backward
# compatibility (a few targets still link it).
#
# History: musl's libc-musl.a used to carry WEAK -ENOSYS stubs
# (__uros_syscall0..6, __uros_set_thread_area_tp, __uros_clone;
# uros_syscall_stub.c, #234) that exist only so the standalone libc.so /
# ld-musl links under --no-undefined.  Those weak defs leaked into the
# STATIC archive too, where they shadowed the strong libposix-uros
# dispatcher unless every binary force-pulled them with -Wl,-u (#286, #288).
#
# #291 strips uros_syscall_stub.lo from the static libc-musl.a entirely (see
# contrib/CMakeLists.txt), so the strong __uros_syscallN / TLS / clone defs
# are now the only ones in the static link and get pulled automatically —
# no -Wl,-u anywhere.  __uros_clone is safe to pull into forking binaries
# since #292 (fork's post-fork reinit moved to a pthread_atfork handler).
if(UROS_BUILD_MUSL)
  add_library(uros_musl_dispatcher INTERFACE)
endif()

# Per ora costruiamo solo i tool host necessari (mig/migcom, librerie base).

# #406: the userland — host tools, libraries, servers, tests, commands — is
# i386-only until the x86-64 port carries it across (its own contracts:
# #414 musl, #421-#431).  An x86-64 build is the kernel alone for now, so
# the kernel's own CMakeLists can build its boot stub (#406) against an
# otherwise-empty tree.  Guard everything that is not the kernel on i386.
set(UROS_USERLAND_ARCH_OK FALSE)
if(UROS_TARGET_ARCH STREQUAL "i386")
  set(UROS_USERLAND_ARCH_OK TRUE)
endif()

# migcom is a HOST tool, not target code (#416): it runs on the build machine
# and generates stubs for whatever architecture this build directory targets.
# It used to be reachable only through lib/, which is gated on i386 below --
# so the x86-64 kernel could not run mig at all, and nine machine-independent
# sources failed on generated headers that had never been generated.
#
# Gated on the KERNEL, not on OSFMK_BUILD_TOOLS: since #416 migcom is not an
# optional convenience but a build dependency of the kernel, which cannot be
# assembled without the stubs it generates.  A configuration with the kernel
# on and tools off used to produce a build that simply had no mig, and said
# so only as "No target migcom" from inside a generator expression.
#
# Added once: lib/CMakeLists.txt adds it too, and only when that whole
# directory is in the build.
set(_uros_migcom_from_lib FALSE)
if(OSFMK_BUILD_TOOLS AND UROS_USERLAND_ARCH_OK)
  set(_uros_migcom_from_lib TRUE)
endif()
if(OSFMK_BUILD_KERNEL AND NOT _uros_migcom_from_lib)
  add_subdirectory(lib/migcom)
endif()

# The BAR decode's unit test, on the build machine (#427).
#
# Beside migcom because it is the same kind of thing: a host binary, built in
# every configuration rather than behind the userland gate.  It is here and not
# beside hal_server because it needs nothing hal_server needs -- the decode is
# a pure function, and keeping the test where the server is would have made it
# arrive only on the target where the server already builds.
add_subdirectory(tests/pci_bar_test)

if(OSFMK_BUILD_TOOLS AND UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib)
  add_subdirectory(mach_services/lib)
  add_subdirectory(file_systems)
  add_subdirectory(tools/mkbundle)
  add_subdirectory(tools/mkmanifest)
endif()

# mkbundle, for the target whose userland is not complete either (#422).
#
# It is a host tool exactly as migcom is: it reads files and writes an archive
# of fixed-width little-endian records, and knows nothing whatever about the
# machine those files are for.  Sitting in the block above -- gated on the
# architecture whose userland IS complete -- meant an x86-64 build could not
# make a bundle at all, and a bundle is the difference between bootstrap being
# a binary that links and bootstrap being init.
#
# Same shape as the migcom line above, and added the same way: only when the
# block above has not already added it.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tools/mkbundle)
endif()

# Kernel build.  Both targets need migcom first: since #416 the x86-64 kernel
# generates its interface stubs with the same host tool i386 does.
if(OSFMK_BUILD_KERNEL)
  add_subdirectory(mach_kernel)
endif()

# The Mach trap stubs, ahead of the gate below (#426).
#
# lib/ as a whole is i386-only, and will be until musl and the servers cross
# (#414, #429).  But the stubs are not a library: they are the one file that
# lets anything at all call the kernel, and every issue behind the gate needs
# them before it can begin.  See lib/libmach/CMakeLists.txt for what it builds
# there and why it is a stage rather than the library.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib/libmach)
endif()

# The one user image that is not behind the gate below (#422).
#
# Everything in the userland tree needs musl (#414) or libmach (#426), so an
# x86-64 build has nothing at all to load — and the kernel's ELF64 loader, the
# task set-up and the argument block would all be written against nothing.
# boot_probe is a page of assembly with no dependencies of any kind, which is
# what lets it exist before those issues and what keeps it working after them.
add_subdirectory(tests/boot_probe)

# libpthreads, ahead of the gate (#425).
#
# It needs neither musl nor a filesystem: it is Mach threads, a spin lock and
# a stack.  bootstrap does not link without it -- the thread that answers
# service_checkin while the main thread is busy is a pthread -- so it comes
# before every server rather than after the gate that waits for #414.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib/libpthreads)
endif()

# libnetname, ahead of the gate (#422).
#
# It has no C of its own: it is the MIG stubs of netname.defs, which is what a
# client calls to reach the name server.  bootstrap needs one symbol from it.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib/libnetname)
endif()

# libcap and libgpu_console, ahead of the gate (#422).
#
# bootstrap uses two symbols from each -- cap_provision/cap_request, and
# gpu_console_init/gpu_console_puts.  ⚠️ libgpu_console is a CLIENT of
# gpu_server, which does not run on this target yet (#427): it builds and it
# will fail to connect, which is what it already does on i386 before
# gpu_server is up.  Not building it would mean a different bootstrap here.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib/libcap)
  add_subdirectory(lib/libgpu_console)
endif()

# libsa_fs and libblk, ahead of the gate (#422).
#
# Stage 2: the filesystem bootstrap reads server images from once the bundle
# has got the first ones running.  ⚠️ libblk is here because libsa_fs uses it,
# not because bootstrap does -- bootstrap references nothing in it.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  # ⚠️ libelf was NEVER configured for this target.  Its ELF64 support was
  # written and checked with a host compile, and reported as done -- while no
  # build here consumed it.  The consumer is what collaudates the producer.
  add_subdirectory(lib/libelf)
  add_subdirectory(lib/libblk)
  add_subdirectory(file_systems)
endif()

# The dynamic loader and the module loader (#423).
#
# These are not a server, and they are here rather than beside the fleet
# because they are what four of its members are WAITING for: hal_server,
# block_device_server, char_server and gpu_server all link libmodload and
# libdl, and all four load driver plug-ins at runtime.  #429 lists the fleet in
# boot order, which does not show that one library gates four of them.
#
# ⚠️ libdl reads the file image through libelf now instead of parsing it a
# second time, so it must come after libelf above -- that ordering is the
# dependency and not a preference.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib/libdl)
  add_subdirectory(lib/libmodload)
  # And what proves it, which crosses WITH it (#423).  A loader that builds is
  # not a loader that relocates: the three relocation types the driver plug-ins
  # use are checked one at a time, each by a wrong ANSWER rather than a fault.
  add_subdirectory(tests/dl_module)
  add_subdirectory(tests/dl_test)
endif()

# bootstrap, ahead of the gate (#422).
#
# It is init on this target as it is on i386: the first task the kernel starts,
# and the one that creates every server after it.  So it comes before the gate
# for the same reason the six libraries above do -- it needs none of what is
# behind it, and everything behind it needs to be started by it.
#
# The one symbol that used to hold it out was host_load_symbol_table, from
# mach_debug.defs; what kept that .defs from generating was six types in
# mach_debug_types.defs declared as a fixed count of 32-bit words when their
# size follows the pointer width.  See the note in lib/libmach/CMakeLists.txt.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/bootstrap)
endif()

# And the first server, also ahead of the gate (#426).
#
# The gate below is about musl and the C library; the name server needs
# neither.  It links libmach and nothing else, which is precisely the claim
# #426 has to end on -- so it is built here rather than waiting for #414 to
# open the door for a server that does not use what is behind it.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/name_server)
endif()

# And the task that asks the kernel for a fault (#467).
#
# Here rather than behind the gate for the same reason as the name server: it
# links libmach and libpthreads and needs nothing else.  It is ahead of the
# gate and BEHIND libpthreads, because the arm that hands a fault to the task's
# exception port needs a second thread -- a thread suspended in
# exception_raise() cannot be the one that receives the message it just sent.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/fault_test)
endif()

# And the task that takes its own threads out of calls they will not finish
# (#475).  Same placement and the same reason: libmach, libpthreads, and a
# second thread, because every arm of it is one thread acting on another.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/act_test)
endif()

# And the task that claims an interrupt line and gives it back (#457).
#
# 🔑 The only test here that is added on BOTH targets, and deliberately.  The
# two machines answer <device/device_machdep.h> with different machinery, and
# the whole reason the contract names the operation rather than the mechanism
# is that one program above it cannot tell.  Running it on one target would
# measure an implementation.
#
# ⚠️ It is also the only thing that reaches the defect it was written for.
# ds_master_device_intr_unregister() halted i386 -- take_irq() on a line whose
# priority its own register had just set -- and nothing exercised it, because
# the one caller in the tree is a module detach() that core.c reaches only
# when the device table is full.
add_subdirectory(tests/irq_claim_test)

# And the task that forks with copy-on-write (#407) -- the fifth clause of that
# issue's done-when, and the only one nothing else exercises.
#
# One thread, unlike the two above: the subject is an address space, and the
# cleanest subject is a child task with nothing running inside it, because
# nothing there can move while it is being read.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/cow_test)
endif()

# And the task that CONTACTS the name server (#426), which is the last clause
# of that issue's done-when and the one nothing else could show.
#
# ⚠️ It goes here, after libnetname above, and not beside the name server: this
# is the CLIENT half.  The server starting proves it can reach bootstrap; only
# a second task sending it a message proves anybody can reach the server, and
# on this target there is no other netname client -- cap_server and proc_server
# are not ported, and bootstrap's own look-up is in stage 2, behind a disk.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/netname_test)
endif()

# And the POSIX thread suite (#425), which is that issue's whole done-when.
#
# Nothing new has to cross for it: the five libraries it links -- libpthreads,
# libmach, libgpu_console, libcap, libnetname -- are every one of them already
# above this gate for their own reasons.  What was missing was asking.
#
# ⚠️ It is the biggest program on this target by an order of magnitude, and that
# is the point rather than a risk: act_test proves pthread_create works, which
# is one thread created once.  Twenty-three arms of mutexes, condition
# variables, joins, cancellation and thread-local storage prove the library.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/pthread_test)
endif()

# And the first server of the fleet (#495, part of #429).
#
# ⚠️ The issue says cap_server "links libmach and libpthreads and nothing
# else".  It links FIVE -- libcap, libpthreads, libmach, libnetname and
# libgpu_console -- and the conclusion survives only because all five happen to
# be above this gate already, each for its own reason.  Checked rather than
# inherited: the premise was optimistic and the answer was still yes.
#
# It is second in boot order, after the name server, because what comes above
# it asks it for capabilities.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/cap_server)
endif()

# And its test, which crosses WITH the server and not after it (#495).
#
# 🔑 A server that starts and says so is not evidence that it works; cap_test
# is the only thing on either target that asks it for a capability and checks
# the answer.  Landing the server alone would mean landing an unmeasured one.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(tests/cap_test)
endif()

# hal_server on x86-64 (#427), and STARTED as of #457.
#
# 🔑 "It compiles" and "it runs" are two steps, and #427 took the first: every
# library it links came across with #423, and lifting it found what a traversal
# finds, at build time.
#
# It was then deliberately absent from bootstrap.conf, because MACH_DEVICE_MASTER
# was 0 here and the kernel implemented none of the fourteen RPCs a discovery
# module needs -- every device_pci_config_read would have answered MIG_BAD_ID
# and the server would have printed `module pci_scan scan failed' on every boot,
# for months.  A known and permanent red line is how a suite stops being read,
# which is exactly what #512 was.
#
# That premise fell with #457: the knob is 1, config space answers by ECAM or by
# the port pair depending on the board, and the server is in the bundle.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/hal_server)
endif()

# block_device_server on x86-64 (#520), and the first task here that moves a
# byte with a device.
#
# 🔑 IT IS NOT HERE FOR ITSELF.  #432's done-when asks that a driver run against
# a real IOMMU domain and that an out-of-domain DMA be blocked and reported --
# and nothing in this target's bundle does DMA at all.  hal_server enumerates
# devices and reads their BARs; the only callers of device_dma_alloc in the tree
# are this server's two modules and ext_server.  So the IOMMU could be built,
# programmed and read back while the property it exists to provide stayed
# undemonstrable for want of anything to demonstrate it on.
#
# ⚠️ And the obvious substitute does not work.  The MSI doorbell #457 makes a
# device write IS a DMA write, but both vendors exclude it: a single-DWORD write
# to FEEx_xxxxh is an interrupt request, "not subjected to DMA remapping (even
# if translation structures specify a mapping for this range)".  See the
# citations in <cpu/iommu_backend.h>.
#
# Nothing was holding it out but this file.  Every library it links is already
# above the gate -- libdl and libmodload came across with #423 precisely because
# four servers were waiting on them, and this is one of the four.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/block_device_server)
endif()

# Everything below is userland; skip it wholesale on a non-i386 target.
if(NOT UROS_USERLAND_ARCH_OK)
  message(STATUS "userland skipped: ${UROS_TARGET_ARCH} builds the kernel only (#406)")
  return()
endif()

# Build bootstrap server
if(OSFMK_BUILD_BOOTSTRAP)
  add_subdirectory(servers/bootstrap)
endif()

# Build default pager server
if(OSFMK_BUILD_DEFAULT_PAGER)
  add_subdirectory(servers/default_pager)
endif()

# Build hello server (test/demo server)
if(OSFMK_BUILD_HELLO_SERVER)
  add_subdirectory(servers/hello_server)
endif()

# Build exec_server (userspace ELF loader / process spawner, #228)
if(OSFMK_BUILD_EXEC_SERVER)
  add_subdirectory(servers/exec_server)
endif()

# Build hello_exec test binary (#228 v0.1.0 acceptance)
if(OSFMK_BUILD_HELLO_EXEC)
  add_subdirectory(tests/hello_exec)
endif()

# Build hello_dyn — first dynamically-linked binary (#234 Phase 7).
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/hello_dyn)
endif()

# Build fd_exec_test (#262 step 3: execve fd-inheritance smoke target)
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/fd_exec_test)
endif()

# Build hello_world (#286: standard printf-to-ctty hello world)
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/hello_world)
endif()

# Build hello_dyn_world (#289: dynamic twin of hello_world — verifies the
# dynamic clean-exit/reap path through ld-musl + the umbrella libc.so)
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/hello_dyn_world)
endif()

# Build pthread_min (#291: minimal musl pthread regression — proves
# pthread_create works without any -Wl,-u clone workaround)
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/pthread_min)
endif()

# Build flipc_bench (#272: standalone libvfs FLIPC v2 A/B benchmark,
# launched from the shell post-boot for quiescent, median-of-N numbers)
if(UROS_BUILD_MUSL)
  add_subdirectory(bench/flipc_bench)
endif()

# Build ush — Uros shell (#275.5).
if(UROS_BUILD_MUSL)
  add_subdirectory(cmds/ush)
endif()

# Build cpustat (#375) — per-CPU load monitor, the first dynamically-linked
# real tool (umbrella libc.so + libmach_core-pic), launched from the shell.
if(UROS_BUILD_MUSL)
  add_subdirectory(cmds/cpustat)
endif()

# Build virtual_terminal_server — VT shell supervisor (#365 phase 2b).
if(UROS_BUILD_MUSL)
  add_subdirectory(servers/virtual_terminal_server)
endif()

# Build libfoo.so + dlopen_test — musl dlopen end-to-end (#234 incr 4)
if(UROS_BUILD_MUSL)
  add_subdirectory(tests/libfoo)
  add_subdirectory(tests/dlopen_test)
endif()

# Build proc_server (POSIX process model owner, #237)
if(OSFMK_BUILD_PROC_SERVER)
  add_subdirectory(servers/proc_server)
endif()

# Build IPC performance benchmark
if(OSFMK_BUILD_IPC_BENCH)
  add_subdirectory(bench/ipc_bench)
endif()

# The standalone AHCI userspace driver (src/ahci_driver) was removed: it was
# superseded by the modular block_device_server/ahci (ahci.so loaded by blk)
# and was no longer deployed in the bundle or on disk.  ext_server's MIG
# ahci_batch stubs now come from block_device_server/ahci/ (identical defs).
# The standalone virtio-blk driver (src/virtio_blk) followed it in #396,
# for the same reason: superseded by block_device_server/virtio_blk
# (virtio_blk.so), the only thing the bundle ever shipped.

# Build modular block device server
if(OSFMK_BUILD_BLOCK_SERVER)
  add_subdirectory(servers/block_device_server)
endif()

# Build HAL server (hardware discovery + device registry)
if(OSFMK_BUILD_HAL_SERVER)
  add_subdirectory(servers/hal_server)
endif()

# Build name server (netname)
if(OSFMK_BUILD_NAME_SERVER)
  add_subdirectory(servers/name_server)
endif()

# Build ext filesystem server (ext2/ext3/ext4)
if(OSFMK_BUILD_EXT2_SERVER)
  add_subdirectory(servers/ext_server)
endif()

# Build pthread test server
if(OSFMK_BUILD_PTHREAD_TEST)
  add_subdirectory(tests/pthread_test)
endif()

# Build capability server (Uros cap_server)
if(OSFMK_BUILD_CAP_SERVER)
  add_subdirectory(servers/cap_server)
endif()

# Build capability negative-test binary
if(OSFMK_BUILD_CAP_TEST)
  add_subdirectory(tests/cap_test)
endif()

# Build kernel242_test (#242 no-goto kernel exerciser)
add_subdirectory(tests/kernel242_test)

# Build sig_test (#238 proc_server signal exerciser)
add_subdirectory(tests/sig_test)

# Build gpustat userspace probe for gpu_query_stats (#203)
if(OSFMK_BUILD_GPUSTAT)
  add_subdirectory(cmds/gpustat)
endif()

# Build gpu_server (#194 — userspace GPU/display server)
if(OSFMK_BUILD_GPU_SERVER)
  add_subdirectory(servers/gpu_server)
endif()

# Build char_server (#205 — userspace character device server)
if(OSFMK_BUILD_CHAR_SERVER)
  add_subdirectory(servers/char_server)
endif()
