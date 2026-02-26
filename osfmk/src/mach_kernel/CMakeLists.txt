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

# =============================================================================
# Kernel Configuration
# =============================================================================

set(KERNEL_TARGET "mach_kernel")
set(KERNEL_TEXTORG "0x100000")  # 1MB - physical load address

# Get GCC's internal include path (for stdarg.h, etc.) EARLY so we can use it in C_FLAGS
# Must use -m32 to get 32-bit compatible include path
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -m32 -print-file-name=include
    OUTPUT_VARIABLE GCC_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "GCC internal include path: ${GCC_INCLUDE_DIR}")

# Kernel-specific compiler flags
# -isystem is needed with -nostdinc to allow GCC internal headers (stdarg.h, etc.)
set(KERNEL_C_FLAGS
    -m32
    -march=i686
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
    -m32
    -DASSEMBLER
    -nostdinc
    -ffreestanding
)

set(KERNEL_DEFINES
    -DKERNEL
    -DMACH_KERNEL
    -DAT386=1
    -DNCPUS=1
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
    -DNPCI=0
    # GCC/ELF does not use underscore prefix for C symbols
    -D__NO_UNDERSCORES__
)

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

set(KERNEL_INCLUDES
    ${KERNEL_SRC_DIR}
    ${KERNEL_SRC_DIR}/include
    ${KERNEL_SRC_DIR}/i386
    ${KERNEL_SRC_DIR}/i386/AT386
    ${KERNEL_BUILD_DIR}
    ${KERNEL_BUILD_DIR}/mach
    ${KERNEL_BUILD_DIR}/mach_debug
    ${KERNEL_BUILD_DIR}/device
)

# =============================================================================
# Step 1: Generate assym.S from genassym.c
#
# genassym.c contains offsetof() expressions that are converted to 
# assembly .set directives. The sed script extracts lines with #DEFINITION#
# =============================================================================

set(GENASSYM_INPUT ${KERNEL_SRC_DIR}/i386/genassym.c)
set(GENASSYM_OUTPUT ${KERNEL_BUILD_DIR}/genassym.s)
set(ASSYM_OUTPUT ${KERNEL_BUILD_DIR}/assym.S)

# Compile genassym.c to assembly (this produces offset definitions)
add_custom_command(
    OUTPUT ${GENASSYM_OUTPUT}
    COMMAND ${CMAKE_C_COMPILER} -m32 -S 
            -nostdinc
            -ffreestanding
            -isystem ${GCC_INCLUDE_DIR}
            -I${KERNEL_SRC_DIR}
            -I${KERNEL_SRC_DIR}/include
            -I${KERNEL_SRC_DIR}/i386
            -I${KERNEL_SRC_DIR}/i386/AT386
            ${KERNEL_DEFINES}
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
    
    add_custom_command(
        OUTPUT ${USER_C} ${USER_H}
        COMMAND ${CMAKE_C_COMPILER} -E -P -x c
                -I${KERNEL_SRC_DIR}
                -I${KERNEL_SRC_DIR}/include
                -I${KERNEL_SRC_DIR}/i386
                -I${KERNEL_SRC_DIR}/i386/AT386
                ${KERNEL_DEFINES}
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
# TODO: device.defs has multi-line type definitions that confuse migcom
# Disabling for now - the device subsystem will need stub implementations
#add_mig_server(${KERNEL_SRC_DIR}/device/device.defs
#               ${KERNEL_BUILD_DIR}/device DEVICE)

# TODO: device_reply.defs may still have issues
#add_mig_user(${KERNEL_SRC_DIR}/device/device_reply.defs
#             ${KERNEL_BUILD_DIR}/device DEVICE)

# Collect all generated MIG sources
set(MIG_GENERATED_SOURCES
    ${MACH_GENERATED}
    ${MACH_DEBUG_GENERATED}
    ${DEVICE_GENERATED}
)

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
    kern/ledger.c
    kern/mach_clock.c
    kern/mach_factor.c
    kern/machine.c
    kern/norma_bootstrap.c
    kern/printf.c
    kern/priority.c
    kern/processor.c
    kern/queue.c
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
    ipc/ipc_splay.c
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
    i386/gdt.c
    i386/hardclock.c
    i386/io_emulate.c
    i386/io_map.c
    i386/iopb.c
    i386/ktss.c
    i386/ldt.c
    i386/loose_ends.c
    i386/net_filter.c
    i386/pcb.c
    i386/phys.c
    i386/pic.c
    i386/i386_rpc.c
    i386/rtclock.c
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
    i386/AT386/hd.c
    i386/AT386/himem.c
    i386/AT386/iopl.c
    i386/AT386/kd.c
    i386/AT386/kd_event.c
    i386/AT386/kd_mouse.c
    i386/AT386/kd_queue.c
    i386/AT386/model_dep.c
    i386/AT386/physmem.c
    i386/AT386/pic_isa.c
    i386/AT386/vga.c
)

# Chips/busses
set(CHIPS_SOURCES
    chips/busses.c
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
)

# Include directories
target_include_directories(${KERNEL_TARGET} PRIVATE
    ${KERNEL_INCLUDES}
)

# Linker options
set(KERNEL_LINKER_SCRIPT ${KERNEL_SRC_DIR}/i386/AT386/kernel.ld)

target_link_options(${KERNEL_TARGET} PRIVATE
    -m32
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
)

# =============================================================================
# Info
# =============================================================================

message(STATUS "Configuring Mach Kernel for i386/AT386")
message(STATUS "  Load address: ${KERNEL_TEXTORG}")
message(STATUS "  Source dir: ${KERNEL_SRC_DIR}")
message(STATUS "  Build dir: ${KERNEL_BUILD_DIR}")
