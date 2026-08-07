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
# libmachid: built, installed, and linked by nothing — the six mentions of
# libmachid.a in build.ninja are all its own build, install and phony rules.
# It also never carried UROS_ARCH_FLAGS, so it was compiled at the host's
# width and archived into export/.../i386/user/lib as if it were target code,
# holding 32-bit Mach types the whole time.  Nobody noticed because nobody
# links it; #453's <mach/i386/vm_types.h> guard is what surfaced it.
#
# The sources stay in the tree and the arch flags are now in its own
# CMakeLists, so uncommenting is all it takes if something ever needs the
# Mach ID service.  Verdict belongs to #433, the heritage-husk inventory.
#maybe_add(libmachid)
maybe_add(libcthreads)
maybe_add(libcthreads_p)
maybe_add(librthreads)
#maybe_add(libxmm)  # excluded for now: needs K&R modernization
maybe_add(libservice)
maybe_add(libnetmemory)
