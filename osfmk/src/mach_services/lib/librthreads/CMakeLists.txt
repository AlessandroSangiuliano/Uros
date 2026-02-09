cmake_minimum_required(VERSION 3.16)
project(librthreads C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AT386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "AT386/*.c" "AT386/*.S")
  file(GLOB ARCH_S_LOWER_S CONFIGURE_DEPENDS "AT386/*.s")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})

  set(ASM_PREPROCESS_INCLUDES "-I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_BINARY_DIR}/generated/include -I${CMAKE_SOURCE_DIR}/src/mach_services/include -I${CMAKE_SOURCE_DIR}/src/mach_kernel -I${CMAKE_SOURCE_DIR}/export/powermac/include -I${CMAKE_SOURCE_DIR}/include")
  foreach(sfile IN LISTS ARCH_S_LOWER_S)
    get_filename_component(sname ${sfile} NAME)
    set(pp_output "${CMAKE_CURRENT_BINARY_DIR}/generated/asm/${sname}.S")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/asm")
    add_custom_command(
      OUTPUT ${pp_output}
      COMMAND /bin/sh -c "${CMAKE_C_COMPILER} -E -x assembler-with-cpp -D__ASSEMBLER__ ${ASM_PREPROCESS_INCLUDES} ${sfile} \
        | sed '/\/\*/,/\*\//d' \
        | sed -e 's#/ # #g' -e 's/\.align ALIGN/.align 2/g' -e 's/^ \+zero /# zero /g' > ${pp_output}"
      DEPENDS ${sfile}
      COMMENT "Preprocessing asm ${sfile} and converting '/' comments to '#'"
      VERBATIM
    )
    set_source_files_properties(${pp_output} PROPERTIES GENERATED TRUE)
    list(APPEND LIB_SOURCES ${pp_output})
  endforeach()
endif()


add_library(librthreads STATIC ${LIB_SOURCES})
# Ensure stdint is available for legacy headers (C only)
target_compile_options(librthreads PRIVATE $<$<COMPILE_LANGUAGE:C>:-include stdint.h>)
target_compile_options(librthreads PRIVATE $<$<COMPILE_LANGUAGE:ASM>:-m32 -march=i686>)

target_include_directories(librthreads BEFORE PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_BINARY_DIR}/generated/include
   ${CMAKE_SOURCE_DIR}/src/mach_services/include
   ${CMAKE_SOURCE_DIR}/src/mach_kernel
   ${CMAKE_SOURCE_DIR}/export/powermac/include
   ${CMAKE_SOURCE_DIR}/include
   ${CMAKE_SOURCE_DIR}/export/powermac/include
)

# Ensure generated include dir contains a wrapper stdio.h/stdlib.h che forwardano a sa_mach
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include")
file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdio.h" "/* Auto-generated wrapper: prefer Mach stdio */\n#include <sa_mach/stdio.h>\n")
file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdlib.h" "/* Auto-generated wrapper: prefer Mach stdlib */\n#include <sa_mach/stdlib.h>\n")

# Link against libmach for Mach syscall wrappers
target_link_libraries(librthreads libmach)

set_target_properties(librthreads PROPERTIES
  OUTPUT_NAME "rthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
