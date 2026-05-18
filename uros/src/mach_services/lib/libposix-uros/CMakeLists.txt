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
set(PROC_DEFS   ${CMAKE_SOURCE_DIR}/src/proc_server/proc.defs)
set(PROC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/proc_user.c)
set(PROC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/proc.h)

add_custom_command(
  OUTPUT ${PROC_USER_C} ${PROC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/proc_server
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
set(EXEC_DEFS   ${CMAKE_SOURCE_DIR}/src/exec_server/exec.defs)
set(EXEC_USER_C ${CMAKE_CURRENT_BINARY_DIR}/exec_user.c)
set(EXEC_USER_H ${CMAKE_CURRENT_BINARY_DIR}/exec.h)

add_custom_command(
  OUTPUT ${EXEC_USER_C} ${EXEC_USER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_BINARY_DIR}/generated/include
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/exec_server
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

add_library(libposix-uros STATIC
  syscall.c
  handlers.c
  stubs.c
  signals.c
  exceptions.c
  posix_exec.c
  posix_wait.c
  ${PROC_USER_C}
  ${EXEC_USER_C}
  i386/traps.S
  i386/sig_thread_entry.S
)

add_dependencies(libposix-uros mig_mach_public_headers)

target_compile_options(libposix-uros PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -m32 -march=i686 -fno-builtin
                          -ffreestanding -fno-stack-protector
                          -Wall -Wextra -Werror>
  $<$<COMPILE_LANGUAGE:ASM>:-m32>
)

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
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/proc_server
  ${CMAKE_SOURCE_DIR}/src/exec_server
)

# Ensure musl has been built before we try to compile against its
# headers.
if(TARGET musl_extproj)
  add_dependencies(libposix-uros musl_extproj)
endif()

set_target_properties(libposix-uros PROPERTIES
  OUTPUT_NAME "posix-uros"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
