cmake_minimum_required(VERSION 3.16)

# uros/src/lib — the Uros userspace libraries (#396).
#
# This subtree owns the actively-developed libraries and the MIG tooling
# they are generated with.  The genuinely inherited OSF libraries stay in
# src/mach_services/lib; this directory is added first from src/CMakeLists
# so migcom, add_mig_defs() and the generated headers exist before anything
# that consumes them.

# Build every userspace static library in this subtree with -fPIC so they
# can be linked into PIE (ET_DYN) servers.  See #164 / #166.  For plain
# ET_EXEC servers the cost is negligible (a handful of extra instructions
# in the rarely-executed prologue); we'll flip each server to -pie in
# follow-up issues (Phase 3+ of #164).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 🔥 THE TWO FORWARDING HEADERS THIS USED TO WRITE ARE GONE (#481).
#
# It wrote generated/include/machine/{types,va_list}.h, each a wrapper naming
# sa_mach/i386/ outright, into a directory 489 units have on their include path
# ahead of src/mach_services/include.  The wrappers of the same name that live
# there choose the architecture from the compiler's own macro -- which is what
# #426 changed them to do, after a userland built for another machine got
# i386's varargs in silence.
#
# So for 477 of those 489 units the repair was not in force: a second file,
# with the architecture frozen at configure time, answered first.  Writing a
# header into a shared include directory is the same move that made #434's
# clean build fail, and it is removed for the same reason.

# Path to the mig shell script (handles cc -E | migcom pipeline)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)

# Helper: genera i file MIG (.c/.h) usando lo script mig originale.
# Uso: add_mig_defs(path/to/file.defs INCLUDES...)
function(add_mig_defs DEFS_FILE)
  get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)

  mig_user_header_name(${DEFS_NAME} USER_H_NAME)

  set(USER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_user.c)
  set(SERVER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_server.c)
  # When both -header and -sheader are given, migcom makes _user.c include
  # the -header file and _server.c include the -sheader file.
  set(USER_H   ${CMAKE_BINARY_DIR}/generated/${USER_H_NAME}.h)
  set(SERVER_H ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_server.h)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

  set_source_files_properties(${USER_C} ${SERVER_C} ${USER_H} ${SERVER_H} PROPERTIES GENERATED TRUE)

  # Evita di registrare più volte lo stesso .defs (previene errori CMake).
  # Usa GLOBAL PROPERTY (non CACHE) così si azzera a ogni cmake run.
  get_property(_mig_processed GLOBAL PROPERTY MIG_PROCESSED_${DEFS_NAME})
  if(_mig_processed)
    message(VERBOSE "MIG: ${DEFS_NAME} already processed, reusing outputs")
    set(${DEFS_NAME}_MIG_GENERATED ${USER_C} ${SERVER_C} PARENT_SCOPE)
    set_source_files_properties(${USER_C} ${SERVER_C} ${USER_H} ${SERVER_H} PROPERTIES GENERATED TRUE)
    return()
  endif()
  set_property(GLOBAL PROPERTY MIG_PROCESSED_${DEFS_NAME} TRUE)

  # ⚠️ uapi is NOT added here (#471).  scripts/mig adds it, which is the one
  # place every invocation on this side funnels through -- including the
  # servers that build their MIG rules by hand and never call this function.
  set(INC_FLAGS)
  foreach(inc IN LISTS ARGN)
    list(APPEND INC_FLAGS -I${inc})
  endforeach()

  add_custom_command(
    OUTPUT ${USER_C} ${SERVER_C} ${USER_H} ${SERVER_H}
    COMMAND ${MIG_SCRIPT}
      -cpp ${CMAKE_C_COMPILER}
      -migcom $<TARGET_FILE:migcom>
      ${INC_FLAGS}
      -header ${USER_H}
      -sheader ${SERVER_H}
      -server ${SERVER_C}
      -user ${USER_C}
      ${DEFS_FILE}
    DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEFS_FILE}
    COMMENT "MIG: ${DEFS_NAME}"
    VERBATIM
  )

  # Espongo solamente i file .c generati come sorgenti della libreria (l'header viene generato ma non è sorgente)
  set(${DEFS_NAME}_MIG_GENERATED ${USER_C} ${SERVER_C} PARENT_SCOPE)
endfunction()

# Tool host principali: migcom (e in seguito mig se necessario).
add_subdirectory(migcom)

# Generate device/device.h from device.defs via MIG (shared by libmach, libflipc2, etc.)
set(DEVICE_DEFS "${UROS_UAPI_DIR}/device/device.defs")
# Public header for #include <device/device.h>
set(DEVICE_HEADER "${CMAKE_BINARY_DIR}/generated/include/device/device.h")
# Co-located headers for MIG .c files (#include "device.h" / "device_server.h")
set(DEVICE_USER_H "${CMAKE_BINARY_DIR}/generated/device.h")
set(DEVICE_SERVER_H "${CMAKE_BINARY_DIR}/generated/device_server.h")
set(DEVICE_USER_C "${CMAKE_BINARY_DIR}/generated/device_user.c")
set(DEVICE_SERVER_C "${CMAKE_BINARY_DIR}/generated/device_server.c")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/device")

# Generate both user and server headers using the mig script.
add_custom_command(
  OUTPUT ${DEVICE_USER_H} ${DEVICE_SERVER_H} ${DEVICE_USER_C} ${DEVICE_SERVER_C}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DEVICE_USER_H}
    -sheader ${DEVICE_SERVER_H}
    -user ${DEVICE_USER_C}
    -server ${DEVICE_SERVER_C}
    ${DEVICE_DEFS}
  DEPENDS migcom ${UROS_MIG_COMMON_DEFS} ${DEVICE_DEFS}
  COMMENT "MIG: device.defs -> device.h + device_server.h"
  VERBATIM
)
# Copy the user header to the public include path so #include <device/device.h> works
add_custom_command(
  OUTPUT ${DEVICE_HEADER}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEVICE_USER_H} ${DEVICE_HEADER}
  DEPENDS ${DEVICE_USER_H}
  COMMENT "Copy device.h -> generated/include/device/device.h"
)
add_custom_target(mig_device_header DEPENDS ${DEVICE_HEADER})

# Aggiungi sottodirectory solo se esistono per non rompere la riconfigurazione
macro(maybe_add DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}/CMakeLists.txt")
    add_subdirectory(${DIR})
  endif()
endmacro()

maybe_add(libmach)
maybe_add(libnetname)
maybe_add(libpthreads)
maybe_add(libblk)

maybe_add(libflipc2)
maybe_add(libvfs)
maybe_add(libelf)
maybe_add(libdl)
maybe_add(libmodload)
maybe_add(libcap)
maybe_add(libfspager)
maybe_add(libgpu_console)
maybe_add(libposix-uros)
