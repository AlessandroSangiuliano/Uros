cmake_minimum_required(VERSION 3.16)
project(pthread_test C)

add_executable(pthread_test_server
  pthread_test.c
)

target_compile_options(pthread_test_server PRIVATE
  -std=gnu11
  -m32 -march=i686 -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -w
  -include stdint.h
)

target_compile_definitions(pthread_test_server PRIVATE
  MACH_IPC_COMPAT=0
)

target_include_directories(pthread_test_server PRIVATE
  # pthread.h lives in libpthreads source dir
  ${CMAKE_SOURCE_DIR}/src/mach_services/lib/libpthreads
  # Generated includes (mach/machine -> i386 symlink)
  ${CMAKE_BINARY_DIR}/generated/include
  # Mach services include (sa_mach.h, stdio.h, string.h)
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  # Pre-built export headers (mach/mach_interface.h, etc.)
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # Kernel headers (mach/, device/, i386/)
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  # Top-level include
  ${CMAKE_SOURCE_DIR}/include
)

# Link with libpthreads INSTEAD of libcthreads.
# Both define _thread_init_routine — linking both would cause conflicts.
target_link_libraries(pthread_test_server PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libsa_mach
  -Wl,--end-group
)

target_link_options(pthread_test_server PRIVATE
  -m32 -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--allow-multiple-definition
  -Wl,--build-id=none
)

set_target_properties(pthread_test_server PROPERTIES
  OUTPUT_NAME "pthread_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/sbin"
)
