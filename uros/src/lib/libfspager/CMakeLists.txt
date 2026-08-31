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
set(MO_DEFS     "${CMAKE_SOURCE_DIR}/uapi/mach/memory_object.defs")
set(MO_USER_C   "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_user.c")
set(MO_SERVER_C "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_server.c")
set(MO_USER_H   "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object.h")
set(MO_SERVER_H "${CMAKE_BINARY_DIR}/generated/libfspager_memory_object_server.h")

add_custom_command(
  OUTPUT ${MO_USER_C} ${MO_SERVER_C} ${MO_USER_H} ${MO_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    ${UROS_MIG_TARGET_ARGS}
    -DSEQNOS
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header  ${MO_USER_H}
    -sheader ${MO_SERVER_H}
    -user    ${MO_USER_C}
    -server  ${MO_SERVER_C}
    ${MO_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${MO_DEFS}
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
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin 
                          -mstack-protector-guard=global
                          -include stdint.h ${UROS_ARCH_FLAGS}>
)

# #443 part C: this used to carry a TypeCheck=0 exception for the generated
# memory_object server stubs.  Its recorded reason was that the host-compiled
# .defs predicted a message shape the kernel does not send — which is exactly
# what #416 removed, by having migcom ask the target compiler for the sizes
# instead of taking sizeof on the host.  The reason expired, so the exception
# goes: these stubs now check their arguments like the other 179 in userland.
#
# ⚠️ Removed on the static argument, NOT on a passing run.  This paragraph used
# to go on to say that a green boot proved nothing because "the path is not
# entered, so no check can fire", the only client of file-backed mmap being an
# h_mmap2 that still handled MAP_ANONYMOUS alone.  That was already wrong when
# it was written: dlopen_test enters this path on every i386 boot, and the check
# fired there — a memory_object_init refused for a trailer too short to carry a
# seqno, which is where every dlopen died until ext_server asked for one.
#
# 🔑 The prediction it made about the shape of the failure was wrong in the
# useful direction.  It said a userland stub has no equivalent of the kernel's
# MIG_CHECK_FAILED diagnostic and would surface as a bare MIG_BAD_ARGUMENTS.
# #443 gave userland that diagnostic, so the console named the routine and the
# check instead:
#
#     mig: memory_object_init refused a message: seqno
#
# which is the difference between an afternoon and a month.  That is the whole
# argument for leaving the checks on, and it has now been collected rather than
# merely asserted.

target_include_directories(libfspager PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  # generated/ exposes mach_interface.h, mach_port.h (user stubs vm_allocate,
  # memory_object_data_supply, mach_port_*).
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # <string.h> lives in sa_mach/, not in the directory above it (#506,
  # same class as #495): without this root it came from the host's glibc.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libfspager PROPERTIES
  OUTPUT_NAME "fspager"
  ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
