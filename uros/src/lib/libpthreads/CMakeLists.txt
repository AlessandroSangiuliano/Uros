cmake_minimum_required(VERSION 3.16)
project(libpthreads C ASM)

# Which machine's half of this library to build.
#
# ⚠️ Set HERE, above the first use.  It was being chosen further down, beside
# the source list, while the include path a hundred lines earlier already
# named a directory -- so the variable was empty where it was first read and
# CMake said nothing.
if(UROS_TARGET_ARCH STREQUAL "i386")
  set(_PTHREAD_MD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/AT386)
else()
  set(_PTHREAD_MD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/x86_64)
endif()

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
  ${_PTHREAD_MD_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
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

# Architecture-specific sources.
#
# ⚠️ Chosen by UROS_TARGET_ARCH and not by `if(EXISTS <dir>)', which is what
# this said before: the AT386 directory exists in every checkout, so the test
# was true on every target and an x86-64 build would have compiled i386's
# assembly.  A directory being present is not evidence that it is the right
# one.
if(UROS_TARGET_ARCH STREQUAL "i386")
  list(APPEND LIB_SOURCES
    AT386/i386_thread.c
    AT386/i386_lock.S
    AT386/i386_stack.S
  )
else()
  list(APPEND LIB_SOURCES
    x86_64/x86_64_thread.c
    x86_64/x86_64_lock.S
    x86_64/x86_64_stack.S
  )
endif()

add_library(libpthreads STATIC ${LIB_SOURCES})

add_dependencies(libpthreads mig_mach_public_headers pthread_impl_header)

# 🔴 NO -w, and its removal is the point (#425).
#
# It was here, and it threw away the one line that named the defect that hung
# this library on x86-64:
#
#   pthread_cond.c:52: warning: '__atomic_compare_exchange_8' writing 8 bytes
#   into a region of size 4 overflows the destination [-Wstringop-overflow=]
#
# An `int' expected-value against a `long' field: the compare failed for every
# thread, every thread took the losing branch of the lazy init, and every one
# spun for a magic word nobody was left to publish.  pthread_test stopped on
# arm 2 of 23 and no log could say why, because a thread parked in a spin
# prints nothing.  It cost a debugger session to find what gcc had written
# down and this flag had discarded.
#
# ⚠️ Removing it is free TODAY, and that is a fact with a date on it: with the
# two defects fixed, both targets compile this library with zero warnings, so
# nothing is being suppressed and nothing has to be triaged first.  Whoever
# adds the next warning here gets to decide what to do about it, which is the
# whole arrangement.
target_compile_options(libpthreads PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -include stdint.h -mstack-protector-guard=global>
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
  ${_PTHREAD_MD_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include/sa_mach
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
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

# 🔥 And every other consumer, not just libpthreads (#434).
#
# <pthread.h> includes <machine/pthread_impl.h>, and plenty of targets
# include the first while linking none of this library -- libvfs, the file
# systems, libgpu_console, half the servers.  They carry generated/include
# on their path and nothing more, so whether the header exists when they
# compile is decided by build order.  On a warm tree it always does; from
# an empty directory it is a coin toss, and the loser gets `fatal error:
# machine/pthread_impl.h'.
#
# mig_mach_public_headers is this tree's de-facto "the generated public
# headers are ready" gate -- sixty targets already depend on it -- so the
# ordering is stated once here rather than remembered at each consumer.
# The name says MIG and this header is not MIG's; what the target actually
# means is what is published into generated/include, and this is published
# there.
# ⚠️ The ordering is NOT hung on mig_mach_public_headers, though that is the
# tree's de-facto "generated headers are ready" gate and sixty targets
# already depend on it (#434).  It cannot be: mk_pthread_impl -- the program
# that WRITES this header -- depends on that gate itself, so hanging the gate
# on the header closes a cycle, and CMake says so:
#
#   mig_mach_public_headers -> pthread_impl_published
#   pthread_impl_published  -> mk_pthread_impl
#   mk_pthread_impl         -> mig_mach_public_headers
#
# So the dependency goes on the consumers instead.  Targets that LINK this
# library already have it -- linking orders them after libpthreads and
# everything it depends on.  What needed saying out loud is the other kind:
# a target that includes <pthread.h> and links something else entirely.
# Those name pthread_impl_published themselves; see libvfs, libgpu_console,
# libsa_fs and libposix-uros.

set_target_properties(libpthreads PROPERTIES
  OUTPUT_NAME "pthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
