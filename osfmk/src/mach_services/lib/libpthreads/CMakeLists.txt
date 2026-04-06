cmake_minimum_required(VERSION 3.16)
project(libpthreads C ASM)

# Library sources (mk_pthread_impl.c is excluded: it's a code generator tool, not library code)
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

target_compile_options(libpthreads PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h -m32 -march=i686 -mstack-protector-guard=global>
  $<$<COMPILE_LANGUAGE:ASM>:-m32>
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
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Install the public header that clients need to #include <pthread.h>.
# pthread.h includes <machine/pthread_impl.h> — copy into generated/include/machine/.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/AT386/pthread_impl.h"
     DESTINATION "${CMAKE_BINARY_DIR}/generated/include/machine")

set_target_properties(libpthreads PROPERTIES
  OUTPUT_NAME "pthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
