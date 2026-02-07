cmake_minimum_required(VERSION 3.16)
project(librthreads C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AT386")
  file(GLOB ARCH_SOURCES CONFIGURE_DEPENDS "AT386/*.c" "AT386/*.s" "AT386/*.S")
  list(APPEND LIB_SOURCES ${ARCH_SOURCES})
endif()

add_library(librthreads STATIC ${LIB_SOURCES})
# Ensure stdint is available for legacy headers
target_compile_options(librthreads PRIVATE -include stdint.h)

target_include_directories(librthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(librthreads PROPERTIES
  OUTPUT_NAME "rthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
