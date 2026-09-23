cmake_minimum_required(VERSION 3.16)
project(xfile_test C)

# A file written by one target and read by the other (#498).
#
# One program built on both targets, which is the point: the file it writes
# on one is checked by the SAME source compiled for the other, so a width that
# differs between the two builds of ext_server shows up as a byte at a named
# offset.
#
# It is a client of libvfs and nothing else -- the path a real program takes
# to a file, FLIPC write-behind included -- and it answers no RPC.

add_executable(xfile_test_bin xfile_test.c)

add_dependencies(xfile_test_bin mig_mach_public_headers)

target_compile_options(xfile_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h
)

target_compile_definitions(xfile_test_bin PRIVATE
  MACH_IPC_COMPAT=0
)

# libvfs exports its include directories, which are this program's too.
target_link_libraries(xfile_test_bin PRIVATE
  -Wl,--start-group
  libvfs
  libflipc2
  libnetname
  libpthreads
  libmach
  -Wl,--end-group
)

target_link_options(xfile_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,--build-id=none
)

set_target_properties(xfile_test_bin PROPERTIES
  OUTPUT_NAME "xfile_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)
