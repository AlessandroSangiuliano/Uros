cmake_minimum_required(VERSION 3.16)
project(libmachid C)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c" "*.S" "*.s")
# Exclude vm tooling sources that depend on server-only headers when building for the host
if(NOT OSFMK_BUILD_KERNEL)
  list(REMOVE_ITEM LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/machid_vmstuff.c")
endif()
add_library(libmachid STATIC ${LIB_SOURCES})

add_dependencies(libmachid mig_mach_public_headers)

# This library never had the target's architecture flags, while every one of
# its siblings does -- so it was compiled at the host's width and archived
# into export/.../i386/user/lib as if it were i386 code, with <mach/machine/
# vm_types.h> handing it 32-bit vm_offset_t all the while.
#
# Nothing links it, which is why nothing ever said so.  It surfaced only when
# #453 made a mismatched <mach/i386/vm_types.h> an #error instead of a
# truncation, and it is the first thing that guard caught.
target_compile_options(libmachid PRIVATE ${UROS_ARCH_FLAGS})

target_include_directories(libmachid PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Generazione MIG per .defs locali (machid.defs, machid_debug.defs)
file(GLOB MIG_DEFS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.defs")
foreach(DEFS IN LISTS MIG_DEFS)
  add_mig_defs(${DEFS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/export/include)
  get_filename_component(DEFS_NAME ${DEFS} NAME_WE)
  list(APPEND LIB_SOURCES ${${DEFS_NAME}_MIG_GENERATED})
endforeach()

set_target_properties(libmachid PROPERTIES
  OUTPUT_NAME "machid"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
