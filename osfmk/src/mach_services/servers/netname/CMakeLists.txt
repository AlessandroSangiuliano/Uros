cmake_minimum_required(VERSION 3.16)
project(name_server C)

# ===========================================================================
# MIG stub generation (via scripts/mig)
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(MIG_OUT    ${CMAKE_CURRENT_BINARY_DIR}/mig)
file(MAKE_DIRECTORY "${MIG_OUT}")

# Helper macro: run scripts/mig to produce all 4 outputs from a .defs file
macro(mig_all DEFS_FILE STEM)
  set(_defs  "${DEFS_FILE}")
  set(_srvC  "${MIG_OUT}/${STEM}_server.c")
  set(_srvH  "${MIG_OUT}/${STEM}_server.h")
  set(_usrC  "${MIG_OUT}/${STEM}_user.c")
  set(_hdr   "${MIG_OUT}/${STEM}.h")
  add_custom_command(
    OUTPUT ${_srvC} ${_srvH} ${_usrC} ${_hdr}
    COMMAND ${MIG_SCRIPT}
      -cpp ${CMAKE_C_COMPILER}
      -migcom $<TARGET_FILE:migcom>
      -I${CMAKE_SOURCE_DIR}/export/powermac/include
      -I${CMAKE_SOURCE_DIR}/src/mach_kernel
      -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
      -header  ${_hdr}
      -sheader ${_srvH}
      -user    ${_usrC}
      -server  ${_srvC}
      ${_defs}
    DEPENDS migcom ${_defs}
    COMMENT "MIG: ${STEM}.defs (name_server)"
    VERBATIM
  )
endmacro()

# 1) netname.defs — server stubs (netname_server demux)
mig_all(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/servers/netname.defs"
  netname)

# 2) notify.defs — server stubs (do_mach_notify_dead_name etc.)
mig_all(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/notify.defs"
  notify)

# 3) service.defs — user stubs (service_checkin)
mig_all(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/servers/service.defs"
  service)

# ===========================================================================
# Source files
# ===========================================================================

set(NETNAME_SOURCES
  netname.c
  nprocs.c
  nostdlib_stubs.c
  ${MIG_OUT}/netname_server.c
  ${MIG_OUT}/notify_server.c
  ${MIG_OUT}/service_user.c
)

# ===========================================================================
# Build target
# ===========================================================================

add_executable(name_server_bin ${NETNAME_SOURCES})

add_dependencies(name_server_bin mig_mach_public_headers)

target_compile_options(name_server_bin PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(name_server_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(name_server_bin PRIVATE
  # Local source
  ${CMAKE_CURRENT_SOURCE_DIR}
  # MIG-generated stubs
  ${MIG_OUT}
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, cthreads.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Pre-built export headers (mach/*, servers/*, device/*)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers (mach/, device/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against Mach userspace libraries.
target_link_libraries(name_server_bin PRIVATE
  -Wl,--start-group
  libcthreads
  libmach
  libsa_mach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(name_server_bin PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(name_server_bin PROPERTIES
  OUTPUT_NAME "name_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
