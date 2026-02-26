cmake_minimum_required(VERSION 3.16)
project(librthreads C ASM)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c")

# AT386 (i386) architecture-specific sources
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AT386")
  file(GLOB ARCH_C_SOURCES CONFIGURE_DEPENDS "AT386/*.c")
  file(GLOB ARCH_ASM_SOURCES CONFIGURE_DEPENDS "AT386/*.S")
  list(APPEND LIB_SOURCES ${ARCH_C_SOURCES} ${ARCH_ASM_SOURCES})
endif()

add_library(librthreads STATIC ${LIB_SOURCES})

target_compile_options(librthreads PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686>
  $<$<COMPILE_LANGUAGE:ASM>:-m32 -march=i686>
)

target_compile_definitions(librthreads PRIVATE
  __NO_UNDERSCORES__
  $<$<COMPILE_LANGUAGE:ASM>:ASSEMBLER>
)

target_include_directories(librthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

# Ensure generated include dir contains wrapper stdio.h/stdlib.h forwarding to sa_mach
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include")
file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdio.h" "/* Auto-generated wrapper: prefer Mach stdio */\n#include <sa_mach/stdio.h>\n")
file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdlib.h" "/* Auto-generated wrapper: prefer Mach stdlib */\n#include <sa_mach/stdlib.h>\n")

set_target_properties(librthreads PROPERTIES
  OUTPUT_NAME "rthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
