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
# Ensure stdint is available for legacy headers
target_compile_options(libcthreads PRIVATE -include stdint.h)

target_include_directories(libcthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libcthreads PROPERTIES
  OUTPUT_NAME "cthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${CMAKE_SYSTEM_NAME}/lib"
)
