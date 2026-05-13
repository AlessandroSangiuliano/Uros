cmake_minimum_required(VERSION 3.21)

project(osfmk
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
option(OSFMK_BUILD_AHCI_DRIVER "Costruisci il driver AHCI userspace" OFF)
option(OSFMK_BUILD_VIRTIO_BLK "Costruisci il driver virtio-blk userspace" OFF)
option(OSFMK_BUILD_BLOCK_SERVER "Costruisci il block device server modulare" OFF)
option(OSFMK_BUILD_HAL_SERVER "Costruisci l'HAL server (discovery + device registry)" OFF)
option(OSFMK_BUILD_NAME_SERVER "Costruisci il name server (netname)" OFF)
option(OSFMK_BUILD_EXT2_SERVER "Costruisci il server ext2 filesystem userspace" OFF)
option(OSFMK_BUILD_PTHREAD_TEST "Build pthread test server" OFF)
option(OSFMK_BUILD_CAP_SERVER "Build capability server (Uros cap_server + libcap)" OFF)
option(OSFMK_BUILD_CAP_TEST "Build capability negative-test binary" OFF)
option(OSFMK_BUILD_GPU_SERVER "Build gpu_server (userspace GPU/display server, #194)" OFF)
option(OSFMK_BUILD_CHAR_SERVER "Build char_server (userspace character device server, #205)" OFF)

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
option(EXPORT_PLATFORM "Platform name used for export directory (default: ${CMAKE_SYSTEM_NAME} or 'osfmk' when building kernel)" "")
if(NOT EXPORT_PLATFORM)
  if(OSFMK_BUILD_KERNEL)
    set(EXPORT_PLATFORM "osfmk")
  else()
    set(EXPORT_PLATFORM "${CMAKE_SYSTEM_NAME}")
  endif()
endif()
message(STATUS "EXPORT_PLATFORM = ${EXPORT_PLATFORM}")

# Directory sorgenti principale
include(CTest)
enable_testing()
add_subdirectory(src)

# Target clean-all: rimuove completamente la directory di build
add_custom_target(clean-all
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}
  COMMENT "Removing entire build directory: ${CMAKE_BINARY_DIR}"
)
