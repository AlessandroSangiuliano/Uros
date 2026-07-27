# OSFMK Mach Kernel CMakeLists.txt
# Target: i386 (AT386)
#
# Build order:
# 1. Build migcom (tool) - done in parent
# 2. Generate assym.S from genassym.c
# 3. Generate MIG stubs from .defs files  
# 4. Compile C and assembly sources
# 5. Link kernel with kernel.ld

cmake_minimum_required(VERSION 3.21)

# Kernel is freestanding - no standard C library
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

# =============================================================================
# Build Type Configuration
# =============================================================================
# Usage: cmake -DCMAKE_BUILD_TYPE=Debug ..   (or Release)
# Debug:   -O0 -g3 (full debug info, no optimization)
# Release: -O2 -g  (optimized with basic debug info)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING 
        "Build type: Debug or Release" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Set optimization and debug flags based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(KERNEL_OPT_FLAGS -O0 -g3 -DDEBUG=1)
    message(STATUS "Debug build: -O0 -g3 (no optimization, full debug)")
else()
    set(KERNEL_OPT_FLAGS -O2 -g)
    message(STATUS "Release build: -O2 -g (optimized)")
endif()

# ⚠️ The block above sits ahead of the x86-64 branch on purpose: that branch
# returns before the rest of this file runs, so anything set after it applies
# to i386 alone.  The optimisation flags were in that position and the x86-64
# kernel was built at -O0 by omission — which for #410 in particular is the
# one configuration where a missing volatile or a missing compiler barrier
# cannot show itself.

# ------------------------------------------------------------------ #406
# x86-64 kernel: build the machine-dependent boot stub and stop.
#
# The i386 machinery below assembles the whole SMP/pmap/trap kernel, none
# of which compiles for x86-64 until the MD contracts (#407-#411) land.
# This branch builds contract 1/6 — the boot entry in x86_64/ — as the
# `mach_kernel` output, and returns before the i386 code runs.  The stub
# grows into the real kernel as the other contracts fill x86_64/ in.
#
# The image is ELF64: GRUB boots it via multiboot2 (`multiboot2` loads an
# ELF64 kernel; qemu's `-kernel` multiboot1 loader rejects ELF64 — "give a
# 32bit one" — so the dev loop is a GRUB rescue ISO, `qemu -cdrom`).  The
# object is assembled -m64; the entry itself is still 32-bit code (the
# bootloader hands off in protected mode) via a `.code32` section, and the
# 64-bit half begins after the long-mode transition — one file, two modes.
# The x86-64 tree is one directory per MD contract, so a contract's code,
# its headers and its build inputs sit together and nothing has to be
# untangled later:
#   boot/     #406 long-mode entry, first page tables, descriptor tables
#   pmap/     #407 four-level paging
#   cpu/      shared: control registers, MSRs, CPUID (no single owner)
#   thread/   #408 context switch and thread state
#   trap/     #409 trap and interrupt entry
#   sync/     #410 atomics, locks, barriers
#   syscall/  #411 SYSCALL/SYSRET
# None of those names collide with a top-level kernel directory (vm/, kern/,
# ipc/ ...), so <pmap/pte.h> can never resolve to the wrong header once the
# MI tree is on the include path too.
if(UROS_TARGET_ARCH STREQUAL "x86_64")
  set(X86_64_BOOT_LD ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot.ld)

  add_executable(mach_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/ap_trampoline.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/boot_c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/multiboot2.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/acpi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/cons.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ddb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/ddb/ksym.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/desc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ipi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/ioapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/lapic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/percpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/pic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/spl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/cpu/smp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/bootmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/direct.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/phys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/pmap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/pv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/tlb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/pmap/walk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/probe.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/syscall/syscall.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/pit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/time/tsc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/context.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/context.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/fpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/thread/state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/entry.S
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/trap/trap.c
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/sync/lock.c)
  # <mach/...> and the per-target <mach/machine/...> behind it (#413).
  #
  # The first thing this target has ever taken from outside x86_64/, and it
  # is deliberately the interface headers rather than any of the code that
  # uses them: the message layout is a decision, and a decision is worth
  # nothing until the target that has to live with it compiles the header
  # and checks it.  The MI tree follows later and follows this.
  #
  # ⚠️ Ordered as the i386 target orders them — uapi, then the arch links —
  # so <mach/machine/vm_types.h> resolves to mach/x86_64/ here for exactly
  # the same reason it resolves to mach/i386/ there.
  target_include_directories(mach_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/uapi
    ${UROS_ARCH_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/x86_64)
  # -nostdinc drops the compiler's own freestanding headers too (stdint.h,
  # stddef.h); add just that directory back with -isystem so we get the
  # fixed-width types without pulling in the hosted system headers.
  execute_process(COMMAND ${CMAKE_C_COMPILER} -m64 -print-file-name=include
    OUTPUT_VARIABLE X86_64_GCC_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
  # The C is bare-metal higher-half: -mcmodel=kernel so absolute references
  # land in the top-2 GiB where the image is linked; -mno-red-zone because
  # the red zone is unsafe once interrupts land on these stacks; SSE off
  # (general regs only) to match the kernel's SSE-free discipline.
  #
  # -fno-omit-frame-pointer is kept for the same reason the i386 kernel keeps
  # it: the backtrace walks the frame chain, and a register is a cheaper
  # price than not knowing how the machine got where it died.
  target_compile_options(mach_kernel PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-m64 -ffreestanding -nostdinc>
    $<$<COMPILE_LANGUAGE:C>:${KERNEL_OPT_FLAGS}
      -m64 -mcmodel=kernel -mno-red-zone
      -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-pic
      -fno-pie -fno-stack-protector -fno-builtin -nostdinc
      -fno-omit-frame-pointer
      -isystem ${X86_64_GCC_INC} -std=gnu11 -Wall>)
  target_link_options(mach_kernel PRIVATE
    -m64 -static -nostdlib -nostartfiles -no-pie
    -T ${X86_64_BOOT_LD}
    -Wl,--build-id=none)
  set_target_properties(mach_kernel PROPERTIES
    LINK_DEPENDS ${X86_64_BOOT_LD}
    OUTPUT_NAME "mach_kernel"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/uros/boot")

  # GRUB2 multiboot2 rescue ISO — the dev loop for the x86-64 kernel.
  # `ninja uros_iso` stages the kernel + a one-entry grub.cfg (timeout 0)
  # and calls grub-mkrescue; boot with `qemu-system-x86_64 -cdrom`.
  set(X86_64_ISO_DIR ${CMAKE_BINARY_DIR}/iso-x86_64)
  set(X86_64_ISO ${CMAKE_BINARY_DIR}/uros-x86_64.iso)
  add_custom_target(uros_iso
    COMMAND ${CMAKE_COMMAND} -E make_directory ${X86_64_ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mach_kernel>
            ${X86_64_ISO_DIR}/boot/mach_kernel
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
            ${X86_64_ISO_DIR}/boot/grub/grub.cfg
    COMMAND grub-mkrescue -o ${X86_64_ISO} ${X86_64_ISO_DIR}
    DEPENDS mach_kernel ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/boot/grub.cfg
    COMMENT "Building GRUB2 multiboot2 rescue ISO: ${X86_64_ISO}"
    VERBATIM)

  message(STATUS "x86-64 kernel: boot stub (#406) — MD contracts #407-#411 pending")
  return()
endif()

# =============================================================================
# Kernel Configuration
# =============================================================================

set(KERNEL_TARGET "mach_kernel")
set(KERNEL_TEXTORG "0x100000")  # 1MB - physical load address

# #300 — SMP bring-up.  Default 1 keeps the v0.1.0 single-CPU build unchanged.
# Override with -DUROS_NCPUS=2 (or 4, 8) to enable the SMP code paths.
#
# #330 (dynamic per-CPU, option B): for SMP builds this is now the fixed
# *maximum* CPU count (the size of the per-CPU arrays), NOT the number you
# boot.  Set it once to a generous cap (e.g. 64); the real count comes from
# the MADT/MP table at runtime (real_ncpus, clamped to <= NCPUS).  The same
# binary then boots any N <= UROS_NCPUS with no rebuild and a fixed image
# layout.  See i386/AT386/cpus.h.
set(UROS_NCPUS "1" CACHE STRING "Max CPUs supported (per-CPU array size; 1 = UP). Runtime count from MADT.")
if(UROS_NCPUS GREATER 1)
    message(STATUS "SMP build: UROS_NCPUS=${UROS_NCPUS} (experimental, #300 in progress)")
    # MPS 1.x AP-boot path (mp.c, mp_v1_1.c, slave_boot.S).
    # ACPI MADT path will land via #305.
    set(KERNEL_SMP_DEFINES -DMP_V1_1=1)
else()
    message(STATUS "UP build: UROS_NCPUS=${UROS_NCPUS}")
    set(KERNEL_SMP_DEFINES "")
endif()

# Get GCC's internal include path (for stdarg.h, etc.) EARLY so we can use it in C_FLAGS
# Must use -m32 to get 32-bit compatible include path
execute_process(
    COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -print-file-name=include
    OUTPUT_VARIABLE GCC_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "GCC internal include path: ${GCC_INCLUDE_DIR}")

# Kernel-specific compiler flags
# -isystem is needed with -nostdinc to allow GCC internal headers (stdarg.h, etc.)
set(KERNEL_C_FLAGS
    ${UROS_ARCH_FLAGS}
    -mno-sse
    -ffreestanding
    -fno-stack-protector
    -fno-pic
    -fno-pie
    -nostdlib
    -nostdinc
    -isystem ${GCC_INCLUDE_DIR}
    -fno-builtin
    -fno-common
    -fno-strict-aliasing
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -std=gnu11
    -Wall
    -Wno-implicit-function-declaration
    -Wno-int-conversion  
    -Wno-pointer-sign
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-missing-braces
    -Wno-parentheses
    -Wno-format
    ${KERNEL_OPT_FLAGS}
)

set(KERNEL_ASM_FLAGS
    ${UROS_ARCH_FLAGS}
    -DASSEMBLER
    -nostdinc
    -ffreestanding
)

set(KERNEL_DEFINES
    -DKERNEL
    -DMACH_KERNEL
    -DAT386=1
    -DNCPUS=${UROS_NCPUS}
    -DMACH_ASSERT=1
    -DMACH_DEBUG=1
    -DMACH_KDB=1
    -DMACH_HOST=1
    -DKERNEL_PRIVATE
    -DSTAT_TIME
    -DMACH_MACHINE_ROUTINES
    # -DMACH_PROF  # Disabled - requires prof.defs MIG generation
    -DKERNEL_SERVER=1
    -DTASK_SWAPPER=1
    # ODE device configuration options
    -DEISA=1
    -DHIMEM=1
    -DNPCI=1
    # GCC/ELF does not use underscore prefix for C symbols
    -D__NO_UNDERSCORES__
    ${KERNEL_SMP_DEFINES}
)

# #319 scheduler instrumentation: per-CPU counters + rdtsc around the global
# pset->runq.lock acquire in thread_select, dumped from sched_thread.
# Diagnostic tool: flip ON (cmake -DUROS_S319_INSTRUMENT=ON) when measuring
# scheduler/runq behavior, keep OFF for normal and perf builds.
option(UROS_S319_INSTRUMENT "Build the #319 runq instrumentation (s319 counters + dump)" OFF)
if(UROS_S319_INSTRUMENT)
    list(APPEND KERNEL_DEFINES -DS319_INSTRUMENT=1)
    message(STATUS "#319 s319 scheduler instrumentation: ON")
endif()

# MIG-specific flags - disable type checking for kernel stubs
set(MIG_CFLAGS
    -Dmig_internal=
    -DTypeCheck=0
)

# =============================================================================
# Include directories
# =============================================================================

set(KERNEL_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(KERNEL_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Note: GCC_INCLUDE_DIR is already defined above and added via -isystem in KERNEL_C_FLAGS
# Regular -I include paths should NOT include GCC_INCLUDE_DIR (must use -isystem with -nostdinc)
#
# IMPORTANT: Source directories come FIRST so that original headers (e.g. mach/memory_object.h 
# with type definitions) are found before MIG-generated headers with the same name.
# The build directory is included for generated headers like *_server.h and *_user.c

# ODE-style device config headers for PCI sub-drivers (all disabled).
# The main pci.h lives in i386/AT386/pci.h with NPCI=1.
foreach(_dev_hdr ncr810 ncr825 adsl fta tu)
    if(NOT EXISTS "${KERNEL_SRC_DIR}/i386/AT386/${_dev_hdr}.h")
        file(WRITE "${KERNEL_BUILD_DIR}/${_dev_hdr}.h"
             "#ifndef _CONFIG_${_dev_hdr}_H_\n#define _CONFIG_${_dev_hdr}_H_\n"
             "#define N${_dev_hdr} 0\n#endif\n")
    endif()
endforeach()

set(KERNEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/uapi			# #179 — UAPI split: single source of truth
    ${UROS_ARCH_INCLUDE}
    ${KERNEL_SRC_DIR}
    ${KERNEL_SRC_DIR}/include
    ${KERNEL_SRC_DIR}/i386
    ${KERNEL_SRC_DIR}/i386/AT386
    ${KERNEL_BUILD_DIR}
    ${KERNEL_BUILD_DIR}/mach
    ${KERNEL_BUILD_DIR}/mach_debug
    ${KERNEL_BUILD_DIR}/device
    # Public Uros export headers (LAST so kernel-internal mach/ wins on
    # duplicates).  Lets the kernel pull header-only public headers like
    # <mach/urmach_version.h> from a single source of truth.
    ${CMAKE_SOURCE_DIR}/export/include
)

# =============================================================================
# Step 1: Generate assym.S from genassym.c
#
# genassym.c contains offsetof() expressions that are converted to 
# assembly .set directives. The sed script extracts lines with #DEFINITION#
# =============================================================================

set(GENASSYM_INPUT ${KERNEL_SRC_DIR}/i386/genassym.c)
set(GENASSYM_OUTPUT ${KERNEL_BUILD_DIR}/genassym.s)
set(GENASSYM_DEPFILE ${KERNEL_BUILD_DIR}/genassym.d)
set(ASSYM_OUTPUT ${KERNEL_BUILD_DIR}/assym.S)

# Compile genassym.c to assembly (this produces offset definitions).
# -MD -MF generates a depfile so CMake tracks all transitively included
# headers (kern/thread.h, kern/task.h, ipc/ipc_port.h, etc.) automatically.
# Without this, changes to kernel structs silently produce stale offsets.
add_custom_command(
    OUTPUT ${GENASSYM_OUTPUT}
    DEPFILE ${GENASSYM_DEPFILE}
    COMMAND ${CMAKE_C_COMPILER} ${UROS_ARCH_FLAGS} -S
            -nostdinc
            -ffreestanding
            -isystem ${GCC_INCLUDE_DIR}
            -I${CMAKE_SOURCE_DIR}/uapi
            -I${UROS_ARCH_INCLUDE}
            -I${KERNEL_SRC_DIR}
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            ${KERNEL_DEFINES}
            -MD -MF ${GENASSYM_DEPFILE}
            -o ${GENASSYM_OUTPUT}
            ${GENASSYM_INPUT}
    DEPENDS ${GENASSYM_INPUT}
    COMMENT "Generating genassym.s from genassym.c"
    VERBATIM
)

# Extract #DEFINITION# lines and convert to assym.S
add_custom_command(
    OUTPUT ${ASSYM_OUTPUT}
    COMMAND sed -e "/#DEFINITION#/!d" 
            -e "s/^.*#DEFINITION#//" 
            -e "s/\\$$//"
            -e "s/\\$\\([0-9-]\\)/\\1/g"
            ${GENASSYM_OUTPUT} > ${ASSYM_OUTPUT}
    DEPENDS ${GENASSYM_OUTPUT}
    COMMENT "Generating assym.S"
    VERBATIM
)

add_custom_target(genassym DEPENDS ${ASSYM_OUTPUT})

# =============================================================================
# Step 2: MIG stub generation
#
# Generate server and user stubs from .defs files
# Server stubs: called by kernel to handle incoming messages
# User stubs: called by kernel to send outgoing messages
# =============================================================================

# migcom is built as part of the parent project (OSFMK_BUILD_TOOLS)
# CMake will handle the dependency automatically via $<TARGET_FILE:migcom>

# Create output directories for MIG files
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/mach)
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/mach_debug)
file(MAKE_DIRECTORY ${KERNEL_BUILD_DIR}/device)

# Helper function to add MIG generation rule
# Use -x c to force C preprocessing of .defs files (modern GCC ignores unknown extensions)
function(add_mig_server DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(SERVER_C ${OUTPUT_DIR}/${DEFS_NAME}_server.c)
    set(SERVER_H ${OUTPUT_DIR}/${DEFS_NAME}_server.h)
    
    add_custom_command(
        OUTPUT ${SERVER_C} ${SERVER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -P -x c
                -I${UROS_ARCH_INCLUDE}
                -I${KERNEL_SRC_DIR} 
                -I${KERNEL_SRC_DIR}/include
                -I${KERNEL_SRC_DIR}/i386
                -I${KERNEL_SRC_DIR}/i386/AT386
                ${KERNEL_DEFINES}
                ${DEFS_FILE} | 
                $<TARGET_FILE:migcom>
                -sheader ${SERVER_H}
                -server ${SERVER_C}
                -header /dev/null
                -user /dev/null
        DEPENDS migcom ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (server)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${SERVER_C} PARENT_SCOPE)
endfunction()

function(add_mig_user DEFS_FILE OUTPUT_DIR SUBSYS_NAME)
    get_filename_component(DEFS_NAME ${DEFS_FILE} NAME_WE)
    set(USER_C ${OUTPUT_DIR}/${DEFS_NAME}_user.c)
    # Use _user.h suffix to avoid conflicts with source headers (e.g. mach/memory_object.h)
    set(USER_H ${OUTPUT_DIR}/${DEFS_NAME}_user.h)
    
    # For KernelUser stubs: define KERNEL_USER=1 so .defs files enable
    # the KernelUser subsystem modifier (generates mach_msg_send_from_kernel
    # instead of mach_msg_overwrite). Undefine KERNEL_SERVER to prevent
    # memory_object.defs (which has both guards) from also setting
    # IsKernelServer, which would emit '#undef MACH_KERNEL' and cause
    # type conflicts between user-space and kernel type definitions.
    add_custom_command(
        OUTPUT ${USER_C} ${USER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -P -x c
                -I${UROS_ARCH_INCLUDE}
                -I${KERNEL_SRC_DIR}
                -I${KERNEL_SRC_DIR}/include
                -I${KERNEL_SRC_DIR}/i386
                -I${KERNEL_SRC_DIR}/i386/AT386
                ${KERNEL_DEFINES}
                -DKERNEL_USER=1
                -UKERNEL_SERVER
                ${DEFS_FILE} |
                $<TARGET_FILE:migcom>
                -header ${USER_H}
                -user ${USER_C}
                -server /dev/null
        DEPENDS migcom ${DEFS_FILE}
        COMMENT "MIG: ${DEFS_NAME} (user)"
        VERBATIM
    )
    
    set(${SUBSYS_NAME}_GENERATED ${${SUBSYS_NAME}_GENERATED} ${USER_C} PARENT_SCOPE)
endfunction()

# ---- Mach subsystem MIG files ----

# mach_server.c - kernel receives mach messages
add_mig_server(${KERNEL_SRC_DIR}/mach/mach.defs 
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_port_server.c - kernel receives mach_port messages  
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_port.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_host_server.c - kernel receives mach_host messages
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_host.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_klog_server.c - #200, kernel printf ring drain
add_mig_server(${KERNEL_SRC_DIR}/mach/mach_klog.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# clock_server.c - kernel receives clock messages
add_mig_server(${KERNEL_SRC_DIR}/mach/clock.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# sync_server.c - kernel receives sync messages  
add_mig_server(${KERNEL_SRC_DIR}/mach/sync.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# bootstrap_server.c - kernel receives bootstrap messages
add_mig_server(${KERNEL_SRC_DIR}/mach/bootstrap.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# ledger_server.c - kernel receives ledger messages
add_mig_server(${KERNEL_SRC_DIR}/mach/ledger.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# mach_i386_server.c - kernel receives i386-specific messages
add_mig_server(${KERNEL_SRC_DIR}/mach/i386/mach_i386.defs
               ${KERNEL_BUILD_DIR}/mach MACH)

# exc_user.c - kernel sends exception messages
add_mig_user(${KERNEL_SRC_DIR}/mach/exc.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# memory_object_user.c - kernel sends memory object messages
add_mig_user(${KERNEL_SRC_DIR}/mach/memory_object.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# device_pager_server.c - generated from memory_object.defs for device pager
# The dev_pager.c file includes this with #defines to rename memory_object_* to device_pager_*
add_custom_command(
    OUTPUT ${KERNEL_BUILD_DIR}/device/device_pager_server.c ${KERNEL_BUILD_DIR}/device/device_pager_server.h
    COMMAND ${CMAKE_C_COMPILER} -E -P -x c
            -I${UROS_ARCH_INCLUDE}
            -I${KERNEL_SRC_DIR} 
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            ${KERNEL_DEFINES}
            ${KERNEL_SRC_DIR}/mach/memory_object.defs |
            $<TARGET_FILE:migcom>
            -sheader ${KERNEL_BUILD_DIR}/device/device_pager_server.h
            -server ${KERNEL_BUILD_DIR}/device/device_pager_server.c
            -header /dev/null
            -user /dev/null
    DEPENDS migcom ${KERNEL_SRC_DIR}/mach/memory_object.defs
    COMMENT "MIG: memory_object -> device_pager (server)"
    VERBATIM
)
# NOTE: device_pager_server.c is NOT added to sources because
# dev_pager.c does #include <device/device_pager_server.c>

# memory_object_default_user.c - kernel sends default pager messages
add_mig_user(${KERNEL_SRC_DIR}/mach/memory_object_default.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# clock_reply_user.c - kernel sends clock reply messages
add_mig_user(${KERNEL_SRC_DIR}/mach/clock_reply.defs
             ${KERNEL_BUILD_DIR}/mach MACH)

# ---- Mach debug subsystem ----
add_mig_server(${KERNEL_SRC_DIR}/mach_debug/mach_debug.defs
               ${KERNEL_BUILD_DIR}/mach_debug MACH_DEBUG)

# ---- Device subsystem ----
add_mig_server(${KERNEL_SRC_DIR}/device/device.defs
               ${KERNEL_BUILD_DIR}/device DEVICE)

# device_reply_user.c - kernel sends async device replies
add_mig_user(${KERNEL_SRC_DIR}/device/device_reply.defs
             ${KERNEL_BUILD_DIR}/device DEVICE)

# ---- Device master subsystem (userspace driver primitives) ----
add_mig_server(${KERNEL_SRC_DIR}/device/device_master.defs
               ${KERNEL_BUILD_DIR}/device DEVICE_MASTER)

# Collect all generated MIG sources
set(MIG_GENERATED_SOURCES
    ${MACH_GENERATED}
    ${MACH_DEBUG_GENERATED}
    ${DEVICE_GENERATED}
    ${DEVICE_MASTER_GENERATED}
)

# MIG generates (mach_msg_type_name_t)-1 as a sentinel in static port
# descriptor templates; the value intentionally overflows the 8-bit
# disposition bitfield (truncates to 0xFF).  Suppress the noise.
set_source_files_properties(${MIG_GENERATED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-overflow")

# device_pager_server header needs to exist for ipc_kobject.c
set(DEVICE_PAGER_HEADER ${KERNEL_BUILD_DIR}/device/device_pager_server.h)

# =============================================================================
# Step 3: Source file lists
# =============================================================================

# Core kernel subsystem
set(KERN_SOURCES
    kern/ast.c
    kern/bootstrap.c
    kern/boot_script.c
    kern/lock_smoke.c
    kern/clock.c
    kern/counters.c
    kern/cpu_data.c
    kern/debug.c
    kern/exception.c
    kern/eventcount.c
    kern/etap.c
    kern/lock.c
    kern/host.c
    kern/ipc_clock.c
    kern/ipc_host.c
    kern/ipc_kobject.c
    kern/ipc_mig.c
    kern/ipc_subsystem.c
    kern/ipc_sync.c
    kern/ipc_tt.c
    kern/kalloc.c
    kern/klog.c
    kern/ledger.c
    kern/mach_clock.c
    kern/mach_factor.c
    kern/machine.c
    kern/norma_bootstrap.c
    kern/printf.c
    kern/priority.c
    kern/processor.c
    kern/queue.c
    kern/rcu.c
    kern/rpc.c
    kern/sched_prim.c
    kern/sscanf.c
    kern/startup.c
    kern/subsystem.c
    kern/sync_lock.c
    kern/sync_policy.c
    kern/sync_sema.c
    kern/syscall_emulation.c
    kern/syscall_subr.c
    kern/syscall_sw.c
    kern/cap.c
    kern/sha256.c
    kern/hmac_sha256.c
    kern/task.c
    kern/task_swap.c
    kern/thread.c
    kern/thread_act.c
    kern/thread_pool.c
    kern/timer.c
    kern/posixtime.c
    kern/thread_swap.c
    kern/version.c
    kern/zalloc.c
)

# IPC subsystem
set(IPC_SOURCES
    ipc/ipc_entry.c
    ipc/ipc_hash.c
    ipc/ipc_init.c
    ipc/ipc_kmsg.c
    ipc/ipc_mqueue.c
    ipc/ipc_notify.c
    ipc/ipc_object.c
    ipc/ipc_port.c
    ipc/ipc_pset.c
    ipc/ipc_right.c
    ipc/ipc_space.c
    ipc/ipc_radix.c
    ipc/ipc_table.c
    ipc/ipc_thread.c
    ipc/mach_msg.c
    ipc/mach_port.c
)

# Virtual Memory subsystem
set(VM_SOURCES
    vm/memory_object.c
    vm/vm_external.c
    vm/vm_fault.c
    vm/vm_init.c
    vm/vm_kern.c
    vm/vm_map.c
    vm/vm_object.c
    vm/vm_pageout.c
    vm/vm_resident.c
    vm/vm_user.c
)

# Device subsystem
set(DEVICE_SOURCES
    device/blkio.c
    device/chario.c
    device/cirbuf.c
    device/dev_lookup.c
    device/dev_name.c
    device/dev_pager.c
    device/device_init.c
    device/device_master.c
    device/driver_lock.c
    device/ds_routines.c
    device/io_scatter.c
    device/net_io.c
    device/subrs.c
    device/data_device.c
    device/net_device.c
    device/test_device.c
)

# Kernel debugger
set(DDB_SOURCES
    ddb/db_access.c
    ddb/db_break.c
    ddb/db_command.c
    ddb/db_cond.c
    ddb/db_examine.c
    ddb/db_expr.c
    ddb/db_ext_symtab.c
    ddb/db_input.c
    ddb/db_lex.c
    ddb/db_macro.c
    ddb/db_output.c
    ddb/db_print.c
    ddb/db_run.c
    ddb/db_sym.c
    ddb/db_task_thread.c
    ddb/db_trap.c
    ddb/db_variables.c
    ddb/db_watch.c
    ddb/db_write_cmd.c
    ddb/db_aout.c
    ddb/db_elf.c
    ddb/tr.c
)

# Intel common code
set(INTEL_SOURCES
    intel/pmap.c
    intel/read_fault.c
)

# i386-specific C sources
set(I386_C_SOURCES
    i386/cpuid.c
    i386/db_disasm.c
    i386/db_interface.c
    i386/db_trace.c
    i386/fpu.c
    i386/fpu_sanity.c
    i386/gdt.c
    i386/hardclock.c
    # #358: HWP (hardware P-states) bring-up -- verdict line + enable-if-off,
    # '-E' EPP performance bias.  CPUID-gated, no-op on KVM.
    i386/hwp.c
    i386/io_emulate.c
    i386/io_map.c
    i386/iopb.c
    i386/nmi_watchdog.c
    i386/ktss.c
    i386/ldt.c
    i386/loose_ends.c
    i386/net_filter.c
    i386/pcb.c
    i386/phys.c
    i386/kmap.c
    i386/pic.c
    i386/i386_rpc.c
    i386/rtclock.c
    i386/sysenter.c
    i386/trap.c
    i386/user_ldt.c
)

# AT386 platform-specific C sources
# NOTE: fd.c removed - floppy driver obsolete in 2026
set(AT386_SOURCES
    i386/AT386/autoconf.c
    i386/AT386/bbclock.c
    i386/AT386/bios_label.c
    i386/AT386/com.c
    i386/AT386/conf.c
    i386/AT386/eisa.c
    i386/AT386/himem.c
    i386/AT386/iopl.c
    # In-kernel keyboard/mouse drivers retired in #208 (kd.c, kd_event.c,
    # kd_mouse.c, kd_queue.c gone; ps2.so under char_server owns the
    # keyboard, a future ps2_mouse.so will own the mouse).
    i386/AT386/console_cdev.c
    i386/AT386/ddb_kbd.c
    # Emergency in-kernel framebuffer console (#342): the ONLY way to see
    # boot/panic output on pure-UEFI machines (no serial, no legacy 0xB8000).
    # Not a display driver — the userspace gpu_server still owns the screen.
    i386/AT386/fbcons.c
    i386/AT386/model_dep.c
    i386/AT386/physmem.c
    i386/AT386/pic_isa.c
    # #357: idle HLT (machine_idle + wake doorbell; POWER_SAVE=1 in
    # power_save.h, -S boot flag reverts to the legacy poll idle).
    i386/AT386/power_save.c
    # In-kernel VGA driver removed in #199 (vga.c, vga.h, vgareg.h,
    # vga_entries.h gone; userspace gpu_server owns the display).
)

# #300 — SMP bring-up sources, only when UROS_NCPUS > 1.
# mp.c is the high-level shim (slave_clock, cause_ast_check, cpu_int_word[]).
# mp_stub.c provides the link-time symbols (cpu_start, mp_probe_cpus, ...)
# that mp_v1_1.c would have hosted.  The historical mp_v1_1.c is kept out
# of the build until Increment 3 replaces it with a modern MPS 1.4 parser.
# slave_boot.S (added below to AT386_ASM_SOURCES) is the AP trampoline.
if(UROS_NCPUS GREATER 1)
    list(APPEND AT386_SOURCES
        i386/mp_desc.c
        i386/lapic.c
        i386/ioapic.c
        i386/AT386/mp/mp.c
        i386/AT386/mp/mp_stub.c
        i386/AT386/mp/mp_table.c
    )
endif()

# #309: per-CPU SSE/FPU sanity check is useful both on UP (cheap
# baseline) and SMP (proves AP CR4/FPU init worked).  Goes into the
# common i386 source list.

# Chips/busses
set(CHIPS_SOURCES
    chips/busses.c
)

# PCI subsystem
set(PCI_SOURCES
    busses/pci/pci.c
    i386/pci/pcibios.c
    i386/pci/i386_pci.c
    i386/pci/pci_config.c
)

# SCSI subsystem
set(SCSI_SOURCES
    # scsi/scsi_info.c  # Disabled - too many dependencies
    device/device_stubs.c  # Stub implementations for missing device functions
)

# All C sources
set(KERNEL_C_SOURCES
    ${KERN_SOURCES}
    ${IPC_SOURCES}
    ${VM_SOURCES}
    ${DEVICE_SOURCES}
    ${DDB_SOURCES}
    ${INTEL_SOURCES}
    ${I386_C_SOURCES}
    ${AT386_SOURCES}
    ${CHIPS_SOURCES}
    ${PCI_SOURCES}
    ${SCSI_SOURCES}
)

# =============================================================================
# Step 4: Assembly sources
#
# The kernel has a special locore.S that is built from concatenating:
#   assym.S + start.S + locore.S + cswitch.S
# =============================================================================

# Individual assembly files (not part of locore)
set(I386_ASM_SOURCES
    i386/_setjmp.S
    i386/bcopy.S
    i386/bzero.S
    i386/gcc.S
    i386/i386_lock.S
    i386/idt.S
    i386/interrupt.S
    i386/ntoh.S
    i386/spl.S
)

set(AT386_ASM_SOURCES
    i386/AT386/kdasm.S
)

# #300 — AP trampoline (real-mode -> 32-bit PM) when UROS_NCPUS > 1.
# #302 — IPI entry stubs (3 vectors).
if(UROS_NCPUS GREATER 1)
    list(APPEND AT386_ASM_SOURCES
        i386/AT386/mp/slave_boot.S
        i386/ipi.S
    )
endif()

# locore.S is special - concatenated from multiple files
set(LOCORE_DEPS
    ${ASSYM_OUTPUT}
    ${KERNEL_SRC_DIR}/i386/start.S
    ${KERNEL_SRC_DIR}/i386/locore.S
    ${KERNEL_SRC_DIR}/i386/cswitch.S
)

set(LOCORE_OUTPUT ${KERNEL_BUILD_DIR}/locore.S)

add_custom_command(
    OUTPUT ${LOCORE_OUTPUT}
    COMMAND cat ${LOCORE_DEPS} > ${LOCORE_OUTPUT}
    DEPENDS ${LOCORE_DEPS}
    COMMENT "Generating locore.S (concatenated startup code)"
    VERBATIM
)

add_custom_target(locore DEPENDS ${LOCORE_OUTPUT})

# =============================================================================
# Step 5: Build kernel executable
# =============================================================================

# Prefix source paths
list(TRANSFORM KERNEL_C_SOURCES PREPEND ${KERNEL_SRC_DIR}/)
list(TRANSFORM I386_ASM_SOURCES PREPEND ${KERNEL_SRC_DIR}/)
list(TRANSFORM AT386_ASM_SOURCES PREPEND ${KERNEL_SRC_DIR}/)

# Generate an empty mach_interface.h stub in the kernel build directory.
# Needed because migcom emits #include <mach/mach_interface.h> in user stubs;
# the kernel doesn't need its contents.  This file lives ONLY in the build dir
# so it doesn't shadow the real export/include/mach/mach_interface.h
# that the userspace build uses.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mach/mach_interface.h"
    "#ifndef _MACH_MACH_INTERFACE_H_\n"
    "#define _MACH_MACH_INTERFACE_H_\n"
    "/* nothing needed here for in-kernel MIG user stubs */\n"
    "#endif\n"
)

# Create kernel executable
add_executable(${KERNEL_TARGET}
    ${KERNEL_C_SOURCES}
    ${I386_ASM_SOURCES}
    ${AT386_ASM_SOURCES}
    ${LOCORE_OUTPUT}
    ${MIG_GENERATED_SOURCES}
    ${DEVICE_PAGER_HEADER}
)

# Ensure dependencies are built first
add_dependencies(${KERNEL_TARGET} genassym locore)

# Set compile options
target_compile_options(${KERNEL_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${KERNEL_C_FLAGS}>
    $<$<COMPILE_LANGUAGE:ASM>:${KERNEL_ASM_FLAGS}>
)

target_compile_definitions(${KERNEL_TARGET} PRIVATE
    ${KERNEL_DEFINES}
    # MIG server stubs default to TypeCheck=1 via #ifndef guard.
    # The original ODE build passed -DTypeCheck=0 in MIG_CFLAGS;
    # replicate that here so the kernel skips MIG type-checking
    # (which can fail spuriously, e.g. OOL-ports disposition not
    # translated by ipc_kmsg_copyin when count==0).
    TypeCheck=0
    mig_internal=
)

# Include directories
target_include_directories(${KERNEL_TARGET} PRIVATE
    ${KERNEL_INCLUDES}
)

# Linker options
set(KERNEL_LINKER_SCRIPT ${KERNEL_SRC_DIR}/i386/AT386/kernel.ld)

target_link_options(${KERNEL_TARGET} PRIVATE
    ${UROS_ARCH_FLAGS}
    -static
    -nostdlib
    -nostartfiles
    -no-pie
    -T ${KERNEL_LINKER_SCRIPT}
    -Wl,--build-id=none
)

set_target_properties(${KERNEL_TARGET} PROPERTIES
    LINK_DEPENDS ${KERNEL_LINKER_SCRIPT}
    OUTPUT_NAME "mach_kernel"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/uros/boot"
)

# =============================================================================
# Issue #211 — DDB symbol table (debug builds only).
#
# .symtab/.strtab are non-allocated ELF sections, so the multiboot loader
# does not load them.  Generate a self-describing binary (ksyms.bin) from
# the linked kernel and ship it as a separate multiboot module
# (run-qemu.sh appends it to -initrd if present).  The kernel locates
# the module by its magic at boot and feeds it to DDB via db_elf.c.
#
# OSFMK_GEN_KSYMS defaults ON for Debug, OFF for Release — see the
# top-level CMakeLists.  When OFF, the file is simply not produced;
# run-qemu.sh boots without DDB symbol-by-name lookup, exactly as the
# pre-#211 behaviour.
# =============================================================================
if(OSFMK_GEN_KSYMS)
    set(KERNEL_KSYMS "${CMAKE_BINARY_DIR}/export/uros/boot/ksyms.bin")
    add_custom_command(
        OUTPUT ${KERNEL_KSYMS}
        COMMAND ${CMAKE_SOURCE_DIR}/../scripts/gen-ksyms.py
                $<TARGET_FILE:${KERNEL_TARGET}> ${KERNEL_KSYMS}
        DEPENDS ${KERNEL_TARGET} ${CMAKE_SOURCE_DIR}/../scripts/gen-ksyms.py
        COMMENT "Generating kernel ksyms.bin (DDB symbol table, #211)"
        VERBATIM
    )
    add_custom_target(mach_kernel_ksyms ALL DEPENDS ${KERNEL_KSYMS})

    # Stale ksyms.bin from a previous Debug build would silently mismatch
    # the current kernel — wipe it on Release reconfigures.
else()
    file(REMOVE "${CMAKE_BINARY_DIR}/export/uros/boot/ksyms.bin")
endif()

# =============================================================================
# Info
# =============================================================================

message(STATUS "Configuring Mach Kernel for i386/AT386")
message(STATUS "  Load address: ${KERNEL_TEXTORG}")
message(STATUS "  Source dir: ${KERNEL_SRC_DIR}")
message(STATUS "  Build dir: ${KERNEL_BUILD_DIR}")

# =============================================================================
# Flat-model verification tests (#47/#51)
# =============================================================================
set(KERNEL_BIN "$<TARGET_FILE:${KERNEL_TARGET}>")

# --- ELF property checks ---

# 1. Kernel must be a 32-bit ELF
add_test(NAME kernel_elf32
  COMMAND sh -c "LANG=C readelf -h '${KERNEL_BIN}' | grep -q 'Class:.*ELF32'"
)

# 2. Entry point must be in kernel virtual space (0xC0xxxxxx)
add_test(NAME kernel_entry_in_kernel_space
  COMMAND sh -c "LANG=C readelf -h '${KERNEL_BIN}' | grep 'Entry point' | grep -q '0xc0'"
)

# 3. .text virtual address must start at 0xC0100000+ (KERNELBASE + 1MB)
add_test(NAME kernel_text_vaddr
  COMMAND sh -c "LANG=C readelf -S '${KERNEL_BIN}' | grep '\\.text' | awk 'BEGIN{r=1} {for(i=1;i<=NF;i++) if(\$i ~ /^c010/) r=0} END{exit r}'"
)

# 4. No LINEAR_KERNEL_ADDRESS symbol in kernel (legacy removed)
add_test(NAME kernel_no_linear_kernel_addr_symbol
  COMMAND sh -c "! LANG=C readelf -s '${KERNEL_BIN}' 2>/dev/null | grep -qi 'LINEAR_KERNEL'"
)

# --- QEMU boot smoke test ---
find_program(QEMU_I386 qemu-system-i386)
if(QEMU_I386)
  add_test(NAME kernel_boot_smoke
    COMMAND sh -c "
      LOG=\$(mktemp)
      ${QEMU_I386} -m 128 -no-reboot -display none \
        -serial file:\$LOG \
        -kernel '${KERNEL_BIN}' &
      QPID=\$!
      sleep 8
      kill \$QPID 2>/dev/null; wait \$QPID 2>/dev/null
      PASS=true
      grep -q 'OSFMK'           \$LOG || PASS=false
      grep -q 'sysenter.*fast'  \$LOG || PASS=false
      grep -q 'vm_page_bootstrap' \$LOG || PASS=false
      if grep -q 'panic.*freed zone' \$LOG; then PASS=false; fi
      rm -f \$LOG
      \$PASS
    "
  )
  set_tests_properties(kernel_boot_smoke PROPERTIES
    TIMEOUT 30
    LABELS "boot;smoke"
  )
endif()
