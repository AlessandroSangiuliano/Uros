cmake_minimum_required(VERSION 3.16)

# Questa directory contiene l'intero albero dei sorgenti Mach.

set(OSFMK_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

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

if(OSFMK_BUILD_TOOLS AND UROS_USERLAND_ARCH_OK)
  add_subdirectory(lib)
  add_subdirectory(mach_services/lib)
  add_subdirectory(file_systems)
  add_subdirectory(tools/mkbundle)
  add_subdirectory(tools/mkmanifest)
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

# And the first server, also ahead of the gate (#426).
#
# The gate below is about musl and the C library; the name server needs
# neither.  It links libmach and nothing else, which is precisely the claim
# #426 has to end on -- so it is built here rather than waiting for #414 to
# open the door for a server that does not use what is behind it.
if(OSFMK_BUILD_KERNEL AND NOT UROS_USERLAND_ARCH_OK)
  add_subdirectory(servers/name_server)
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
