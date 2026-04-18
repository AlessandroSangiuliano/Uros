cmake_minimum_required(VERSION 3.16)
project(libmachid C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")
# Exclude vm tooling sources that depend on server-only headers when building for the host
if(NOT OSFMK_BUILD_KERNEL)
  list(REMOVE_ITEM LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/machid_vmstuff.c")
endif()
add_library(libmachid STATIC ${LIB_SOURCES})

add_dependencies(libmachid mig_mach_public_headers)

target_include_directories(libmachid PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Generazione MIG per .defs locali (machid.defs, machid_debug.defs)
file(GLOB MIG_DEFS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.defs")
foreach(DEFS IN LISTS MIG_DEFS)
  add_mig_defs(${DEFS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/export/powermac/include)
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

set_target_properties(libmachid PROPERTIES
  OUTPUT_NAME "machid"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
