cmake_minimum_required(VERSION 3.16)
project(virtual_terminal_server C)

# virtual_terminal_server — Uros virtual-terminal shell supervisor (#365).
# musl-linked binary that forks/execs a ush shell onto each virtual
# terminal and respawns it on exit.  Template mirrors ush (#275.5) but
# drives no tty of its own, so it needs neither char_server nor proc MIG
# stubs — just fork/execve/waitpid from libposix-uros.

if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(virtual_terminal_server_bin
  virtual_terminal_server.c
)

add_dependencies(virtual_terminal_server_bin musl_extproj libposix-uros)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
  OUTPUT_VARIABLE _GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE _LIBGCC_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_options(virtual_terminal_server_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -w
  -include stdint.h
  -nostdinc
)

target_include_directories(virtual_terminal_server_bin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/src/lib/libposix-uros
  ${MUSL_PREFIX}/include
  ${_GCC_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/src/servers/proc_server      # proc_types.h (proc_exit_msg_t)
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(virtual_terminal_server_bin PRIVATE
  -Wl,--start-group
  libcap
  libnetname
  libvfs
  libflipc2
  libmach_core
  libposix-uros
  ${MUSL_PREFIX}/lib/libc-musl.a
  ${_LIBGCC_A}
  -Wl,--end-group
)

target_link_options(virtual_terminal_server_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(virtual_terminal_server_bin PROPERTIES
  OUTPUT_NAME "virtual_terminal_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# ===========================================================================
# Embedded .uros_manifest section (#235).
# ===========================================================================

set(VTS_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/virtual_terminal_server.manifest)
set(VTS_CMF          ${CMAKE_CURRENT_BINARY_DIR}/virtual_terminal_server.cmf)

add_custom_command(TARGET virtual_terminal_server_bin POST_BUILD
  COMMAND $<TARGET_FILE:mkmanifest> -o ${VTS_CMF} ${VTS_MANIFEST_SRC}
  COMMAND ${CMAKE_OBJCOPY} --add-section .uros_manifest=${VTS_CMF}
            --set-section-flags .uros_manifest=noload,readonly,contents
            $<TARGET_FILE:virtual_terminal_server_bin>
  DEPENDS mkmanifest ${VTS_MANIFEST_SRC}
  COMMENT "embed .uros_manifest into virtual_terminal_server"
  VERBATIM
)
