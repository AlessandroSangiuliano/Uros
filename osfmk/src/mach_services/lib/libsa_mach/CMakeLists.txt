cmake_minimum_required(VERSION 3.16)
project(libsa_mach C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" )

# Arch specific i386 sources, preprocess lowercase .s
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.S")
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "i386/*.s")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
  # Preprocess lowercase assembler files to generated/asm
  set(ASM_PREPROCESS_INCLUDES "-I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/export/powermac/include -I${CMAKE_SOURCE_DIR}/include")
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

# Apply C-only compatibility options and include stdint.h
target_compile_options(libsa_mach PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu89 -D_BSD -fno-builtin -w -include stdint.h>)
# Build 32-bit when needed
target_compile_options(libsa_mach PRIVATE -m32 -march=i686)

target_include_directories(libsa_mach PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libsa_mach PROPERTIES
  OUTPUT_NAME "sa_mach"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${CMAKE_SYSTEM_NAME}/lib"
)
