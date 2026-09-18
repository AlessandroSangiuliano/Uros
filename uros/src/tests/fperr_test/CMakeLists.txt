cmake_minimum_required(VERSION 3.16)
project(fperr_test C ASM)

# ===========================================================================
# Does an arithmetic fault reach the thread that caused it? (#515)
# ===========================================================================
#
# 🔴 BOTH TARGETS, and that is the point of the program.  #515 is the issue
# where each target turned out to be losing the fault the other delivered --
# i386 panics on #XF and x86-64 cannot raise #MF -- so a test built for one of
# them would have found half of it and called the tree healthy.  The C is the
# same on both; only the provocation forks, because the two calling conventions
# disagree in every line that touches the stack.
#
# ⚠️ add_mig_userland and NOT scripts/mig: that wrapper routes a flag it does
# not know to the C preprocessor, so `-target x86_64' would reach cc instead of
# migcom, which then defaults to i386 and emits stubs with the wrong message
# layout, silently (#427).

if(UROS_TARGET_ARCH STREQUAL "i386")
  set(_FE_ASM  fperr_provoke_i386.S)
  set(_FE_PTHR AT386)
else()
  set(_FE_ASM  fperr_provoke_x86_64.S)
  set(_FE_PTHR x86_64)
endif()

set(_K ${CMAKE_SOURCE_DIR}/src/mach_kernel)

set(UROS_MIG_USERLAND_INCLUDES
    -I${UROS_ARCH_INCLUDE}
    -I${_K}
    -I${CMAKE_SOURCE_DIR}/src/mach_services/include/servers)

# exc.defs, for the server half only.  The task is the one being TOLD about a
# fault, so what it needs is the demultiplexer and the three catch_ routines it
# calls; the user half would be the kernel's side of the same protocol.
set(_FE_OUT ${CMAKE_CURRENT_BINARY_DIR}/mig)
set(FE_MIG_GENERATED)
add_mig_userland(${UROS_UAPI_DIR}/mach/exc.defs ${_FE_OUT} FE_MIG)
list(REMOVE_ITEM FE_MIG_GENERATED ${_FE_OUT}/exc_user.c)

add_executable(fperr_test_bin
  fperr_test.c ${_FE_ASM} ${FE_MIG_GENERATED})

# ⚠️ The C-only flags are guarded by language, and that is not tidiness.  The
# neighbouring tests put `-include stdint.h' in the target's flags because they
# have no assembly; here it reaches the .S too, and the assembler answers with
# thirty "no such instruction: typedef signed char int8_t".  ${UROS_ARCH_FLAGS}
# stays unguarded on purpose -- -m32 has to reach both halves or the object
# files disagree about the machine.
target_compile_options(fperr_test_bin PRIVATE
  ${UROS_ARCH_FLAGS}
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
  $<$<COMPILE_LANGUAGE:C>:-fno-builtin>
  $<$<COMPILE_LANGUAGE:C>:-mstack-protector-guard=global>
  $<$<COMPILE_LANGUAGE:C>:-include>
  $<$<COMPILE_LANGUAGE:C>:stdint.h>)

target_compile_definitions(fperr_test_bin PRIVATE MACH_IPC_COMPAT=0)

# ⚠️ ${_FE_OUT} first: the stubs include each other by quoted name.
target_include_directories(fperr_test_bin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${_FE_OUT}
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/lib/libmach
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads
  ${CMAKE_SOURCE_DIR}/src/lib/libpthreads/${_FE_PTHR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/uapi
  ${UROS_ARCH_INCLUDE}
  ${_K}
  ${CMAKE_SOURCE_DIR}/include)

# --start-group because libpthreads and libmach reach into each other:
# libpthreads wants libmach's MIG stubs, libmach wants libpthreads' mig_support.
target_link_libraries(fperr_test_bin PRIVATE
  -Wl,--start-group
  libpthreads
  libmach
  -Wl,--end-group)
add_dependencies(fperr_test_bin libmach libpthreads mig_mach_public_headers)

target_link_options(fperr_test_bin PRIVATE
  ${UROS_ARCH_FLAGS} -static -nostdlib -nostartfiles
  -Wl,-e,__start_mach
  -Wl,-u,__start_mach
  -Wl,--build-id=none)

set_target_properties(fperr_test_bin PROPERTIES
  OUTPUT_NAME "fperr_test"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/sbin")
