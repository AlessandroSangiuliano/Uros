cmake_minimum_required(VERSION 3.16)
project(char_test C)

# ===========================================================================
# A client for char_server, which is what #497's second clause asks for
# ===========================================================================
#
# x86-64 only, for netname_test's reason: on i386 char_server has had a client
# since #275.5 -- ush binds the tty as its controlling terminal and every
# keystroke of the acceptance smoke goes through it -- so a copy there would
# test the 1990 userland rather than the port.
#
# ⚠️ It is also the target where the two halves differ.  i386's ush reaches
# the tty through the unbound-ctty exception, which asks proc_server who the
# session leader is; proc_server has not crossed, so on this target the
# cap-checked road is the only one and this test takes it.
if(UROS_TARGET_ARCH STREQUAL "i386")
  return()
endif()

# char_server.defs, user half only.
#
# ⚠️ add_mig_userland_client and not add_mig_userland: the server half of this
# subsystem belongs to char_server and exists there already.  Generating a
# second copy to throw away is how a tree ends up with two demuxes from one
# .defs and nothing comparing them (#471, #481).
#
# ⚠️ And the include list has to carry char_server's own directory, because
# char_server.defs imports <mach/char_server_types.h> -- which lives under
# src/servers/char_server/mach/ and nowhere else.
set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -I${CMAKE_SOURCE_DIR}/src/servers/char_server
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

set(_CHT_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(CHT_MIG_GENERATED)
add_mig_userland_client(
  ${CMAKE_SOURCE_DIR}/src/servers/char_server/char_server.defs
  ${_CHT_OUT} CHT_MIG)

# device_master.defs, user half: arm [3] asks the kernel to write a port
# char_server owns, and must be told no.  Proving a refusal needs a task that
# is not the driver, which is cap_test's reason for generating these too.
add_mig_userland_client(${UROS_UAPI_DIR}/device/device_master.defs
                        ${_CHT_OUT} CHT_DM)
list(APPEND CHT_MIG_GENERATED ${CHT_DM_GENERATED})

add_executable(char_test_bin char_test.c ${CHT_MIG_GENERATED})

add_dependencies(char_test_bin libmach libpthreads libnetname libcap
                 mig_mach_public_headers)

target_compile_options(char_test_bin PRIVATE
  -std=gnu11
  ${UROS_ARCH_FLAGS} -msse -msse2
  -fno-builtin
  -mstack-protector-guard=global
  -include stdint.h)

target_compile_definitions(char_test_bin PRIVATE MACH_IPC_COMPAT=0)

target_include_directories(char_test_bin PRIVATE
  ${_CHT_OUT}
  ${CMAKE_SOURCE_DIR}/src/servers/char_server
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/uapi
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include)

# --start-group for act_test's reason: libpthreads and libmach reach into each
# other, and libnetname's stubs call libmach's mach_msg while libmach knows
# nothing of libnetname.  libcap joins for the same reason.
target_link_libraries(char_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libnetname
  libcap
  libmach
  -Wl,--end-group)

target_link_options(char_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(char_test_bin PROPERTIES
  OUTPUT_NAME "char_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")

# ===========================================================================
# The manifest — without it this task cannot speak to the serial port
# ===========================================================================
#
# Written to the export tree, for the reason cap_test's copy of this rule
# records: x86-64's bundle names the .cmf from servers/bootstrap and i386's is
# assembled by scripts/make-bundle.sh out of the export tree, so producing the
# file once and letting both read it is what keeps the two bundles shipping
# the same bytes.
set(CHARTEST_MANIFEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/char_test.manifest)
set(CHARTEST_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/char_test.cmf")

add_custom_command(
  OUTPUT ${CHARTEST_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${CHARTEST_MANIFEST_OUT}
          ${CHARTEST_MANIFEST_SRC}
  DEPENDS mkmanifest ${CHARTEST_MANIFEST_SRC}
  COMMENT "mkmanifest: char_test.manifest -> char_test.cmf"
  VERBATIM
)

add_custom_target(char_test_manifest ALL DEPENDS ${CHARTEST_MANIFEST_OUT})
add_dependencies(char_test_bin char_test_manifest)
