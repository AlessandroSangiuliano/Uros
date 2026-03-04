cmake_minimum_required(VERSION 3.16)
project(libmach C)

# Raccolgo le sorgenti locali (only .c and .S; lowercase .s needs preprocessing)
file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S")
file(GLOB TOP_S_LOWER_S CONFIGURE_DEPENDS "*.s")

# Exclude ms_*.c wrappers: they call mig_<name>() variants that require a sed-based
# post-processing rename of MIG output (the original common.mk did this).  Without
# the rename, the MIG stubs provide the public names (vm_allocate, etc.) directly,
# which is sufficient for the bootstrap server and standalone Mach programs.
file(GLOB MS_WRAPPERS CONFIGURE_DEPENDS "ms_*.c")
list(REMOVE_ITEM LIB_SOURCES ${MS_WRAPPERS})

# __NO_UNDERSCORES__: ELF does not prefix symbols with '_' (unlike Mach-O/a.out)
set(ASM_PREPROCESS_INCLUDES "-D__NO_UNDERSCORES__ -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/export/powermac/include -I${CMAKE_SOURCE_DIR}/include")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/asm")

# Preprocess ALL lowercase .s files (they use #include and need CPP)
foreach(sfile IN LISTS TOP_S_LOWER_S)
  get_filename_component(sname ${sfile} NAME)
  set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
  add_custom_command(
    OUTPUT ${pp_output}
    COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\\.align ALIGN/.align 2/g' -e 's/^ \\+zero /# zero /g' > ${pp_output}"
    DEPENDS ${sfile}
    COMMENT "Preprocessing asm ${sfile}"
    VERBATIM
  )
  set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
  list(APPEND LIB_SOURCES ${pp_output})
endforeach()

# Aggiungo sorgenti specifiche per architettura i386 se presenti
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.S")
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "i386/*.s")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
  foreach(sfile IN LISTS ARCH_S_LOWER_S)
    get_filename_component(sname ${sfile} NAME)
    set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
    add_custom_command(
      OUTPUT ${pp_output}
      COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\\.align ALIGN/.align 2/g' -e 's/^ \\+zero /# zero /g' > ${pp_output}"
      DEPENDS ${sfile}
      COMMENT "Preprocessing asm ${sfile}"
      VERBATIM
    )
    set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
    list(APPEND LIB_SOURCES ${pp_output})
  endforeach()
endif()

# Generate MIG user stubs from kernel .defs BEFORE creating the library target
# (add_library snapshots the source list; appending afterwards has no effect)
#
# Explicit list matching the original common.mk MIG_DEFS (17 .defs files).
# Do NOT glob — the kernel tree contains extra .defs (prof.defs, etc.) that
# either fail MIG parsing or are not used by userspace.

set(_KERNEL ${CMAKE_SOURCE_DIR}/src/mach_kernel)
set(_EXPORT ${CMAKE_SOURCE_DIR}/export/powermac/include)
# MIG include path: export tree first (userspace .defs), then kernel for extras
set(_MIG_INC ${_EXPORT} ${_KERNEL} ${_EXPORT}/mach_debug)

# --- All .defs from the export tree (matching original common.mk MIG_DEFS) ---
# Use the export tree versions which are the published userspace interfaces.
# The kernel tree's .defs have kernel-internal definitions (alarm_port_t, SEQNOS
# conditionals, etc.) that produce incompatible MIG output for userspace code.
set(EXPORT_MIG_DEFS
  ${_EXPORT}/mach/bootstrap.defs
  ${_EXPORT}/mach/clock.defs
  ${_EXPORT}/mach/clock_reply.defs
  ${_EXPORT}/mach/default_pager_object.defs
  ${_EXPORT}/mach/exc.defs
  ${_EXPORT}/mach/ledger.defs
  ${_EXPORT}/mach/mach.defs
  ${_EXPORT}/mach/mach_host.defs
  ${_EXPORT}/mach/mach_norma.defs
  ${_EXPORT}/mach/mach_port.defs
  ${_EXPORT}/mach/memory_object.defs
  ${_EXPORT}/mach/notify.defs
  ${_EXPORT}/mach/sync.defs
  ${_EXPORT}/mach_debug/mach_debug.defs
  ${_EXPORT}/device/device_request.defs
  ${_EXPORT}/device/device_reply.defs
)
foreach(DEFS IN LISTS EXPORT_MIG_DEFS)
  add_mig_defs(${DEFS} ${_MIG_INC})
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

# device.defs: the parent CMakeLists already generates device_user.c / device_server.c
# (and the public device/device.h header) via a standalone custom command, so we
# just reference those outputs here to avoid duplicate-rule errors.
list(APPEND LIB_SOURCES
  ${CMAKE_BINARY_DIR}/generated/device_user.c
  ${CMAKE_BINARY_DIR}/generated/device_server.c
)
set_source_files_properties(
  ${CMAKE_BINARY_DIR}/generated/device_user.c
  ${CMAKE_BINARY_DIR}/generated/device_server.c
  PROPERTIES GENERATED TRUE
)

add_library(libmach STATIC ${LIB_SOURCES})

# Depend on MIG-generated device/device.h (defined in parent CMakeLists)
add_dependencies(libmach mig_device_header)

# Compat: il codice è storico (K&R/gnu89); impostiamo opzioni less strict per compilare
# Apply C-only options (including -include stdint.h) only when compiling C sources
target_compile_options(libmach PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -D_BSD -fno-builtin -w -mstack-protector-guard=global -include stdint.h>)
# Build assembly files and C as 32-bit where needed
target_compile_options(libmach PRIVATE -m32 -march=i686 -msse -msse2)

# Mach userspace headers (sa_mach) solo per questa libreria
target_include_directories(libmach BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
)
target_include_directories(libmach PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

# Metto gli archivi in export/<system>/lib per compatibilità con l'albero esistente
set_target_properties(libmach PROPERTIES
  OUTPUT_NAME "mach"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
