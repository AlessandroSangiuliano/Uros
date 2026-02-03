cmake_minimum_required(VERSION 3.16)

project(osfmk
  VERSION 0.1
  LANGUAGES C ASM
)

# Opzioni principali di configurazione
option(OSFMK_TARGET_AT386 "Abilita configurazione Mach per AT386 (x86)" ON)
option(OSFMK_BUILD_TOOLS "Costruisci i tool host (mig, migcom, ecc.)" ON)
option(OSFMK_BUILD_KERNEL "Costruisci il kernel Mach" OFF)
option(OSFMK_BUILD_BOOTSTRAP "Costruisci il bootstrap task" OFF)
option(OSFMK_BUILD_DEFAULT_PAGER "Costruisci il default pager" OFF)

# Directory sorgenti principale
add_subdirectory(src)


