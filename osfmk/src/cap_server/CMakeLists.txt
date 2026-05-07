cmake_minimum_required(VERSION 3.16)
project(cap_server C)

# MIG-generated server-side stubs live in libcap's build tree (they come
# out of the same cap_server.defs invocation as the user stubs).  We
# reference them as generated sources so we don't re-run MIG.
set(CAP_SERVER_MIG_C "${CMAKE_BINARY_DIR}/generated/cap_server_server.c")
set(CAP_SERVER_MIG_H "${CMAKE_BINARY_DIR}/generated/cap_server_server.h")
set_source_files_properties(${CAP_SERVER_MIG_C} ${CAP_SERVER_MIG_H}
  PROPERTIES GENERATED TRUE)

# Issue #183: cap_server is the *caller* of cap_revoke_notify, so it
# pulls the MIG user stub directly (libcap doesn't ship it — see the
# note in libcap/CMakeLists.txt).
set(CAP_REVOKE_USER_MIG_C "${CMAKE_BINARY_DIR}/generated/cap_revoke_user.c")
set(CAP_REVOKE_USER_MIG_H "${CMAKE_BINARY_DIR}/generated/cap_revoke.h")
set_source_files_properties(${CAP_REVOKE_USER_MIG_C} ${CAP_REVOKE_USER_MIG_H}
  PROPERTIES GENERATED TRUE)

set(CAP_SERVER_SOURCES
  cap_server.c
  cap_table.c
  ${CAP_SERVER_MIG_C}
  ${CAP_REVOKE_USER_MIG_C}
)

add_executable(cap_server_bin ${CAP_SERVER_SOURCES})

# libcap owns the MIG generation target; depend on it so
# cap_server_server.c/.h exist before this target compiles.
add_dependencies(cap_server_bin mig_mach_public_headers libcap mig_cap_revoke)

target_compile_options(cap_server_bin PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(cap_server_bin PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(cap_server_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Mach services include FIRST so <mach.h> resolves to mach_services/include/mach.h
  # (which declares panic_init/mach_msg_server) and not to the MIG-generated
  # build/generated/mach.h that ships with the user stubs.
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # libcap headers (libcap.h, sha256.h, hmac_sha256.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libcap
  # libgpu_console headers (#209: async printf mirror to gpu_server)
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libgpu_console
  # pthread.h and machine/pthread_impl.h
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads/AT386
  # Pre-built export headers (mach/cap_types.h, mach/bootstrap.h, ...)
  ${CMAKE_SOURCE_DIR}/export/include
  # MIG-generated headers (cap_server_server.h, cap_server.h)
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link order mirrors block_device_server: the MIG stubs and server
# handlers are in the executable itself, libcap provides SHA-256/HMAC,
# libnetname for netname_check_in, libmach/libsa_mach for Mach + libc.
target_link_libraries(cap_server_bin PRIVATE
  -Wl,--start-group
  libcap
  libpthreads
  libmach
  libsa_mach
  libnetname
  libgpu_console
  -Wl,--end-group
)

target_link_options(cap_server_bin PRIVATE
  -m32 -pie -nostdlib
  -Wl,-e,__start_mach
  -Wl,--no-dynamic-linker
  -Wl,--export-dynamic
  -Wl,--hash-style=sysv
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(cap_server_bin PROPERTIES
  OUTPUT_NAME "cap_server"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
