cmake_minimum_required(VERSION 3.16)
project(fork_cap_test C)

# fork_cap_test (#945): what a child made by fork() holds of its parent's
# capabilities.  musl_test's recipe, because fork() is libposix-uros's, and a
# manifest of its own, as cap_test has, so that there is a per-task port to
# fork away from.
if(NOT UROS_BUILD_MUSL)
  return()
endif()

add_executable(fork_cap_test_bin
  fork_cap_test.c
)

add_dependencies(fork_cap_test_bin mig_mach_public_headers musl_extproj)

set(MUSL_PREFIX ${CMAKE_BINARY_DIR}/src/contrib/musl-install)

target_compile_options(fork_cap_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fstack-protector-strong
  -fno-builtin
  -Wall -Wextra
  -nostdinc
)

# musl's headers first; libcap's and libmach's come with uros_libc after them.
target_include_directories(fork_cap_test_bin BEFORE PRIVATE
  ${MUSL_PREFIX}/include
)

target_link_libraries(fork_cap_test_bin PRIVATE
  uros_libc
)

target_link_options(fork_cap_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib
  -Wl,-e,__start_mach
  -Wl,-u,__uros_libc_init
  -Wl,--build-id=none
)

set_target_properties(fork_cap_test_bin PROPERTIES
  OUTPUT_NAME "fork_cap_test"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin"
)

# The manifest, compiled into the export tree, where servers/bootstrap's bundle
# rule reads it by the file, as it reads cap_test's (#432).
set(FORKCAP_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fork_cap_test.manifest)
set(FORKCAP_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/fork_cap_test.cmf")

add_custom_command(
  OUTPUT ${FORKCAP_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${FORKCAP_MANIFEST_OUT}
          ${FORKCAP_MANIFEST_SRC}
  DEPENDS mkmanifest ${FORKCAP_MANIFEST_SRC}
  COMMENT "mkmanifest: fork_cap_test.manifest -> fork_cap_test.cmf"
  VERBATIM
)

add_custom_target(fork_cap_test_manifest ALL DEPENDS ${FORKCAP_MANIFEST_OUT})
add_dependencies(fork_cap_test_bin fork_cap_test_manifest)
