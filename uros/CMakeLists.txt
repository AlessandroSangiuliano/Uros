cmake_minimum_required(VERSION 3.21)

project(uros
  VERSION 0.1
  LANGUAGES C ASM
)

# Opzioni principali di configurazione
option(OSFMK_TARGET_AT386 "Abilita configurazione Mach per AT386 (x86)" ON)
option(OSFMK_BUILD_TOOLS "Costruisci i tool host (mig, migcom, ecc.)" ON)
option(OSFMK_BUILD_KERNEL "Costruisci il kernel Mach" ON)
option(OSFMK_BUILD_BOOTSTRAP "Costruisci il bootstrap task" ON)
option(OSFMK_BUILD_DEFAULT_PAGER "Costruisci il default pager" OFF)
option(OSFMK_BUILD_HELLO_SERVER "Costruisci l'hello server (test server)" OFF)
option(OSFMK_BUILD_IPC_BENCH "Costruisci il benchmark IPC (ipc_bench)" OFF)
option(OSFMK_BUILD_BLOCK_SERVER "Costruisci il block device server modulare" OFF)
option(OSFMK_BUILD_HAL_SERVER "Costruisci l'HAL server (discovery + device registry)" OFF)
option(OSFMK_BUILD_NAME_SERVER "Costruisci il name server (netname)" OFF)
option(OSFMK_BUILD_EXT2_SERVER "Costruisci il server ext2 filesystem userspace" OFF)
option(OSFMK_BUILD_PTHREAD_TEST "Build pthread test server" OFF)
option(OSFMK_BUILD_CAP_SERVER "Build capability server (Uros cap_server + libcap)" OFF)
option(OSFMK_BUILD_CAP_TEST "Build capability negative-test binary" OFF)
option(OSFMK_BUILD_GPUSTAT "Build gpustat userspace probe for gpu_query_stats (#203)" OFF)
option(OSFMK_BUILD_GPU_SERVER "Build gpu_server (userspace GPU/display server, #194)" OFF)
option(OSFMK_BUILD_CHAR_SERVER "Build char_server (userspace character device server, #205)" OFF)
option(OSFMK_BUILD_EXEC_SERVER "Build exec_server (userspace ELF loader / process spawner, #228)" OFF)
option(OSFMK_BUILD_HELLO_EXEC "Build hello_exec test binary (#228 v0.1.0)" OFF)
option(OSFMK_BUILD_PROC_SERVER "Build proc_server (POSIX process model owner, #237)" OFF)

# #249 — libc.so consolidation roadmap, Phase 1: vendor musl 1.2.5 and
# build it standalone as libc-musl.a.  No Uros integration yet.
option(UROS_BUILD_MUSL "Build vendored musl 1.2.5 as standalone libc-musl.a (#249)" OFF)

# Issue #211: kernel symbol table for DDB.  Default ON for Debug / unset
# build types (developer build), OFF for Release where DDB lookup-by-name
# is not needed and the ~88 KB blob would just be dead weight.
if(NOT DEFINED OSFMK_GEN_KSYMS)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
     CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(OSFMK_GEN_KSYMS_DEFAULT OFF)
  else()
    set(OSFMK_GEN_KSYMS_DEFAULT ON)
  endif()
  option(OSFMK_GEN_KSYMS
    "Generate ksyms.bin (DDB kernel symbol table) — debug builds only"
    ${OSFMK_GEN_KSYMS_DEFAULT})
endif()

# ------------------------------------------------------------------ #404
# Target architecture.  One cache variable drives the arch flags, the MD
# source selection and the arch component of every export/ path — never
# uname -m / CMAKE_SYSTEM_PROCESSOR, which name the HOST: this tree spent
# v0.1-v0.2 exporting an i386 build into an export/uros/x86_64/ directory
# because the build machine happened to be 64-bit.
#
# Two build directories against one unmodified source tree is the
# contract: configure one with -DUROS_TARGET_ARCH=i386 (default) and a
# sibling with -DUROS_TARGET_ARCH=x86_64, and nothing in src/ changes.
set(UROS_TARGET_ARCH "i386" CACHE STRING "Target architecture (i386 | x86_64)")
set_property(CACHE UROS_TARGET_ARCH PROPERTY STRINGS i386 x86_64)

if(UROS_TARGET_ARCH STREQUAL "i386")
  set(UROS_ARCH_FLAGS -m32 -march=i686)
elseif(UROS_TARGET_ARCH STREQUAL "x86_64")
  set(UROS_ARCH_FLAGS -m64)
else()
  message(FATAL_ERROR "UROS_TARGET_ARCH must be i386 or x86_64, got '${UROS_TARGET_ARCH}'")
endif()
# String form for raw COMMAND / shell contexts (list form feeds
# target_compile_options and friends).
string(JOIN " " UROS_ARCH_FLAGS_STR ${UROS_ARCH_FLAGS})
message(STATUS "UROS_TARGET_ARCH = ${UROS_TARGET_ARCH} (${UROS_ARCH_FLAGS_STR})")

# The machine-dependent kernel layer lives in src/mach_kernel/<arch>/.
# For a target whose MD layer has not landed yet (x86_64 until the #406-
# #411 contracts), the build system, host tools and arch-neutral targets
# are all in place — but the kernel cannot link, and the failure should
# say so here rather than as a dangling-symlink error deep in a compile.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/mach_kernel/${UROS_TARGET_ARCH}")
  message(WARNING
    "No machine-dependent kernel layer at src/mach_kernel/${UROS_TARGET_ARCH}/ "
    "— the ${UROS_TARGET_ARCH} port's MD contracts (#406-#411) are not in yet. "
    "Host tools and architecture-neutral targets build; the kernel does not. "
    "This is the expected state of a fresh port target, not a misconfiguration.")
endif()

# Stamp the target into the build dir so the scripts (make-bundle,
# make-disk-image, run-qemu, ...) stop asking uname -m which arch this
# build is for.  They fall back to uname -m when the stamp is missing,
# which keeps them working against pre-#404 build directories.
file(WRITE ${CMAKE_BINARY_DIR}/uros-arch "${UROS_TARGET_ARCH}\n")

# The machine/ indirection, per target (#404).  Mach dispatches MI code
# to the active architecture through two include-path names:
#   <machine/...>       -> src/mach_kernel/<arch>/
#   <mach/machine/...>  -> src/mach_kernel/mach/<arch>/
# Historically both were symlinks INSIDE the source tree pointing at
# i386 — state a build system cannot see, wired to one architecture,
# and the reason two targets could not share a tree.  The links now
# live here, in the build directory, created at configure time for
# whatever this build dir targets; the source tree carries none.
# Consumers get -I${UROS_ARCH_INCLUDE} immediately before the
# -I .../src/mach_kernel whose in-tree symlink used to resolve these,
# so relative include priority is exactly what it was.
set(UROS_ARCH_INCLUDE ${CMAKE_BINARY_DIR}/arch-include)
file(MAKE_DIRECTORY ${UROS_ARCH_INCLUDE}/mach)
file(REMOVE ${UROS_ARCH_INCLUDE}/machine ${UROS_ARCH_INCLUDE}/mach/machine)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/src/mach_kernel/${UROS_TARGET_ARCH}
     ${UROS_ARCH_INCLUDE}/machine SYMBOLIC)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/src/mach_kernel/mach/${UROS_TARGET_ARCH}
     ${UROS_ARCH_INCLUDE}/mach/machine SYMBOLIC)

# Export layout customization: platform name for exported artifacts (can be overridden by user)
option(EXPORT_PLATFORM "Platform name used for export directory (default: ${CMAKE_SYSTEM_NAME} or 'uros' when building kernel)" "")
if(NOT EXPORT_PLATFORM)
  if(OSFMK_BUILD_KERNEL)
    set(EXPORT_PLATFORM "uros")
  else()
    set(EXPORT_PLATFORM "${CMAKE_SYSTEM_NAME}")
  endif()
endif()
message(STATUS "EXPORT_PLATFORM = ${EXPORT_PLATFORM}")

# #179 — UAPI split: shared kernel/userspace wire-format headers live in
# uros/uapi/.  Expose the directory via an INTERFACE library so any target
# can simply `target_link_libraries(<x> PRIVATE uros_uapi)` to get the
# include path.  Today only kernel + libmach + a handful of low-level libs
# need it; as more headers migrate, more targets will pick this up.
add_library(uros_uapi INTERFACE)
target_include_directories(uros_uapi INTERFACE
  ${CMAKE_SOURCE_DIR}/uapi
)

# Quick-and-broad: also add it to the global include path so legacy
# CMakeLists with hand-rolled include paths don't need per-file edits
# during the migration.  Once every consumer goes through uros_uapi this
# can be removed.
include_directories(${CMAKE_SOURCE_DIR}/uapi)

# Directory sorgenti principale
include(CTest)
enable_testing()
add_subdirectory(src)

if(UROS_BUILD_MUSL)
  add_subdirectory(src/contrib)
endif()

# Target clean-all: rimuove completamente la directory di build
add_custom_target(clean-all
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}
  COMMENT "Removing entire build directory: ${CMAKE_BINARY_DIR}"
)
