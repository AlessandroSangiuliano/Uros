# Bootstrap server build with CMake

cmake_minimum_required(VERSION 3.21)
project(bootstrap_server C)

set(BOOTSTRAP_SRC
    bootstrap.c
    service.c
    a_out.c
    coff.c
    elf.c
    load.c
    rose.c
    som.c
    strfcns.c
)

add_library(bootstrap_server STATIC ${BOOTSTRAP_SRC})

# Ensure bootstrap can find Mach headers (mach.h, mach_init.h)
# Start with our local include dirs
target_include_directories(bootstrap_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../mach_services/include
    ${CMAKE_SOURCE_DIR}/src/mach_kernel
)

# Prefer generated/installed export includes (platform-specific).  Try a couple of
# likely locations and add the first one that contains mach/mach_interface.h.
# This avoids hard-coding a platform like "powermac" when not needed.
find_path(MACH_INTERFACE_DIR
    NAMES mach/mach_interface.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/../../export/powermac/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../export
        ${CMAKE_SOURCE_DIR}/export/powermac/include
        ${CMAKE_SOURCE_DIR}/export
)

if(MACH_INTERFACE_DIR)
    target_include_directories(bootstrap_server PRIVATE ${MACH_INTERFACE_DIR})
else()
    message(WARNING "Could not locate 'mach/mach_interface.h' in export paths; if build fails, set OSFMK export include path or provide generated headers.")
endif()

# Some exported headers (e.g., <cthreads.h>) include <machine/cthreads.h> which
# may not exist for host builds. If that header is missing, generate a minimal
# stub in the build tree and add it to the include path so compilation succeeds.
find_path(MACHINE_CTHREADS_PATH
    NAMES machine/cthreads.h
    PATHS
        ${MACH_INTERFACE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../export/powermac/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../export
        ${CMAKE_SOURCE_DIR}/export
)

if(NOT MACHINE_CTHREADS_PATH)
    set(GEN_MACHINE_DIR ${CMAKE_BINARY_DIR}/generated/include/machine)
    file(MAKE_DIRECTORY ${GEN_MACHINE_DIR})
    set(GEN_CTHREADS ${GEN_MACHINE_DIR}/cthreads.h)
    if(NOT EXISTS ${GEN_CTHREADS})
        file(WRITE ${GEN_CTHREADS} "/* Auto-generated stub: machine/cthreads.h */\n#ifndef _MACHINE_CTHREADS_H_\n#define _MACHINE_CTHREADS_H_\ntypedef int spin_lock_t;\n#endif /* _MACHINE_CTHREADS_H_ */\n")
    endif()
    target_include_directories(bootstrap_server PRIVATE ${CMAKE_BINARY_DIR}/generated/include)
endif()

# Optional: Add platform-specific sources
#if (CMAKE_SYSTEM_PROCESSOR STREQUAL "i386")
#    target_sources(bootstrap_server PRIVATE i386/some_file.c)
#endif

# Optional: Export header
# install(FILES bootstrap.h DESTINATION include)
