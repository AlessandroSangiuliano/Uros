cmake_minimum_required(VERSION 3.16)

# Ensure Mach-specific includes (e.g., sa_mach/stdio.h) are found before
# the host's system headers so plain `#include <stdio.h>` will pick up the
# project-provided Mach implementation when building mach_services.
# This is a directory-level setting applied before adding subdirectories.
include_directories(BEFORE "${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach")

# Create small generated forwarding headers so legacy includes like
# "machine/types.h" and <machine/va_list.h> resolve to the
# architecture-specific headers (use i386 for libsa_mach builds).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include/machine")
set(GEN_MACHINE_TYPES "${CMAKE_BINARY_DIR}/generated/include/machine/types.h")
set(GEN_MACHINE_VALIST "${CMAKE_BINARY_DIR}/generated/include/machine/va_list.h")
file(WRITE ${GEN_MACHINE_TYPES} "#ifndef _GEN_MACHINE_TYPES_H_\n#define _GEN_MACHINE_TYPES_H_\n#include <sa_mach/i386/types.h>\n#endif\n")
file(WRITE ${GEN_MACHINE_VALIST} "#ifndef _GEN_MACHINE_VALIST_H_\n#define _GEN_MACHINE_VALIST_H_\n#include <sa_mach/i386/va_list.h>\n#endif\n")

# Helper: genera i file MIG (.c/.h) usando il tool migcom costruito in questo progetto.
# Uso: add_mig_defs(path/to/file.defs INCLUDES...)
function(add_mig_defs DEFS_FILE)
  get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)

  set(USER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_user.c)
  set(SERVER_C ${CMAKE_BINARY_DIR}/generated/${DEFS_NAME}_server.c)
  set(HEADER ${CMAKE_BINARY_DIR}/generated/include/${DEFS_NAME}.h)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated/include)

  # Segnala a CMake che i file sorgente saranno generati (evita errori "cannot find source file")
  set_source_files_properties(${USER_C} ${SERVER_C} ${HEADER} PROPERTIES GENERATED TRUE)

  # Evita di registrare più volte lo stesso .defs (previene errori CMake)
  set(PROCESSED_VAR "MIG_PROCESSED_${DEFS_NAME}")
  if(DEFINED ${PROCESSED_VAR})
    message(VERBOSE "MIG: ${DEFS_NAME} already processed, reusing outputs")
    # Espongo comunque la variabile _MIG_GENERATED nel contesto chiamante (i path sono deterministici)
    set(${DEFS_NAME}_MIG_GENERATED ${USER_C} ${SERVER_C} PARENT_SCOPE)
    # Assicuro che le proprietà GENERATED siano visibili anche in altri scope
    set_source_files_properties(${USER_C} ${SERVER_C} ${HEADER} PROPERTIES GENERATED TRUE)
    return()
  endif()
  set(${PROCESSED_VAR} TRUE CACHE INTERNAL "MIG rules added for ${DEFS_NAME}")

  set(INC_FLAGS)
  foreach(inc IN LISTS ARGN)
    list(APPEND INC_FLAGS "-I${inc}")
  endforeach()

  add_custom_command(
    OUTPUT ${USER_C} ${SERVER_C} ${HEADER}
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c ${INC_FLAGS} ${DEFS_FILE} | $<TARGET_FILE:migcom> -sheader ${HEADER} -server ${SERVER_C} -user ${USER_C}
    DEPENDS migcom ${DEFS_FILE}
    COMMENT "MIG: ${DEFS_NAME}"
    VERBATIM
  )

  # Espongo solamente i file .c generati come sorgenti della libreria (l'header viene generato ma non è sorgente)
  set(${DEFS_NAME}_MIG_GENERATED ${USER_C} ${SERVER_C} PARENT_SCOPE)
endfunction()

# Tool host principali: migcom (e in seguito mig se necessario).
add_subdirectory(migcom)

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
maybe_add(libxmm)
maybe_add(libservice)
maybe_add(libnetmemory)
maybe_add(libnetname)
maybe_add(libflipc)


