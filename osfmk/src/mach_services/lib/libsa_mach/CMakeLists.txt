cmake_minimum_required(VERSION 3.16)
project(libsa_mach C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" )

# Arch specific i386 sources, preprocess lowercase .s
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.S")
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "i386/*.s")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
  # Preprocess lowercase assembler files to generated/asm
  # __NO_UNDERSCORES__: ELF does not prefix symbols with '_'
  set(ASM_PREPROCESS_INCLUDES "-D__NO_UNDERSCORES__ -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/export/powermac/include -I${CMAKE_SOURCE_DIR}/include")
  foreach(sfile IN LISTS ARCH_S_LOWER_S)
    get_filename_component(sname ${sfile} NAME)
    set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/asm")
    add_custom_command(
      OUTPUT ${pp_output}
      COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} | sed -e 's#/ # #g' -e 's/\.align ALIGN/.align 2/g' -e 's/^ \+zero /# zero /g' > ${pp_output}"
      DEPENDS ${sfile}
      COMMENT "Preprocessing asm ${sfile} and converting comments"
      VERBATIM
    )
    set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
    list(APPEND LIB_SOURCES ${pp_output})
  endforeach()


endif()

add_library(libsa_mach STATIC ${LIB_SOURCES})

# printf.c includes <device/device.h> which is MIG-generated
add_dependencies(libsa_mach mig_device_header)


# Apply C-only compatibility options and include math.h from project
# math.h wrapper includes float.h and stdint.h
# -m32 -march=i686 for cross-compilation
# -w disables warnings
# -fno-builtin disables builtins
# -D_BSD for BSD compatibility
# -std=gnu11 for legacy C
#
target_compile_options(libsa_mach PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -D_BSD -fno-builtin -w -m32 -march=i686 -msse -msse2 -mstack-protector-guard=global -include math.h>)
target_compile_options(libsa_mach PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-m32 -march=i686>)

# Work around GCC constprop + SSP + va_list code generation bug on i386:
# GCC creates a constprop clone of _doprnt where the va_list calling
# convention is inconsistent between the inlined printf and standalone
# vprintf callers.  Disable SSP for printf.c until GCC is fixed.
set_source_files_properties(printf.c PROPERTIES COMPILE_FLAGS -fno-stack-protector)

# Mach userspace headers (sa_mach) solo per questa libreria
target_include_directories(libsa_mach BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
)
# Prefer project includes first so project headers are used instead of system headers
target_include_directories(libsa_mach BEFORE PUBLIC
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libsa_mach PROPERTIES
  OUTPUT_NAME "sa_mach"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
