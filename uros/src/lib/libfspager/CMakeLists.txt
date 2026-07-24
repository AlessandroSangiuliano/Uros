cmake_minimum_required(VERSION 3.16)
project(libfspager C)

# libfspager — Mach memory_object pager protocol as a server-side library
# shared across fs servers (#276).  Phase A: scaffold + MIG handler stubs.
#
# We generate seqnos_memory_object_server (the demux) from
# <mach/memory_object.defs> with -DSEQNOS; libfspager.c provides every
# handler the demux dispatches to (stubs in Phase A, real bodies in B-D).

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)

# memory_object.defs lives in the public Mach include tree.  We only
# care about the *server* side (the pager).  No user stub needed by
# libfspager itself — fs servers that want to *issue* data_supply etc.
# pull in the user stubs from libmach.
set(MO_DEFS     "${CMAKE_SOURCE_DIR}/export/include/mach/memory_object.defs")
set(MO_USER_C   "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_user.c")
set(MO_SERVER_C "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_server.c")
set(MO_USER_H   "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object.h")
set(MO_SERVER_H "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_server.h")

add_custom_command(
  OUTPUT ${MO_USER_C} ${MO_SERVER_C} ${MO_USER_H} ${MO_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -DSEQNOS
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header  ${MO_USER_H}
    -sheader ${MO_SERVER_H}
    -user    ${MO_USER_C}
    -server  ${MO_SERVER_C}
    ${MO_DEFS}
  DEPENDS migcom ${MO_DEFS}
  COMMENT "MIG: memory_object.defs (libfspager, -DSEQNOS)"
  VERBATIM
)

set_source_files_properties(
  ${MO_USER_C} ${MO_SERVER_C} ${MO_USER_H} ${MO_SERVER_H}
  PROPERTIES GENERATED TRUE
)

add_library(libfspager STATIC
  libfspager.c
  ${MO_SERVER_C}
)

add_dependencies(libfspager mig_mach_public_headers)

target_compile_options(libfspager PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w
                          -mstack-protector-guard=global
                          -include stdint.h ${UROS_ARCH_FLAGS}>
)

# Disable MIG's hardcoded msgh_size / disposition TypeCheck on the
# generated server stubs.  The kernel's memory_object_init message
# shape may differ slightly from what the host-compiled .defs predict
# (different Mach branches send slightly different trailers / port
# dispositions); without this every well-formed call silently fails
# MIG_BAD_ARGUMENTS in the wrapper before our seqnos_* handler runs.
set_source_files_properties(${MO_SERVER_C} PROPERTIES
  COMPILE_DEFINITIONS "TypeCheck=0")

target_include_directories(libfspager PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  # generated/ exposes mach.h, mach_port.h (user stubs vm_allocate,
  # memory_object_data_supply, mach_port_*).
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libfspager PROPERTIES
  OUTPUT_NAME "fspager"
  ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
