cmake_minimum_required(VERSION 3.16)
project(librthreads C ASM)

file(GLOB LIB_SOURCES CONFIGURE_DEPENDS "*.c")

# AT386 (i386) architecture-specific sources
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AT386")
  file(GLOB ARCH_C_SOURCES CONFIGURE_DEPENDS "AT386/*.c")
  file(GLOB ARCH_ASM_SOURCES CONFIGURE_DEPENDS "AT386/*.S")
  list(APPEND LIB_SOURCES ${ARCH_C_SOURCES} ${ARCH_ASM_SOURCES})
endif()

add_library(librthreads STATIC ${LIB_SOURCES})

add_dependencies(librthreads mig_mach_public_headers)

target_compile_options(librthreads PRIVATE
  ${UROS_ARCH_FLAGS} $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -include stdint.h>
  ${UROS_ARCH_FLAGS}
)

target_compile_definitions(librthreads PRIVATE
  __NO_UNDERSCORES__
  $<$<COMPILE_LANGUAGE:ASM>:ASSEMBLER>
)

target_include_directories(librthreads PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${UROS_ARCH_INCLUDE}
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

# Wrapper stdio.h/stdlib.h forwarding to sa_mach -- only where those names
# are still ours to define (#434).
#
# 🔥 These two files were written unconditionally, at configure time, into
# a `generated/include' that forty-eight targets carry on their include
# path -- including every one that compiles against musl.  With
# UROS_BUILD_MUSL=ON the names `stdio.h' and `stdlib.h' belong to musl, and
# this wrapper pulls <sa_mach/types.h> instead, whose time_t is an `int'
# against musl's `long'.  That is the conflicting-types error a clean build
# stops on.
#
# It only bites while musl-install/include is still empty, which is why it
# looked like a race -- but the race merely decides whether you fall in.
# The file being there at all is what makes falling in possible, so it is
# the file that goes.
#
# ⚠️ And they are REMOVED rather than just not written, because a build
# directory configured before this change still has them, and a stale
# header that no rule produces is exactly the kind of leftover that makes
# an incremental tree disagree with a fresh one.
#
# ⚠️ librthreads is deprecated and nothing links it; when it goes (#425),
# these wrappers go with it and the musl-off branch below can go too.
if(UROS_BUILD_MUSL)
  file(REMOVE "${CMAKE_BINARY_DIR}/generated/include/stdio.h"
              "${CMAKE_BINARY_DIR}/generated/include/stdlib.h")
else()
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/include")
  file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdio.h" "/* Auto-generated wrapper: prefer Mach stdio */\n#include <sa_mach/stdio.h>\n")
  file(WRITE "${CMAKE_BINARY_DIR}/generated/include/stdlib.h" "/* Auto-generated wrapper: prefer Mach stdlib */\n#include <sa_mach/stdlib.h>\n")
endif()

set_target_properties(librthreads PROPERTIES
  OUTPUT_NAME "rthreads"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${UROS_TARGET_ARCH}/user/lib"
)
