cmake_minimum_required(VERSION 3.16)
project(irq_claim_test C)

# ===========================================================================
# Claiming an interrupt line and giving it back, from where a driver stands
# (#457)
# ===========================================================================
#
# 🔑 BOTH TARGETS, and that is the point rather than convenience.  The two
# machines answer <device/device_machdep.h> with completely different
# machinery -- i386 writes a handler, a unit and a priority into three
# parallel arrays its dispatch reads; x86-64 picks a vector, claims it in the
# IDT dispatch table and points an I/O APIC pin at it -- and the whole reason
# for naming the operation instead of the mechanism is that one program above
# them cannot tell.  A test that ran on one target would be measuring an
# implementation; running on both is what measures the contract.
#
# ⚠️ And the defect it was written for is i386's, while the new code is
# x86-64's.  Neither target alone would have covered the change.
#
# ⚠️ add_mig_userland and NOT scripts/mig.  That wrapper routes any flag it
# does not recognise to the C preprocessor, so `-target x86_64' would arrive
# at cc and never reach migcom -- which then defaults to i386 and emits
# stubs with the wrong message layout, silently.  #427 paid for that once.

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

# device_master.defs, for the user half: this program is a client of the
# fourteen RPCs, not a server of them.
set(_IQ_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(IQ_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/device/device_master.defs ${_IQ_OUT} IQ_MIG)
list(REMOVE_ITEM IQ_MIG_GENERATED ${_IQ_OUT}/device_master_server.c)

# hal.defs, because a program cannot find its own device any more (#511).
#
# 🔑 A message-signalled interrupt is programmed into the DEVICE's own table,
# so asking for one is now a write to whoever owns that device -- which means
# this test has to own one.  And it cannot discover which device to own by
# reading configuration space, because reading configuration space is itself
# something only an owner or the bus scanner may do.  So it asks the HAL, the
# same way every driver in the tree does.
set(IQ_HAL_GENERATED)
add_mig_userland(${CMAKE_SOURCE_DIR}/src/servers/hal_server/hal.defs
                 ${_IQ_OUT} IQ_HAL)
list(REMOVE_ITEM IQ_HAL_GENERATED ${_IQ_OUT}/hal_server.c)
list(APPEND IQ_MIG_GENERATED ${IQ_HAL_GENERATED})

add_executable(irq_claim_test_bin
  irq_claim_test.c ${IQ_MIG_GENERATED})

target_compile_options(irq_claim_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(irq_claim_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ ${_IQ_OUT} first: the generated stubs include each other by quoted name.
target_include_directories(irq_claim_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # 🔴 THE HAL'S OWN DIRECTORY BEFORE THE GENERATED ONE, AND THE ORDER IS
  # LOAD-BEARING -- hal_bar_test's file says the same thing and paid for it
  # first.  migcom writes a `hal_server.h' of its own, the server half of
  # hal.defs, and that is also the name of the header defining struct
  # hal_device_info.  Generated directory first and this program compiles
  # against a header full of MIG declarations with the record it is written
  # about left an incomplete type.
  ${CMAKE_SOURCE_DIR}/src/servers/hal_server
  ${CMAKE_SOURCE_DIR}/src/lib/libpci
  ${_IQ_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/src/lib/libcap
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(irq_claim_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  libcap
  libnetname
  -Wl,--end-group)
add_dependencies(irq_claim_test_bin libmach libpthreads mig_mach_public_headers)

target_link_options(irq_claim_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(irq_claim_test_bin PROPERTIES
  OUTPUT_NAME "irq_claim_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/uros/${UROS_TARGET_ARCH}/user/sbin")

# ===========================================================================
# The manifest (#511)
# ===========================================================================
#
# Without it arm [7] does not fail, it DEGRADES: every device refuses, the
# count reaches zero, and zero has a passing verdict of its own.
set(IQ_MANIFEST_OUT
  "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests/irq_claim_test.cmf")

add_custom_command(
  OUTPUT ${IQ_MANIFEST_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/manifests"
  COMMAND $<TARGET_FILE:mkmanifest> -o ${IQ_MANIFEST_OUT}
          ${CMAKE_CURRENT_SOURCE_DIR}/irq_claim_test.manifest
  DEPENDS mkmanifest ${CMAKE_CURRENT_SOURCE_DIR}/irq_claim_test.manifest
  COMMENT "mkmanifest: irq_claim_test.manifest -> irq_claim_test.cmf"
  VERBATIM
)

add_custom_target(irq_claim_test_manifest ALL DEPENDS ${IQ_MANIFEST_OUT})
add_dependencies(irq_claim_test_bin irq_claim_test_manifest)
