cmake_minimum_required(VERSION 3.16)

# uros/src/mach_services/lib — the genuinely inherited OSF libraries (#396).
#
# The actively-developed Uros libraries (and migcom + the MIG helper
# functions) live in src/lib, which src/CMakeLists adds BEFORE this
# directory — so add_mig_defs() and the maybe_add macro are already
# defined by the time this file runs.

# Same -fPIC rationale as src/lib: these statics link into PIE servers.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# libsa_mach merged into libmach (#106)
# libcthreads, libcthreads_p and librthreads: retired rather than ported
# (#425, #434), and their sources deleted (#433).  The live path is
# libpthreads plus libmach's crt0.
maybe_add(libservice)
