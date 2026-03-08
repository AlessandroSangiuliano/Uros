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
