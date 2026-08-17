cmake_minimum_required(VERSION 3.16)
project(name_server C)

# ===========================================================================
# x86-64 (#426) — the first SERVER built for this target
# ===========================================================================
#
# The last thing #426 asks for is that the name server can be contacted, and
# it is the right server to ask it of: it is the smallest real one in the
# tree, it links libmach and nothing else, and it is single-threaded, so what
# it exercises is exactly the surface this issue delivers -- the trap stubs,
# the startup, and MIG in both directions.
#
# ⚠️ It cannot go through scripts/mig, and that is not a style preference.
# The wrapper routes any flag it does not recognise to the C PREPROCESSOR, so
# `-target x86_64' would arrive at cc and never reach migcom -- whose default
# is i386.  The stubs would be generated with i386's message layout, and this
# is the kind of wrong that is quiet: the _Static_asserts migcom emits catch
# it, but only in the stubs it emits them for.  add_mig_userland calls migcom
# directly for exactly this reason (cmake/mig.cmake).
#
# The i386 half below is untouched and still uses the wrapper.
if(NOT UROS_TARGET_ARCH STREQUAL "i386")
  set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

  set(UROS_MIG_USERLAND_INCLUDES
      -I${UROS_ARCH_INCLUDE}
      -I${_K}
      -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

  set(_NS_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
  set(NS_MIG_GENERATED)
  add_mig_userland(${UROS_UAPI_DIR}/servers/netname.defs ${_NS_OUT} NS_MIG)
  add_mig_userland(${UROS_UAPI_DIR}/servers/service.defs ${_NS_OUT} NS_MIG)
  add_mig_userland(${UROS_UAPI_DIR}/mach/notify.defs     ${_NS_OUT} NS_MIG)

  # ⚠️ migcom writes both halves of a .defs from one parse, and the unused
  # half is NOT free: a server stub REFERENCES the do_<routine> the program
  # would have to implement, so compiling service_server.c into a program that
  # only CALLS service_checkin leaves do_service_checkin undefined.  So the
  # three files kept here are the three i386 has always built -- demultiplex
  # netname and notify, call service -- and the rest are generated and left
  # alone.
  list(REMOVE_ITEM NS_MIG_GENERATED
    ${_NS_OUT}/service_server.c
    ${_NS_OUT}/netname_user.c
    ${_NS_OUT}/notify_user.c)

  add_executable(name_server_bin
    netname.c nprocs.c nostdlib_stubs.c ${NS_MIG_GENERATED})

  target_compile_options(name_server_bin PRIVATE
    ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin -w
    -mstack-protector-guard=global -include stdint.h)

  target_compile_definitions(name_server_bin PRIVATE MACH_IPC_COMPAT=0)

  # ⚠️ ${_NS_OUT} first: netname.c includes "netname_server.h" and the stubs
  # include each other by quoted name.  The shared generated/ MIG directory is
  # not needed here -- this server's stubs are its own.
  target_include_directories(name_server_bin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${_NS_OUT}
    ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
    ${CMAKE_SOURCE_DIR}/src/lib/libmach
    ${CMAKE_BINARY_DIR}/generated/include
    ${CMAKE_SOURCE_DIR}/src/mach_services/include
    ${CMAKE_SOURCE_DIR}/uapi
    ${UROS_ARCH_INCLUDE}
    ${_K}
    ${CMAKE_SOURCE_DIR}/include)

  # By PATH, and with -u for the entry: see tests/boot_probe/CMakeLists.txt.
  target_link_libraries(name_server_bin PRIVATE $<TARGET_FILE:libmach>)
  add_dependencies(name_server_bin libmach mig_mach_public_headers)

  target_link_options(name_server_bin PRIVATE
    ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
    -Wl,-e,__start_mach
    -Wl,-u,__start_mach
    -Wl,--build-id=none)

  set_target_properties(name_server_bin PROPERTIES
    OUTPUT_NAME "name_server"
    RUNTIME_OUTPUT_DIRECTORY
      "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")

  return()
endif()

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
      -I${UROS_ARCH_INCLUDE}
      -I${CMAKE_SOURCE_DIR}/src/mach_kernel
      -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers
      -header  ${_hdr}
      -sheader ${_srvH}
      -user    ${_usrC}
      -server  ${_srvC}
      ${_defs}
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${_defs}
    COMMENT "MIG: ${STEM}.defs (name_server)"
    VERBATIM
  )
endmacro()

# 1) netname.defs — server stubs (netname_server demux)
mig_all(
  "${UROS_UAPI_DIR}/servers/netname.defs"
  netname)

# 2) notify.defs — server stubs (do_mach_notify_dead_name etc.)
mig_all(
  "${UROS_UAPI_DIR}/mach/notify.defs"
  notify)

# 3) service.defs — user stubs (service_checkin)
mig_all(
  "${UROS_UAPI_DIR}/servers/service.defs"
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
  ${UROS_ARCH_FLAGS} -msse -msse2
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
  # Kernel headers (mach/, device/)
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against Mach userspace libraries.  name_server is purely
# single-threaded (one mach_msg_server loop, no cthread_fork /
# pthread_create anywhere), so neither libcthreads nor libpthreads
# is linked — the gratuitous libcthreads dep was just template inertia.
target_link_libraries(name_server_bin PRIVATE
  -Wl,--start-group
  libmach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
target_link_options(name_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(name_server_bin PROPERTIES
  OUTPUT_NAME "name_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
