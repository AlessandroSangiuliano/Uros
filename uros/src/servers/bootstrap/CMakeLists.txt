cmake_minimum_required(VERSION 3.16)
project(bootstrap_server C)

# ===========================================================================
# MIG: generate server-side stubs for the 4 subsystems
# ===========================================================================

set(MIG_INCLUDE_FLAGS
  -I${UROS_ARCH_INCLUDE}
  -I${CMAKE_SOURCE_DIR}/src/mach_kernel
  -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
)

# Helper macro: run CPP | migcom to produce server .c and .h from a .defs
macro(mig_server_stub DEFS_FILE STEM OUT_DIR)
  set(_defs  "${DEFS_FILE}")
  set(_srvC  "${OUT_DIR}/${STEM}Server.c")
  set(_srvH  "${OUT_DIR}/${STEM}Server.h")
  set(_usrC  "${OUT_DIR}/${STEM}User.c")
  set(_hdr   "${OUT_DIR}/${STEM}.h")
  set(_mig_pp "${OUT_DIR}/${STEM}.mig.i")
  add_custom_command(
    OUTPUT ${_srvC} ${_usrC} ${_hdr} ${_srvH}
    # #471: uapi first, and no pipe -- see cmake/mig.cmake for why both.
    COMMAND ${CMAKE_C_COMPILER} -E -x c -I${UROS_UAPI_DIR} ${MIG_INCLUDE_FLAGS}
            ${_defs} -o ${_mig_pp}
    COMMAND $<TARGET_FILE:migcom>
        # ⚠️ Which machine the stubs are for (#453/#426).  migcom defaults to
        # i386, so a rule without this generates i386 layout in silence.
        ${UROS_MIG_TARGET_ARGS}
        -header ${_hdr}
        -sheader ${_srvH}
        -user   ${_usrC}
        -server ${_srvC}
        ${_mig_pp}
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${_defs}
    COMMENT "MIG: ${STEM}.defs -> ${STEM}Server.c"
    VERBATIM
  )
endmacro()

set(MIG_OUT "${CMAKE_BINARY_DIR}/generated/bootstrap_mig")
file(MAKE_DIRECTORY "${MIG_OUT}")

# 1) bootstrap.defs  (subsystem 999999, ServerPrefix do_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/uapi/mach/bootstrap.defs"
  bootstrap  ${MIG_OUT})

# 2) exc.defs  (subsystem 2400, ServerPrefix catch_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/uapi/mach/exc.defs"
  exc  ${MIG_OUT})

# 3) notify.defs  (subsystem 64, ServerPrefix do_)
mig_server_stub(
  "${UROS_UAPI_DIR}/mach/notify.defs"
  notify  ${MIG_OUT})

# 4) service.defs  (subsystem 400, ServerPrefix do_)
mig_server_stub(
  "${UROS_UAPI_DIR}/servers/service.defs"
  service  ${MIG_OUT})

# 5) gpu_server.defs (subsystem 4000) — bootstrap is the gpu_text_puts
#    client (#198 log_forwarder).  We only need the user stubs but the
#    macro emits both; the server-side .c is left unused.
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/src/servers/gpu_server/gpu_server.defs"
  gpu_server  ${MIG_OUT})

set(MIG_SERVER_SOURCES
  ${MIG_OUT}/bootstrapServer.c
  ${MIG_OUT}/excServer.c
  ${MIG_OUT}/notifyServer.c
  ${MIG_OUT}/serviceServer.c
  ${MIG_OUT}/gpu_serverUser.c
)

# ===========================================================================
# Source files
# ===========================================================================

# Core sources (platform-independent)
set(BOOTSTRAP_SOURCES
  bootstrap.c
  service.c
  load.c
  strfcns.c
  # ⚠️ device_read_overwrite.c was here, and is gone rather than moved: it was
  # a private copy of a libmach call, written because the mig_device_read
  # fallback the real wrapper needs was missing from this build.  It is not
  # missing any more -- libmach/syscall_wrappers.c supplies it, calling
  # device_read() which is the same stub under the name this build gives it --
  # and the copy here skipped the trap on the way, which is right for
  # bootstrap's own BDS handle and wrong for a kernel device.  ext_server had
  # the identical file, byte for byte.
  # Issue #186: stage-1 multiboot bundle parser (early-stage server
  # binaries pre-loaded into RAM, removing the need for kernel IDE).
  bundle.c
  # Issue #198: forward bootstrap log output to gpu_server's userspace
  # text path (replaces the kernel VGA console once #199 lands).
  log_forwarder.c
)

# Executable format loaders
# i386: a_out + elf (formats.c selects them; coff/rose/som are for other archs)
# ⚠️ a_out.c only on i386: there is no 64-bit a.out, so x86_64/formats.c names
# elf_switch alone and this file would define a loader nothing points at.
if(UROS_TARGET_ARCH STREQUAL "i386")
  list(APPEND BOOTSTRAP_SOURCES a_out.c)
endif()
list(APPEND BOOTSTRAP_SOURCES elf.c)

# The parser is shared; the rest is the machine's own -- which formats it
# loads, where a task starts, and what it can do with an image besides run it.
list(APPEND BOOTSTRAP_SOURCES parse_args.c)
if(UROS_TARGET_ARCH STREQUAL "i386")
  list(APPEND BOOTSTRAP_SOURCES
    i386/boot_dep.c
    i386/set_regs.c
    i386/formats.c
  )
else()
  list(APPEND BOOTSTRAP_SOURCES
    x86_64/boot_dep.c
    x86_64/set_regs.c
    x86_64/formats.c
  )
endif()

# ===========================================================================
# Build target: Mach standalone executable
# ===========================================================================

# Stubs for symbols normally provided by host libc (-nostdlib build)
list(APPEND BOOTSTRAP_SOURCES nostdlib_stubs.c)

add_executable(bootstrap_server
  ${BOOTSTRAP_SOURCES}
  ${MIG_SERVER_SOURCES}
)

add_dependencies(bootstrap_server mig_mach_public_headers)

target_compile_options(bootstrap_server PRIVATE
  -std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h ${UROS_ARCH_FLAGS} -msse -msse2
)

target_compile_definitions(bootstrap_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(bootstrap_server PRIVATE
  # Local sources first (bootstrap.h, exec.h, elf.h)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # MIG generated output (bootstrapServer.h, excServer.h, etc.)
  ${MIG_OUT}
  # Generated includes (mach/machine -> i386 symlink created in src/CMakeLists.txt)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, servers/)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  # ⚠️ The machine's half of libpthreads, chosen rather than named: AT386 was
  # hardcoded here, and on this target it would have shadowed x86_64's
  # pthread_machdep.h with i386's -- a lock type of the wrong width and a
  # stack-switch sequence in 32-bit assembly.
  $<IF:$<STREQUAL:${UROS_TARGET_ARCH},i386>,${CMAKE_SOURCE_DIR}/src/lib/libpthreads/AT386,${CMAKE_SOURCE_DIR}/src/lib/libpthreads/x86_64>
  # Filesystem headers (file_system.h, externs.h)
  ${CMAKE_SOURCE_DIR}/src/file_systems
  # Kernel headers (mach/, device/, ddb/, i386/) — bootstrap needs kernel-internal
  # extensions (boot_info.h multi-segment support) so kernel must come BEFORE export
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
  # Issue #185: stage-2 reads via BDS (libblk) gated by Uros caps (libcap)
  ${CMAKE_SOURCE_DIR}/src/lib/libblk
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  # libgpu_console (#199 prep) — printf mirror to gpu_server
  ${CMAKE_SOURCE_DIR}/src/lib/libgpu_console
)

# Link against Mach userspace libraries.
# Use --start-group/--end-group to resolve circular dependencies between
# the Mach libraries (libpthreads -> libmach for MIG stubs, libmach ->
# libpthreads for mig_support).
target_link_libraries(bootstrap_server PRIVATE
  -Wl,--start-group
  libsa_fs
  libblk
  libcap
  libgpu_console
  libnetname
  libelf
  libpthreads
  libmach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc
target_link_options(bootstrap_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
)

set_target_properties(bootstrap_server PROPERTIES
  OUTPUT_NAME "bootstrap"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# ===========================================================================
# The stage-1 bundle, built by the build (#422)
# ===========================================================================
#
# The bundle is what turns this binary into init: the kernel maps it into the
# bootstrap task and says where on argv, and bootstrap reads bootstrap.conf out
# of it and starts what that names.  Without one there is nothing to start,
# which is what "No bootstrap code loaded with the kernel" has been saying on
# this target since the beginning.
#
# ⚠️ Made here and not by scripts/make-bundle.sh, and that is not a rewrite of
# it.  That script describes an i386 system -- twenty servers, two disk stages,
# a bench selector, driver plug-ins -- and every one of those things is a
# REQUIRED_FILE it checks for.  On this target none of them exists yet.  What
# does exist is one server, so the bundle is two entries; and a rule that knows
# when either of them changed is worth more than a script somebody has to
# remember to re-run after a rebuild.  mkbundle is the piece that is shared,
# which is the piece that was worth sharing.
if(NOT UROS_TARGET_ARCH STREQUAL "i386")
  set(_BUNDLE ${CMAKE_BINARY_DIR}/bootstrap.bundle)
  set(_CONF   ${CMAKE_CURRENT_BINARY_DIR}/bootstrap.conf)
  set(_SBIN   ${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin)

  # Four lines, and the lines that are NOT here are the point: i386's
  # bootstrap.conf names hal_server, block_device_server, default_pager,
  # ext_server and a dozen more, and bootstrap would go looking for each of
  # them in turn.  It names what has crossed: the name server, the task that
  # asks the kernel for a fault (#467), the task that takes its own threads out
  # of calls they will not finish (#475), and the task that forks with
  # copy-on-write (#407).
  #
  # ⚠️ The ORDER is load-bearing rather than tidy, and it is least-damage
  # first.  fault_test faults on purpose and act_test kills a thread on
  # purpose; either one, on a kernel that mishandles it, takes the boot down
  # where it stands.  With the name server first, a failure in either can be
  # read as a failure in that one -- put either ahead of it and a #422 result
  # would look like a regression in whatever ran first.
  #
  # netname_test is SECOND, immediately after the server it talks to (#426).
  # It is harmless -- it sends two messages and reads the answers -- so the
  # least-damage rule puts it early; and being right after the name server
  # means that if it fails, the two things that could be at fault are adjacent
  # in the log rather than separated by three programs that fault and kill
  # threads on purpose.
  #
  # pthread_test is THIRD, and it is the biggest program here by an order of
  # magnitude (#425).  It goes after the two that only send messages and before
  # the three that fault, kill threads and fork on purpose: it does nothing
  # deliberately fatal, so a boot that ends inside it is a finding about the
  # thread library rather than collateral from a neighbour.
  #
  # cow_test is last, and by the strongest form of that same rule rather than
  # by seniority: it is the only entry that asks the kernel for a mechanism no
  # boot on this target has ever used, so it is the one most likely to end the
  # run.  Last means every other verdict is already in the log when it does,
  # and the blame cannot land anywhere else.
  file(GENERATE OUTPUT ${_CONF}
       CONTENT "name_server name_server\nnetname_test netname_test\npthread_test pthread_test\nfault_test fault_test\nact_test act_test\ncow_test cow_test\n")

  # ⚠️ It was briefly six while #476 was being hunted -- three copies of the
  # same image under three names, to multiply the STARTING of a task without
  # adding a second program's behaviour to the measurement.  That was an
  # experiment and is not a configuration: it belongs in the issue and in
  # ~/uros-tests, not in what the build ships.
  add_custom_command(
    OUTPUT ${_BUNDLE}
    COMMAND $<TARGET_FILE:mkbundle> -o ${_BUNDLE}
            bootstrap.conf:${_CONF}
            name_server:${_SBIN}/name_server
            netname_test:${_SBIN}/netname_test
            pthread_test:${_SBIN}/pthread_test
            fault_test:${_SBIN}/fault_test
            act_test:${_SBIN}/act_test
            cow_test:${_SBIN}/cow_test
    DEPENDS mkbundle name_server_bin netname_test_bin pthread_test_server fault_test_bin act_test_bin cow_test_bin
            ${_CONF}
    COMMENT "Bundle: bootstrap.conf + name_server + netname_test + pthread_test + fault_test + act_test + cow_test"
    VERBATIM)

  add_custom_target(bootstrap_bundle ALL DEPENDS ${_BUNDLE})
endif()
