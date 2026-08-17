cmake_minimum_required(VERSION 3.21)

project(uros
  VERSION 0.1
  LANGUAGES C ASM
)

# Emit compile_commands.json.
#
# Not for editors, though they benefit: it is so that a tool which needs to
# know how this tree compiles something can ASK, instead of keeping its own
# copy of the flags.  scripts/mi-x86_64-probe.sh kept one, and it drifted --
# three separate measurement errors came from a probe that was compiling with
# -DMACH_KDB=1 and -DAT386=1 while the build was not (#453).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
  set(UROS_PTR_BITS 32)
elseif(UROS_TARGET_ARCH STREQUAL "x86_64")
  set(UROS_ARCH_FLAGS -m64)
  set(UROS_PTR_BITS 64)
else()
  message(FATAL_ERROR "UROS_TARGET_ARCH must be i386 or x86_64, got '${UROS_TARGET_ARCH}'")
endif()
# String form for raw COMMAND / shell contexts (list form feeds
# target_compile_options and friends).
string(JOIN " " UROS_ARCH_FLAGS_STR ${UROS_ARCH_FLAGS})
message(STATUS "UROS_TARGET_ARCH = ${UROS_TARGET_ARCH} (${UROS_ARCH_FLAGS_STR})")

# Which machine migcom generates message layout for, said once (#426).
#
# ⚠️ Here and not in a directory, and that is the point.  It used to be set
# twice inside src/mach_kernel/CMakeLists.txt -- once per architecture, in
# that directory's scope -- so it was EMPTY in every other directory that runs
# mig, and an empty target argument is not a missing flag: migcom defaults to
# i386 and generates i386 layout in silence.  cmake/mig.cmake says so at
# length, and add_mig_userland derives the target again by hand for exactly
# this reason.
#
# A variable that must be right in twenty CMakeLists is a variable that will
# be wrong in one of them.  Setting it where the architecture is decided means
# no caller has to know the spelling, and none of them can be somewhere it has
# not reached.
set(UROS_MIG_TARGET_ARGS -target ${UROS_TARGET_ARCH})

# What the .defs themselves are preprocessed with, said in the same one place.
#
# A .defs is text for migcom, and it is preprocessed by the HOST compiler with
# no -m32/-m64 -- so __i386__ and __x86_64__ inside one describe the machine
# doing the build and not the machine being built for.  These two macros are
# how a .defs asks about the target instead.
#
# 🔑 MIG_PTR_BITS, and it is the one to reach for first.  A type like
# zone_info_t is not larger on x86-64 because the target is called x86_64: it
# is larger because a vm_size_t is 64 bits there.  Written as a width, the
# condition states the actual reason, and the next 64-bit machine -- rv64 is
# where this tree is going -- inherits every one of them by saying how wide it
# is.  Written as `defined(MIG_ARCH_X86_64)' each of those types would go
# quietly back to the 32-bit layout on that machine, and each would have to be
# found again.
#
# MIG_ARCH_<NAME> stays, for the questions whose answer really is the machine:
# i386's descriptor_t is about the i386, not about four-byte pointers.  The
# two are not interchangeable, and the one that names the reason is the one to
# use.
string(TOUPPER "${UROS_TARGET_ARCH}" UROS_MIG_ARCH_UPPER)
set(UROS_MIG_DEFS_ARCH -DMIG_ARCH_${UROS_MIG_ARCH_UPPER}=1
                       -DMIG_PTR_BITS=${UROS_PTR_BITS})

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

# The ODE configuration knobs, per target (#450).  Same reasoning as the
# links above and the same place to do it: state the build needs, wired to
# one architecture when it lived in the source tree, now written here at
# configure time for whatever this build dir targets.  See
# cmake/kernel-config.cmake for why these stopped being tracked files.
include(${CMAKE_SOURCE_DIR}/cmake/kernel-config.cmake)
set(UROS_CONFIG_INCLUDE ${CMAKE_BINARY_DIR}/config-include)
uros_write_config_headers(${UROS_CONFIG_INCLUDE} ${UROS_TARGET_ARCH})

# MIG stub generation, shared by both targets (#416).  The rules used to sit
# below the x86-64 branch's return() with -I i386 written into them, so the
# new target could not run mig at all.
include(${CMAKE_SOURCE_DIR}/cmake/mig.cmake)

# The .defs every mig rule reads and none of them declares (#413).
#
# A mig rule names the generator and the one interface file it is handed.
# It does not depend on what that file
# #includes, which is where the type declarations live: the widths and the
# translations that decide every offset and every message size in the
# generated code.  Edit one and nothing rebuilds; the stubs stay as they were
# until something unrelated forces a regeneration, and in between the build
# is quietly generating from a definition that is no longer on disk.
#
# That is a bad way to find out you changed an ABI.  It also makes the one
# cheap regression test for #416 — "i386 output unchanged" — impossible to
# run honestly, because both sides of the comparison are the same generation.
#
# Listing both trees is deliberate: the kernel reads src/mach_kernel/mach/,
# userland reads export/include/mach/, and depending on a file a given rule
# happens not to read costs a rebuild while missing one costs correctness.
#
# 🔥 Found by GLOB and not by hand, and that is the repair of this list rather
# than a tidying of it.  Written out, it named four files and the target's
# machine_types.defs -- and left out mach_debug_types.defs, clock_types.defs,
# device_types.defs and default_pager_types.defs, which declare types by
# exactly the same rules.  So the paragraph above described a defect the list
# underneath it still had: mach_debug_types.defs was corrected for x86-64 and
# nothing regenerated, and the way that was noticed was a deliberately WRONG
# number failing to break the build (#422).
#
# A list of the files that declare types is a list somebody has to remember to
# add to.  A pattern is not: any *_types.defs in either tree is one of these by
# construction, and CONFIGURE_DEPENDS makes a new one re-run cmake rather than
# wait to be noticed.  It sweeps up the machine_types.defs of machines this
# build has never targeted, which costs nothing -- nobody edits hp_pa.
#
# 🔥 AND uapi, NOT export/include (#481).  The interface .defs moved to uapi in
# #471 and this pattern did not follow them, so from that day mach_types.defs
# was in no rule's dependencies: the very file #470 corrected and #478 changed
# could be edited with the stubs keeping their old layout -- the failure this
# list exists to prevent, reintroduced by the move that was meant to end it.
file(GLOB_RECURSE UROS_MIG_COMMON_DEFS CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/src/mach_kernel/*_types.defs
     ${CMAKE_SOURCE_DIR}/uapi/*_types.defs)

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

# #471 — the .defs live here too, and for a stronger reason than the headers.
# A .defs file IS the user/kernel boundary rather than a convenience for
# reaching it, so having two of them is having two boundaries.  There were
# two, in export/include/mach and src/mach_kernel/mach, and neither build
# could see the other's: the kernel's MIG run does not put export on its
# path and userland's does not put the kernel tree first.  In a year they
# accumulated no difference in content and then, in one evening, one -- a
# `destructor:' that #478 needed, which landed in one copy.
#
# MIG resolves `import <mach/foo.defs>' off its include path, so the way to
# make that unrepeatable is the path itself: one directory holds the files,
# the old two hold none, and a second copy cannot be found rather than
# merely being discouraged.
set(UROS_UAPI_DIR ${CMAKE_SOURCE_DIR}/uapi)

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
