cmake_minimum_required(VERSION 3.16)
project(default_pager_server C)

# ===========================================================================
# MIG: generate stubs using the project mig script (scripts/mig)
# ===========================================================================

set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(MIG_OUT "${CMAKE_BINARY_DIR}/generated/default_pager_mig")
file(MAKE_DIRECTORY "${MIG_OUT}")

# Helper function: invoke the mig script to generate .c/.h from a .defs file.
# Usage: dp_mig_defs(<path-to-defs> <stem> INCLUDES ... [DEFINES ...])
# All arguments after the stem are passed as-is to the mig script as cpp flags.
# Items that start with -D are passed literally; others get -I prepended.
function(dp_mig_defs DEFS_FILE STEM)
  set(_userC   "${MIG_OUT}/${STEM}User.c")
  set(_serverC "${MIG_OUT}/${STEM}Server.c")
  set(_userH   "${MIG_OUT}/${STEM}.h")
  set(_serverH "${MIG_OUT}/${STEM}Server.h")

  set(CPP_FLAGS)
  foreach(arg IN LISTS ARGN)
    if(arg MATCHES "^-")
      # Already a flag (-DSEQNOS, -DFOO, etc.) — pass as-is
      list(APPEND CPP_FLAGS ${arg})
    else()
      # Assume it's an include path
      list(APPEND CPP_FLAGS -I${arg})
    endif()
  endforeach()

  add_custom_command(
    OUTPUT ${_userC} ${_serverC} ${_userH} ${_serverH}
    COMMAND ${MIG_SCRIPT}
      -cpp ${CMAKE_C_COMPILER}
      -migcom $<TARGET_FILE:migcom>
      ${CPP_FLAGS}
      -header  ${_userH}
      -sheader ${_serverH}
      -user    ${_userC}
      -server  ${_serverC}
      ${DEFS_FILE}
    DEPENDS migcom ${DEFS_FILE}
    COMMENT "MIG: ${STEM}.defs"
    VERBATIM
  )

  # Export generated server source to parent scope
  set(${STEM}_SERVER_C ${_serverC} PARENT_SCOPE)
  set(${STEM}_USER_C   ${_userC}   PARENT_SCOPE)
endfunction()

# Common include paths and defines for MIG preprocessing.
# -DSEQNOS enables the seqnos_ prefix on memory_object server functions,
# which the default_pager source code expects.
set(MIG_INCLUDES
  -DSEQNOS
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_CURRENT_SOURCE_DIR}/mach
)

# 1) default_pager_object.defs — the pager's own MIG interface
dp_mig_defs(
  "${CMAKE_CURRENT_SOURCE_DIR}/mach/default_pager_object.defs"
  default_pager_object
  ${MIG_INCLUDES}
)

# 2) memory_object_default.defs — kernel→pager interface (data_request, data_return, etc.)
dp_mig_defs(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/memory_object_default.defs"
  memory_object_default
  ${MIG_INCLUDES}
)

# 3) memory_object.defs — pager→kernel interface (data_supply, lock_request, etc.)
dp_mig_defs(
  "${CMAKE_SOURCE_DIR}/export/powermac/include/mach/memory_object.defs"
  memory_object
  ${MIG_INCLUDES}
)

set(MIG_SERVER_SOURCES
  ${default_pager_object_SERVER_C}
  ${memory_object_default_SERVER_C}
  ${memory_object_SERVER_C}
)

# ===========================================================================
# Source files
# ===========================================================================

set(DEFAULT_PAGER_SOURCES
  default_pager.c
  dp_memory_object.c
  dp_backing_store.c
  kalloc.c
  wiring.c
  strfcns.c
  nostdlib_stubs.c
)

# ===========================================================================
# Build target: Mach standalone executable (same pattern as bootstrap)
# ===========================================================================

add_executable(default_pager_server
  ${DEFAULT_PAGER_SOURCES}
  ${MIG_SERVER_SOURCES}
)

target_compile_options(default_pager_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(default_pager_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(default_pager_server PRIVATE
  # Local sources first (default_pager_internal.h, externs.h, etc.)
  ${CMAKE_CURRENT_SOURCE_DIR}
  # MIG generated output (default_pager_objectServer.h, etc.)
  ${MIG_OUT}
  # Generated includes (mach/machine -> i386 symlink, device/device.h)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, cthreads.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Pre-built export headers (mach/mach_interface.h, device/device_reply.h, etc.)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link against Mach userspace libraries.
# Same circular dependency resolution as bootstrap: --start-group/--end-group.
# The original Makefile links: -lcthreads -lmach -lsa_mach -lmach
target_link_libraries(default_pager_server PRIVATE
  -Wl,--start-group
  libcthreads
  libmach
  libsa_mach
  -Wl,--end-group
)

# Linker flags: 32-bit static Mach binary, no host libc.
# --allow-multiple-definition: cthreads inline functions emitted in every .o
# that includes cthreads.h (C89 extern inline semantics).
target_link_options(default_pager_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  # Produce a classic 2-segment ELF layout (text+rodata / data+bss).
  # The OSFMK bootstrap ELF loader only understands R+X and R+W LOAD
  # segments; modern linkers create separate read-only LOAD segments
  # for .rodata, .eh_frame, etc. that the loader skips.
  -Wl,-z,noseparate-code
  -Wl,-z,norelro
  -Wl,--build-id=none
)

set_target_properties(default_pager_server PROPERTIES
  OUTPUT_NAME "default_pager"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
