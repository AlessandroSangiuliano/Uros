cmake_minimum_required(VERSION 3.16)
project(libpthreads C ASM)

# ===========================================================================
# The sizes in <pthread_impl.h>, generated instead of carried (#425)
# ===========================================================================
#
# AT386/pthread_impl.h says "*** CAUTION!! Do not edit!!" at the top, and it
# means it: mk_pthread_impl.c in this directory prints that file.  ⚠️ But no
# rule has ever run it -- the comment below used to exclude the generator as
# "a tool, not library code" -- so the header is a DERIVED file that nothing
# derives.  It has been edited by hand or inherited ever since, and nothing
# compares it with the structures it describes.
#
# What it holds is the size of every opaque POSIX type: the byte count of the
# buffer inside pthread_t, pthread_mutex_t, pthread_cond_t and the rest.  ⚠️
# Those buffers hold structures full of POINTERS -- struct _pthread alone has
# four -- so on a target where a pointer is eight bytes every number in it is
# wrong, and wrong in the worst way available: a size that is too small is not
# a compile error, it is a write past the end of the caller's object.
#
# So it is generated, for both targets.  ⚠️ The generator is compiled with the
# TARGET's flags and headers, because the sizes it prints must be the target's,
# and it RUNS on the host, which is what makes it a build tool rather than part
# of the library.  That is the whole trick and the only reason it needs its own
# target.
add_executable(mk_pthread_impl mk_pthread_impl.c)

target_compile_options(mk_pthread_impl PRIVATE
  ${UROS_ARCH_FLAGS} -std=gnu11 -w -include stdint.h)
target_link_options(mk_pthread_impl PRIVATE ${UROS_ARCH_FLAGS})

# ⚠️ __POSIX_LIB__, the same definition the library is built with, and without
# it this does not compile at all.  pthread.h guards the PUBLIC `struct
# sched_param' -- the opaque one, whose size is among the numbers being printed
# -- with #ifndef __POSIX_LIB__, so that library-internal code keeps the real
# one from posix_sched.h instead.  A generator that measured the public
# structure would be measuring its own output.
target_compile_definitions(mk_pthread_impl PRIVATE __POSIX_LIB__)

# ⚠️ The same include path the library gets, and in the same order.  A size
# computed against a different header than the one the library compiles with
# would be a number that is right about nothing.
target_include_directories(mk_pthread_impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/AT386
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/uapi)

add_dependencies(mk_pthread_impl mig_mach_public_headers)

set(_PTHREAD_IMPL ${CMAKE_BINARY_DIR}/generated/include/pthread_impl.h)
add_custom_command(
  OUTPUT ${_PTHREAD_IMPL}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated/include
  COMMAND mk_pthread_impl > ${_PTHREAD_IMPL}
  DEPENDS mk_pthread_impl
  COMMENT "Generating pthread_impl.h for ${UROS_TARGET_ARCH}"
  VERBATIM)
add_custom_target(pthread_impl_header DEPENDS ${_PTHREAD_IMPL})

# Library sources (mk_pthread_impl.c is the generator above, not library code)
set(LIB_SOURCES
  pthread.c
  pthread_mutex.c
  pthread_cond.c
  pthread_tsd.c
  pthread_rwlock.c
  pthread_barrier.c
  pthread_spin.c
  pthread_signal.c
  mig_support.c
  fprintf.c
  thread_switch.c
)

# AT386 (i386) architecture-specific sources
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AT386")
  list(APPEND LIB_SOURCES
    AT386/i386_thread.c
    AT386/i386_lock.S
    AT386/i386_stack.S
  )
endif()

add_library(libpthreads STATIC ${LIB_SOURCES})

add_dependencies(libpthreads mig_mach_public_headers pthread_impl_header)

target_compile_options(libpthreads PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -mstack-protector-guard=global>
  ${UROS_ARCH_FLAGS}
)

target_compile_definitions(libpthreads PRIVATE
  __POSIX_LIB__
  MACH_IPC_COMPAT=0
  __NO_UNDERSCORES__
  $<$<COMPILE_LANGUAGE:ASM>:ASSEMBLER>
)

target_include_directories(libpthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/AT386
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Install the public header that clients need to #include <pthread.h>.
# pthread.h includes <machine/pthread_impl.h> — copy into generated/include/machine/.
# ⚠️ pthread_impl.h is PUBLISHED FROM THE BUILD now, not copied from the
# source tree -- the checked-in AT386/pthread_impl.h is gone.  It was a
# generated file that nothing generated, and while it was on the include path
# ahead of generated/include it would have shadowed the real one: the same
# trap as the MIG mach.h, with a header whose numbers describe memory layout.
#
# ⚠️ And copied at BUILD time, not configure time.  file(COPY) runs while
# cmake is generating, when the header does not exist yet -- the old line
# could use it because a checked-in file is always there, and a generated one
# never is at that moment.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/include/machine/pthread_impl.h
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${CMAKE_BINARY_DIR}/generated/include/machine
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${_PTHREAD_IMPL}
          ${CMAKE_BINARY_DIR}/generated/include/machine/pthread_impl.h
  DEPENDS ${_PTHREAD_IMPL}
  COMMENT "Publish machine/pthread_impl.h"
  VERBATIM)
add_custom_target(pthread_impl_published
  DEPENDS ${CMAKE_BINARY_DIR}/generated/include/machine/pthread_impl.h)
add_dependencies(libpthreads pthread_impl_published)

set_target_properties(libpthreads PROPERTIES
  OUTPUT_NAME "pthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
