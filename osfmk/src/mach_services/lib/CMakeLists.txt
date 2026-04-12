cmake_minimum_required(VERSION 3.16)


# Create small generated forwarding headers so legacy includes like
# "machine/types.h" and <machine/va_list.h> resolve to the
# architecture-specific headers (use i386 for libsa_mach builds).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/machine")
set(GEN_MACHINE_TYPES "${CMAKE_BINARY_DIR}/generated/include/machine/types.h")
set(GEN_MACHINE_VALIST "${CMAKE_BINARY_DIR}/generated/include/machine/va_list.h")
file(WRITE ${GEN_MACHINE_TYPES} "#ifndef _GEN_MACHINE_TYPES_H_\n#define _GEN_MACHINE_TYPES_H_\n#include <sa_mach/i386/types.h>\n#endif\n")
file(WRITE ${GEN_MACHINE_VALIST} "#ifndef _GEN_MACHINE_VALIST_H_\n#define _GEN_MACHINE_VALIST_H_\n#include <sa_mach/i386/va_list.h>\n#endif\n")

# Path to the mig shell script (handles cc -E | migcom pipeline)
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)

# Helper: genera i file MIG (.c/.h) usando lo script mig originale.
# Uso: add_mig_defs(path/to/file.defs INCLUDES...)
function(add_mig_defs DEFS_FILE)
  get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)

  set(USER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_user.c)
  set(SERVER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_server.c)
  # Place MIG headers alongside the .c files (NOT in generated/include/)
  # to avoid shadowing real userspace headers like mach.h.
  # When both -header and -sheader are given, migcom makes _user.c include
  # the -header file and _server.c include the -sheader file.
  set(USER_H   ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}.h)
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
    DEPENDS migcom ${DEFS_FILE}
    COMMENT "MIG: ${DEFS_NAME}"
    VERBATIM
  )

  # Espongo solamente i file .c generati come sorgenti della libreria (l'header viene generato ma non è sorgente)
  set(${DEFS_NAME}_MIG_GENERATED ${USER_C} ${SERVER_C} PARENT_SCOPE)
endfunction()

# Tool host principali: migcom (e in seguito mig se necessario).
add_subdirectory(migcom)

# Generate device/device.h from device.defs via MIG (shared by libmach, libflipc, etc.)
set(DEVICE_DEFS "${CMAKE_SOURCE_DIR}/export/powermac/include/device/device.defs")
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
    -I${CMAKE_SOURCE_DIR}/export/powermac/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${DEVICE_USER_H}
    -sheader ${DEVICE_SERVER_H}
    -user ${DEVICE_USER_C}
    -server ${DEVICE_SERVER_C}
    ${DEVICE_DEFS}
  DEPENDS migcom ${DEVICE_DEFS}
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

# Librerie mach_services (ogni libreria è un target statico separato)

# Aggiungi sottodirectory solo se esistono per non rompere la riconfigurazione
macro(maybe_add DIR)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}/CMakeLists.txt")
    add_subdirectory(${DIR})
  endif()
endmacro()

maybe_add(libmach)
maybe_add(libmach_p)
maybe_add(libmach_maxonstack)
maybe_add(libsa_mach)
maybe_add(libsa_mach_p)
maybe_add(libmachid)
maybe_add(libcthreads)
maybe_add(libcthreads_p)
maybe_add(librthreads)
maybe_add(libpthreads)
#maybe_add(libxmm)  # excluded for now: needs K&R modernization
maybe_add(libservice)
maybe_add(libnetmemory)
maybe_add(libnetname)
maybe_add(libblk)
maybe_add(libflipc)
maybe_add(libflipc2)
maybe_add(libdl)

