cmake_minimum_required(VERSION 3.16)
project(bootstrap_server C)

# ===========================================================================
# MIG: generate server-side stubs for the 4 subsystems
# ===========================================================================

set(MIG_INCLUDE_FLAGS
  -I${CMAKE_SOURCE_DIR}/export/powermac/include
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
  add_custom_command(
    OUTPUT ${_srvC} ${_usrC} ${_hdr} ${_srvH}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${MIG_INCLUDE_FLAGS} ${_defs}
      | $<TARGET_FILE:migcom>
        -header ${_hdr}
        -sheader ${_srvH}
        -user   ${_usrC}
        -server ${_srvC}
    DEPENDS migcom ${_defs}
    COMMENT "MIG: ${STEM}.defs -> ${STEM}Server.c"
    VERBATIM
  )
endmacro()

set(MIG_OUT "${CMAKE_BINARY_DIR}/generated/bootstrap_mig")
file(MAKE_DIRECTORY "${MIG_OUT}")

# 1) bootstrap.defs  (subsystem 999999, ServerPrefix do_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/bootstrap.defs"
  bootstrap  ${MIG_OUT})

# 2) exc.defs  (subsystem 2400, ServerPrefix catch_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/exc.defs"
  exc  ${MIG_OUT})

# 3) notify.defs  (subsystem 64, ServerPrefix do_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/notify.defs"
  notify  ${MIG_OUT})

# 4) service.defs  (subsystem 400, ServerPrefix do_)
mig_server_stub(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/servers/service.defs"
  service  ${MIG_OUT})

set(MIG_SERVER_SOURCES
  ${MIG_OUT}/bootstrapServer.c
  ${MIG_OUT}/excServer.c
  ${MIG_OUT}/notifyServer.c
  ${MIG_OUT}/serviceServer.c
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
)

# Executable format loaders
# i386: a_out + elf (formats.c selects them; coff/rose/som are for other archs)
list(APPEND BOOTSTRAP_SOURCES
  a_out.c
  elf.c
)

# Architecture-specific (i386)
list(APPEND BOOTSTRAP_SOURCES
  i386/boot_dep.c
  i386/set_regs.c
  i386/formats.c
)

# ===========================================================================
# Build target: Mach standalone executable
# ===========================================================================

# Stubs for symbols normally provided by host libc (-nostdlib build)
list(APPEND BOOTSTRAP_SOURCES nostdlib_stubs.c)

add_executable(bootstrap_server
  ${BOOTSTRAP_SOURCES}
  ${MIG_SERVER_SOURCES}
)

target_compile_options(bootstrap_server PRIVATE
  -std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686 -msse -msse2
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
  # Mach services include (sa_mach.h, servers/, cthreads.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Kernel headers (mach/, device/, ddb/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Pre-built export headers (mach/mach_interface.h, mach/fs/file_system.h, etc.)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against Mach userspace libraries.
# Use --start-group/--end-group to resolve circular dependencies between
# the Mach libraries (e.g. libsa_mach -> libmach for clock_sleep, libcthreads -> libmach
# for MIG stubs, libmach -> libcthreads for mig_support).
target_link_libraries(bootstrap_server PRIVATE
  -Wl,--start-group
  libsa_fs
  libcthreads
  libmach
  libsa_mach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc
# --allow-multiple-definition: cthreads inline functions (spin_unlock, spin_try_lock,
# cthread_sp) are emitted into every .o that includes cthreads.h (C89 inline semantics).
target_link_options(bootstrap_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
)

set_target_properties(bootstrap_server PROPERTIES
  OUTPUT_NAME "bootstrap"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
