cmake_minimum_required(VERSION 3.16)

# uros/src/mach_services/lib — the genuinely inherited OSF libraries (#396).
#
# The actively-developed Uros libraries (and migcom + the MIG helper
# functions) live in src/lib, which src/CMakeLists adds BEFORE this
# directory — so add_mig_defs() and the maybe_add macro are already
# defined by the time this file runs.

# Same -fPIC rationale as src/lib: these statics link into PIE servers.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

maybe_add(libmach_p)
maybe_add(libmach_maxonstack)
# libsa_mach merged into libmach (#106)
maybe_add(libmachid)
maybe_add(libcthreads)
maybe_add(libcthreads_p)
maybe_add(librthreads)
#maybe_add(libxmm)  # excluded for now: needs K&R modernization
maybe_add(libservice)
maybe_add(libnetmemory)
