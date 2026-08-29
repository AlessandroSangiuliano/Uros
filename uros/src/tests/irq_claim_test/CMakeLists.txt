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

add_executable(irq_claim_test_bin
  irq_claim_test.c ${IQ_MIG_GENERATED})

target_compile_options(irq_claim_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -fno-builtin
  -mstack-protector-guard=global -include stdint.h)

target_compile_definitions(irq_claim_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ ${_IQ_OUT} first: the generated stubs include each other by quoted name.
target_include_directories(irq_claim_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${_IQ_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(irq_claim_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
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
