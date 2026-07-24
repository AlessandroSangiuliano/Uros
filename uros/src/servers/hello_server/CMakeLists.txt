cmake_minimum_required(VERSION 3.16)
project(hello_server C)

# ===========================================================================
# Phase 3 (#251) — hello_server is the pilot for the musl libc path.
# Links musl + libposix-uros + libmach_core instead of full libmach.
# Until the migration is finished (Phase 8) this server only builds with
# -DUROS_BUILD_MUSL=ON; the legacy libmach build path is gone.
# ===========================================================================
if(NOT UROS_BUILD_MUSL)
  return()
endif()

set(HELLO_SERVER_SOURCES
  hello_server.c
)

add_executable(hello_server_server
  ${HELLO_SERVER_SOURCES}
)

add_dependencies(hello_server_server mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

# gcc's internal include directory — needed alongside -nostdinc so the
# compiler can still find stddef.h, stdarg.h, intrinsics, etc.
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
  OUTPUT_VARIABLE _GCC_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# libgcc.a for compiler builtins (__udivdi3 etc.) once we drop gcc.s
# from the libmach path.
execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-libgcc-file-name
  OUTPUT_VARIABLE _LIBGCC_A
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

target_compile_options(hello_server_server PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  # SSP re-enabled (#259): __uros_libc_init() now drives musl's real
  # __init_tls + __init_ssp, so __stack_chk_guard and the per-thread
  # canary are seeded before any protected function runs.
  -fstack-protector-strong
  -fno-builtin
  -w
  -include stdint.h
  # nostdinc: drop /usr/include entirely so <stdio.h> resolves to musl's
  # copy, not gcc's host glibc wrapper.
  -nostdinc
)

target_compile_definitions(hello_server_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(hello_server_server PRIVATE
  # ORDER MATTERS — musl headers first.
  ${MUSL_PREFIX}/include
  # gcc internal headers (stddef.h, stdarg.h) — required because of -nostdinc
  ${_GCC_INCLUDE}
  # libposix-uros public ABI (__uros_syscall*, version macros)
  ${CMAKE_SOURCE_DIR}/src/lib/libposix-uros/include
  # In-tree Mach userspace headers (mach_init.h with mach_cap_port,
  # libcap.h ...) — placed before export/include so the developer copy
  # wins over the public-export stub for symbols not yet promoted.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Generated MIG headers (mach/bootstrap.h, mach/mach_port.h, ...)
  ${CMAKE_BINARY_DIR}/generated/include
  # Pre-built export headers (mach.h, libcap.h, device/, ...)
  ${CMAKE_SOURCE_DIR}/export/include
  # Kernel headers needed by some MIG-generated stubs
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hello_server_server PRIVATE
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

target_link_options(hello_server_server PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  # Force-link musl's __uros_libc_init (#259): crt0 references it only
  # weakly (so legacy libmach servers link without it), and a weak ref
  # does not pull an object out of a static archive.  -u makes the
  # linker treat it as an undefined-strong symbol, pulling
  # uros_main_thread.o from libc-musl.a so crt0's call resolves.
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(hello_server_server PROPERTIES
  OUTPUT_NAME "hello_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# ===========================================================================
# #216 v2.1 manifest — unchanged from pre-musl build.
# ===========================================================================

set(HELLO_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/hello_server.manifest)
set(HELLO_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/hello_server.cmf")

add_custom_command(
  OUTPUT ${HELLO_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${HELLO_MANIFEST_OUT} ${HELLO_MANIFEST_SRC}
  DEPENDS mkmanifest ${HELLO_MANIFEST_SRC}
  COMMENT "mkmanifest: hello_server.manifest -> hello_server.cmf"
  VERBATIM
)

add_custom_target(hello_server_manifest ALL DEPENDS ${HELLO_MANIFEST_OUT})
add_dependencies(hello_server_server hello_server_manifest)
