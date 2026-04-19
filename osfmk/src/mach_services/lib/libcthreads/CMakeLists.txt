cmake_minimum_required(VERSION 3.16)
project(libcthreads C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")

# arch-specific dirs (i386, POWERMAC, HP700)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/i386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "i386/*.c" "i386/*.s" "i386/*.S")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/POWERMAC" AND CMAKE_SYSTEM_PROCESSOR MATCHES "ppc|powerpc|ppc64|ppc64le")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "POWERMAC/*.c" "POWERMAC/*.s" "POWERMAC/*.S")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
endif()

add_library(libcthreads STATIC ${LIB_SOURCES})

add_dependencies(libcthreads mig_mach_public_headers)

# Legacy K&R/gnu89 code: suppress warnings, allow implicit declarations
target_compile_options(libcthreads PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686 -msse -msse2>)

target_include_directories(libcthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Define ASSEMBLER when preprocessing assembler sources so macros like ALIGN are available.
# __NO_UNDERSCORES__: ELF does not prefix symbols with '_' (unlike Mach-O/a.out).
target_compile_definitions(libcthreads PRIVATE
  $<$<COMPILE_LANGUAGE:ASM>:ASSEMBLER __NO_UNDERSCORES__>
  $<$<COMPILE_LANGUAGE:ASM-ATT>:ASSEMBLER __NO_UNDERSCORES__>
)

# Expose build-mode to sources so they can use #ifdef OSFMK_BUILD_KERNEL
target_compile_definitions(libcthreads PRIVATE $<$<BOOL:${OSFMK_BUILD_KERNEL}>:OSFMK_BUILD_KERNEL>)

# For host builds only: build i386 objects in 32-bit mode to allow assembling legacy i386 asm
if(NOT OSFMK_BUILD_KERNEL)
  target_compile_options(libcthreads PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-m32>
    $<$<COMPILE_LANGUAGE:ASM-ATT>:-m32>
    $<$<COMPILE_LANGUAGE:C>:-m32>
  )
else()
  # When building the kernel for i386, we still need -m32 to assemble/compile
  # legacy i386 assembly and C code into 32-bit objects.
  if(OSFMK_TARGET_AT386)
    target_compile_options(libcthreads PRIVATE
      $<$<COMPILE_LANGUAGE:ASM>:-m32>
      $<$<COMPILE_LANGUAGE:ASM-ATT>:-m32>
      $<$<COMPILE_LANGUAGE:C>:-m32>
    )
  endif()
endif()

set_target_properties(libcthreads PROPERTIES
  OUTPUT_NAME "cthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
