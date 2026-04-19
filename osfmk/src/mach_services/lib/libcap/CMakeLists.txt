cmake_minimum_required(VERSION 3.16)
project(libcap C)

# Generate MIG stubs from cap_server.defs (user + server).  Both sides are
# emitted: user stubs are linked into libcap for clients; server stubs are
# compiled by the cap_server executable target (osfmk/src/cap_server/).
set(MIG_SCRIPT ${CMAKE_SOURCE_DIR}/../scripts/mig)
set(CAP_DEFS "${CMAKE_SOURCE_DIR}/export/include/mach/cap_server.defs")
set(CAP_USER_C   "${CMAKE_BINARY_DIR}/generated/cap_server_user.c")
set(CAP_SERVER_C "${CMAKE_BINARY_DIR}/generated/cap_server_server.c")
set(CAP_USER_H   "${CMAKE_BINARY_DIR}/generated/cap_server.h")
set(CAP_SERVER_H "${CMAKE_BINARY_DIR}/generated/cap_server_server.h")

add_custom_command(
  OUTPUT ${CAP_USER_C} ${CAP_SERVER_C} ${CAP_USER_H} ${CAP_SERVER_H}
  COMMAND ${MIG_SCRIPT}
    -cpp ${CMAKE_C_COMPILER}
    -migcom $<TARGET_FILE:migcom>
    -I${CMAKE_SOURCE_DIR}/export/include
    -I${CMAKE_SOURCE_DIR}/src/mach_kernel
    -header ${CAP_USER_H}
    -sheader ${CAP_SERVER_H}
    -user ${CAP_USER_C}
    -server ${CAP_SERVER_C}
    ${CAP_DEFS}
  DEPENDS migcom ${CAP_DEFS}
  COMMENT "MIG: cap_server.defs (libcap)"
  VERBATIM
)

set_source_files_properties(
  ${CAP_USER_C} ${CAP_SERVER_C} ${CAP_USER_H} ${CAP_SERVER_H}
  PROPERTIES GENERATED TRUE
)

# Re-export the server-side artifacts so cap_server's CMakeLists can pick
# them up without re-running MIG.
set(CAP_SERVER_MIG_C ${CAP_SERVER_C} PARENT_SCOPE)
set(CAP_SERVER_MIG_H ${CAP_SERVER_H} PARENT_SCOPE)

add_library(libcap STATIC
  libcap.c
  sha256.c
  hmac_sha256.c
  ${CAP_USER_C}
)

add_dependencies(libcap mig_mach_public_headers)

target_compile_options(libcap PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -fno-builtin -w -mstack-protector-guard=global -include stdint.h -m32 -march=i686>
)

target_include_directories(libcap PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_BINARY_DIR}/generated/include
  ${CMAKE_SOURCE_DIR}/src/mach_services/include
  ${CMAKE_SOURCE_DIR}/export/include
  ${CMAKE_SOURCE_DIR}/src/mach_kernel
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(libcap PROPERTIES
  OUTPUT_NAME "cap"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/export/${EXPORT_PLATFORM}/${CMAKE_SYSTEM_PROCESSOR}/user/lib"
)
