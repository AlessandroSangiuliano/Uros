cmake_minimum_required(VERSION 3.16)
project(libmach C)

# Raccolgo le sorgenti locali
file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")

# Aggiungo sorgenti specifiche per architettura i386 se presenti
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.S")
  # Collect lower-case .s files separately and make them run through the C preprocessor
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "i386/*.s")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
  set(ASM_PREPROCESS_INCLUDES "-I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/export/powermac/include -I${CMAKE_SOURCE_DIR}/include")
  foreach(sfile IN LISTS ARCH_S_LOWER_S)
    # Preprocess assembler with the C preprocessor so we can define __ASSEMBLER__ and pass includes
    get_filename_component(sname ${sfile} NAME)
    set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/asm")
    add_custom_command(
      OUTPUT ${pp_output}
      COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\.align ALIGN/.align 2/g' -e 's/^ \+zero /# zero /g' > ${pp_output}"
      DEPENDS ${sfile}
      COMMENT "Preprocessing asm ${sfile} and converting '/' comments to '#'"
      VERBATIM
    )
    set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
    list(APPEND LIB_SOURCES ${pp_output})
  endforeach()
endif()


add_library(libmach STATIC ${LIB_SOURCES})

# Depend on MIG-generated device/device.h (defined in parent CMakeLists)
add_dependencies(libmach mig_device_header)

# Compat: il codice è storico (K&R/gnu89); impostiamo opzioni less strict per compilare
# Apply C-only options (including -include stdint.h) only when compiling C sources
target_compile_options(libmach PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu89 -D_BSD -fno-builtin -w -include stdint.h>)
# Build assembly files and C as 32-bit where needed
target_compile_options(libmach PRIVATE -m32 -march=i686)

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



# Forza la generazione di sync_user.c da sync.defs (Mach API sync)
set(SYNC_DEFS "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/sync.defs")
add_mig_defs(${SYNC_DEFS} ${CMAKE_SOURCE_DIR}/export/powermac/include)
set(SYNC_USER_C "${CMAKE_BINARY_DIR}/generated/sync_user.c")
set(SYNC_SERVER_C "${CMAKE_BINARY_DIR}/generated/sync_server.c")
list(APPEND LIB_SOURCES ${SYNC_USER_C} ${SYNC_SERVER_C})

# Mantieni anche la generazione per tutte le altre .defs kernel
file(GLOB KERNEL_MIG_DEFS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/*.defs")
foreach(DEFS IN LISTS KERNEL_MIG_DEFS)
  add_mig_defs(${DEFS} ${CMAKE_SOURCE_DIR}/src/mach_kernel ${CMAKE_SOURCE_DIR}/src/mach_kernel/mach ${CMAKE_SOURCE_DIR}/export/powermac/include)
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

# Metto gli archivi in export/<system>/lib per compatibilità con l'albero esistente
set_target_properties(libmach PROPERTIES
  OUTPUT_NAME "mach"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
