cmake_minimum_required(VERSION 3.16)

# Questa directory contiene l'intero albero dei sorgenti Mach.

set(OSFMK_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# The ODE export directory uses pseudo-symlinks ("link ppc") for mach/machine.
# We target i386, so create a real symlink: mach/machine -> mach/i386 headers.
# This is shared by all subprojects via ${CMAKE_BINARY_DIR}/generated/include.
set(GEN_MACH_MACHINE "${CMAKE_BINARY_DIR}/generated/include/mach/machine")
if(NOT EXISTS "${GEN_MACH_MACHINE}")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/mach")
  file(CREATE_LINK
    "${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386"
    "${GEN_MACH_MACHINE}"
    SYMBOLIC)
endif()

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

if(OSFMK_BUILD_TOOLS)
  add_subdirectory(lib)
  add_subdirectory(mach_services/lib)
  add_subdirectory(file_systems)
  add_subdirectory(tools/mkbundle)
  add_subdirectory(tools/mkmanifest)
endif()

# Kernel build (requires migcom to be built first)
if(OSFMK_BUILD_KERNEL)
  add_subdirectory(mach_kernel)
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
  add_subdirectory(hello_exec)
endif()

# Build hello_dyn — first dynamically-linked binary (#234 Phase 7).
if(UROS_BUILD_MUSL)
  add_subdirectory(hello_dyn)
endif()

# Build fd_exec_test (#262 step 3: execve fd-inheritance smoke target)
if(UROS_BUILD_MUSL)
  add_subdirectory(fd_exec_test)
endif()

# Build hello_world (#286: standard printf-to-ctty hello world)
if(UROS_BUILD_MUSL)
  add_subdirectory(hello_world)
endif()

# Build hello_dyn_world (#289: dynamic twin of hello_world — verifies the
# dynamic clean-exit/reap path through ld-musl + the umbrella libc.so)
if(UROS_BUILD_MUSL)
  add_subdirectory(hello_dyn_world)
endif()

# Build pthread_min (#291: minimal musl pthread regression — proves
# pthread_create works without any -Wl,-u clone workaround)
if(UROS_BUILD_MUSL)
  add_subdirectory(pthread_min)
endif()

# Build flipc_bench (#272: standalone libvfs FLIPC v2 A/B benchmark,
# launched from the shell post-boot for quiescent, median-of-N numbers)
if(UROS_BUILD_MUSL)
  add_subdirectory(flipc_bench)
endif()

# Build ush — Uros shell (#275.5).
if(UROS_BUILD_MUSL)
  add_subdirectory(ush)
endif()

# Build cpustat (#375) — per-CPU load monitor, the first dynamically-linked
# real tool (umbrella libc.so + libmach_core-pic), launched from the shell.
if(UROS_BUILD_MUSL)
  add_subdirectory(cpustat)
endif()

# Build virtual_terminal_server — VT shell supervisor (#365 phase 2b).
if(UROS_BUILD_MUSL)
  add_subdirectory(servers/virtual_terminal_server)
endif()

# Build libfoo.so + dlopen_test — musl dlopen end-to-end (#234 incr 4)
if(UROS_BUILD_MUSL)
  add_subdirectory(libfoo)
  add_subdirectory(dlopen_test)
endif()

# Build proc_server (POSIX process model owner, #237)
if(OSFMK_BUILD_PROC_SERVER)
  add_subdirectory(servers/proc_server)
endif()

# Build IPC performance benchmark
if(OSFMK_BUILD_IPC_BENCH)
  add_subdirectory(ipc_bench)
endif()

# The standalone AHCI userspace driver (src/ahci_driver) was removed: it was
# superseded by the modular block_device_server/ahci (ahci.so loaded by blk)
# and was no longer deployed in the bundle or on disk.  ext_server's MIG
# ahci_batch stubs now come from block_device_server/ahci/ (identical defs).

# Build virtio-blk userspace driver
if(OSFMK_BUILD_VIRTIO_BLK)
  add_subdirectory(virtio_blk)
endif()

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
  add_subdirectory(pthread_test)
endif()

# Build capability server (Uros cap_server)
if(OSFMK_BUILD_CAP_SERVER)
  add_subdirectory(servers/cap_server)
endif()

# Build capability negative-test binary
if(OSFMK_BUILD_CAP_TEST)
  add_subdirectory(cap_test)
endif()

# Build kernel242_test (#242 no-goto kernel exerciser)
add_subdirectory(kernel242_test)

# Build sig_test (#238 proc_server signal exerciser)
add_subdirectory(sig_test)

# Build gpustat userspace probe for gpu_query_stats (#203)
if(OSFMK_BUILD_GPUSTAT)
  add_subdirectory(gpustat)
endif()

# Build gpu_server (#194 — userspace GPU/display server)
if(OSFMK_BUILD_GPU_SERVER)
  add_subdirectory(servers/gpu_server)
endif()

# Build char_server (#205 — userspace character device server)
if(OSFMK_BUILD_CHAR_SERVER)
  add_subdirectory(servers/char_server)
endif()
