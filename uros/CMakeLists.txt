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
option(OSFMK_BUILD_VIRTIO_BLK "Costruisci il driver virtio-blk userspace" OFF)
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
