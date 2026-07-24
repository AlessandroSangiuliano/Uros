# libposix-uros — POSIX personality shim sitting under musl libc (#250).
#
# Built when -DUROS_BUILD_MUSL=ON.  Produces a static archive that the
# smoke test (and, from Phase 3 onward, every migrated server) links
# alongside libc-musl.a.

if(NOT UROS_BUILD_MUSL)
  return()
endif()

cmake_minimum_required(VERSION 3.16)
project(libposix-uros C ASM)

# -----------------------------------------------------------------------
# Phase 4 (#252): the signals client lives in this library and talks to
# proc_server via MIG.  Generate proc_user.c locally so every musl-
# linked server picks up the stubs by transitively linking us — they
# do not need their own MIG rule.
# -----------------------------------------------------------------------
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(PROC_DEFS   ${CMAKE_SOURCE_DIR}/src/servers/proc_server/proc.defs)
set(PROC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/proc_user.c)
set(PROC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/proc.h)

add_custom_command(
  OUTPUT ${PROC_USER_C} ${PROC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/proc_server
    -header ${PROC_USER_H}
    -sheader /dev/null
    -user ${PROC_USER_C}
    -server /dev/null
    ${PROC_DEFS}
  DEPENDS migcom ${PROC_DEFS}
  COMMENT "MIG: proc.defs (libposix-uros)"
  VERBATIM
)
set_source_files_properties(${PROC_USER_C} ${PROC_USER_H} PROPERTIES GENERATED TRUE)

# Phase 5 (#254): exec_server client stubs.  Same pattern as proc.defs
# above — generated locally, linked into libposix-uros, so any musl-
# linked server gets execve() routing through us.
set(EXEC_DEFS   ${CMAKE_SOURCE_DIR}/src/servers/exec_server/exec.defs)
set(EXEC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/exec_user.c)
set(EXEC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/exec.h)

add_custom_command(
  OUTPUT ${EXEC_USER_C} ${EXEC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/exec_server
    -header ${EXEC_USER_H}
    -sheader /dev/null
    -user ${EXEC_USER_C}
    -server /dev/null
    ${EXEC_DEFS}
  DEPENDS migcom ${EXEC_DEFS}
  COMMENT "MIG: exec.defs (libposix-uros)"
  VERBATIM
)
set_source_files_properties(${EXEC_USER_C} ${EXEC_USER_H} PROPERTIES GENERATED TRUE)

# #275.4-bis: char_server.defs user stubs for /dev/tty open + tty fd
# routing.  Same pattern as proc.defs / exec.defs above.
set(CHAR_DEFS   ${CMAKE_SOURCE_DIR}/src/servers/char_server/char_server.defs)
set(CHAR_USER_C ${CMAKE_CURRENT_BINARY_DIR}/char_server_user.c)
set(CHAR_USER_H ${CMAKE_CURRENT_BINARY_DIR}/char_server.h)

add_custom_command(
  OUTPUT ${CHAR_USER_C} ${CHAR_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/char_server
    -header ${CHAR_USER_H}
    -sheader /dev/null
    -user ${CHAR_USER_C}
    -server /dev/null
    ${CHAR_DEFS}
  DEPENDS migcom ${CHAR_DEFS}
  COMMENT "MIG: char_server.defs (libposix-uros)"
  VERBATIM
)
set_source_files_properties(${CHAR_USER_C} ${CHAR_USER_H} PROPERTIES GENERATED TRUE)

# Phase 6a (#256): i386_set_ldt MIG stubs.  The user side isn't
# generated anywhere else in the tree (mach_i386.defs only emits the
# in-kernel server impl by default), so we MIG it locally.
set(MACH_I386_DEFS   ${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386/mach_i386.defs)
set(MACH_I386_USER_C ${CMAKE_CURRENT_BINARY_DIR}/mach_i386_user.c)
set(MACH_I386_USER_H ${CMAKE_CURRENT_BINARY_DIR}/mach_i386.h)

add_custom_command(
  OUTPUT ${MACH_I386_USER_C} ${MACH_I386_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/i386
    -header ${MACH_I386_USER_H}
    -sheader /dev/null
    -user ${MACH_I386_USER_C}
    -server /dev/null
    ${MACH_I386_DEFS}
  DEPENDS migcom ${MACH_I386_DEFS}
  COMMENT "MIG: mach_i386.defs (libposix-uros)"
  VERBATIM
)
set_source_files_properties(${MACH_I386_USER_C} ${MACH_I386_USER_H} PROPERTIES GENERATED TRUE)

set(LIBPOSIX_UROS_SRCS
  syscall.c
  handlers.c
  posix_fd.c
  posix_time.c
  posix_exec_fds.c
  stubs.c
  signals.c
  exceptions.c
  posix_exec.c
  posix_wait.c
  posix_fork.c
  posix_tls.c
  posix_clone.c
  posix_futex.c
  ${PROC_USER_C}
  ${EXEC_USER_C}
  ${CHAR_USER_C}
  ${MACH_I386_USER_C}
  i386/fork_capture.S
  i386/clone_trampoline.S
)

add_library(libposix-uros STATIC ${LIBPOSIX_UROS_SRCS})

add_dependencies(libposix-uros mig_mach_public_headers)

target_compile_options(libposix-uros PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin
                          -ffreestanding -fno-stack-protector
                          -Wall -Wextra -Werror>
  ${UROS_ARCH_FLAGS}
)

# Phase 6b smoke (#257): the same -DUROS_PTHREAD_SMOKE=1 the user passes
# via CMAKE_C_FLAGS for the C side must also reach our trampoline asm.
# CMake doesn't forward CMAKE_C_FLAGS to ASM compilation, so we mirror
# the flag onto the ASM build line for this target only.
if(CMAKE_C_FLAGS MATCHES "UROS_PTHREAD_SMOKE")
  target_compile_options(libposix-uros PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-DUROS_PTHREAD_SMOKE=1>)
endif()

# Loosen warnings on the MIG-generated proc_user.c — migcom emits
# constructs (uninitialised stack out-args, K&R-ish casts) that trip
# -Wall/-Wextra unconditionally.  Same convention used by every other
# Uros client of MIG.
set_source_files_properties(${PROC_USER_C} PROPERTIES
  COMPILE_OPTIONS "-w"
)
set_source_files_properties(${EXEC_USER_C} PROPERTIES
  COMPILE_OPTIONS "-w"
)
set_source_files_properties(${CHAR_USER_C} PROPERTIES
  COMPILE_OPTIONS "-w"
)
set_source_files_properties(${MACH_I386_USER_C} PROPERTIES
  COMPILE_OPTIONS "-w"
)

# musl headers (errno.h, signal.h, ...) — published by Phase 1
# ExternalProject under build/src/contrib/musl-install/.
# In-tree proc/Mach headers come via the include paths below.
target_include_directories(libposix-uros PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/src/contrib/musl-install/include
)

# Internal-only includes for MIG output, kernel/mach RPC headers, etc.
target_include_directories(libposix-uros PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}        # generated proc.h / exec.h
  ${CMAKE_BINARY_DIR}/generated      # generated mach.h / mach_port.h (#276)
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/servers/proc_server
  ${CMAKE_SOURCE_DIR}/src/servers/exec_server
  ${CMAKE_SOURCE_DIR}/src/servers/char_server
  ${CMAKE_SOURCE_DIR}/src/lib/libvfs   # libvfs.h, vfs_types.h (#262)
)

# Ensure musl has been built before we try to compile against its
# headers.
if(TARGET musl_extproj)
  add_dependencies(libposix-uros musl_extproj)
endif()

set_target_properties(libposix-uros PROPERTIES
  OUTPUT_NAME "posix-uros"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)

# ---------------------------------------------------------------------------
# PIC variant for the umbrella libc.so (#234 incr 2b).
#
# Same sources and flags as libposix-uros, compiled position-independent so
# it can be folded into musl's libc.so via EXTRA_OBJS — the dynamic linker
# (== libc.so) thus carries the real __uros_syscall dispatcher + Mach/VFS
# stack.  The static system servers keep using the non-PIC libposix-uros, so
# their proven path is untouched.  Build/include flags are copied from the
# base target so the two never drift.
# ---------------------------------------------------------------------------
# The -pic archive carries an extra TU (dyn_bootstrap.c) that registers
# mach_init() as a constructor — picked up by ld-musl's __init_array
# walker so name_server_port et al. are valid before main() in dynamic
# tasks (#234 incr 4).  Static binaries call mach_init() explicitly and
# do not need it.
add_library(libposix-uros-pic STATIC ${LIBPOSIX_UROS_SRCS} dyn_bootstrap.c)
add_dependencies(libposix-uros-pic mig_mach_public_headers)
if(TARGET musl_extproj)
  add_dependencies(libposix-uros-pic musl_extproj)
endif()
set_target_properties(libposix-uros-pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(libposix-uros-pic PRIVATE
  $<TARGET_PROPERTY:libposix-uros,COMPILE_OPTIONS>)
target_include_directories(libposix-uros-pic PRIVATE
  $<TARGET_PROPERTY:libposix-uros,INCLUDE_DIRECTORIES>)
set_target_properties(libposix-uros-pic PROPERTIES OUTPUT_NAME "posix-uros-pic")
