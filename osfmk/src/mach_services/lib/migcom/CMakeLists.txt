cmake_minimum_required(VERSION 3.16)

project(migcom_host C)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Require Flex and Bison (lexer/parser generation)
find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

# Bison/Flex outputs (placed in the build directory)
set(BISON_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/parser.c)
set(BISON_HEADER ${CMAKE_CURRENT_BINARY_DIR}/parser.h)
set(FLEX_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lexxer.c)

BISON_TARGET(Parser ${SRC_DIR}/parser.y ${BISON_OUTPUT} DEFINES_FILE ${BISON_HEADER} COMPILE_FLAGS -d)
FLEX_TARGET(Lexer ${SRC_DIR}/lexxer.l ${FLEX_OUTPUT})
ADD_FLEX_BISON_DEPENDENCY(Lexer Parser)

set(MIGCOM_SOURCES
  ${SRC_DIR}/error.c
  ${SRC_DIR}/global.c
  ${SRC_DIR}/header.c
  ${SRC_DIR}/mig.c
  ${SRC_DIR}/routine.c
  ${SRC_DIR}/server.c
  ${SRC_DIR}/statement.c
  ${SRC_DIR}/string.c
  ${SRC_DIR}/type.c
  ${SRC_DIR}/user.c
  ${SRC_DIR}/utils.c
  ${SRC_DIR}/utils_check.c
  ${SRC_DIR}/safestr.c
  ${SRC_DIR}/fwrite_hook.c
  ${BISON_OUTPUT}
  ${FLEX_OUTPUT}
  ${CMAKE_CURRENT_BINARY_DIR}/ident.c
)

# ident.c: generated at build time to contain date/author/host information
# Use configure_file + file(GENERATE ...) approach to avoid Ninja $-escape issues
set(IDENT_GEN_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/gen_ident.sh")
file(WRITE ${IDENT_GEN_SCRIPT}
"#!/bin/sh
printf 'char *MigGenerationDate = \"%s\";\\n' \"\$(date)\" > \"$1\"
printf 'char *MigMoreData = \"%s@%s\";\\n' \"\$(whoami)\" \"\$(hostname)\" >> \"$1\"
")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ident.c
  COMMAND /bin/sh ${IDENT_GEN_SCRIPT} ${CMAKE_CURRENT_BINARY_DIR}/ident.c
  DEPENDS ${IDENT_GEN_SCRIPT}
  COMMENT "Generating ident.c"
)

add_executable(migcom ${MIGCOM_SOURCES})

target_include_directories(migcom PRIVATE
  ${SRC_DIR}
  ${CMAKE_CURRENT_BINARY_DIR} # for parser.h
  ${CMAKE_SOURCE_DIR}/export/powermac/include
  # For mach/ndr.h and other kernel headers
  ${CMAKE_SOURCE_DIR}/../../mach_kernel
  /home/slex/Scrivania/osfmk-mklinux/osfmk/export/powermac/include
)

# link libfl or libl if available
find_library(LEX_LIB NAMES fl l)
if(LEX_LIB)
  target_link_libraries(migcom PRIVATE ${LEX_LIB})
else()
  message(WARNING "libfl/libl not found; if you get link errors, install flex or adjust linking")
endif()

# Link libdl for in-process fwrite hook (dlsym)
find_library(DL_LIB NAMES dl)
if(DL_LIB)
  target_link_libraries(migcom PRIVATE ${DL_LIB})
else()
  message(WARNING "libdl not found; in-process fwrite diagnostics disabled")
endif()

# Suppress some warnings/errors produced by modern compilers on legacy code
set_target_properties(migcom PROPERTIES
  OUTPUT_NAME migcom
)

# Use an older C dialect and relax some warnings to support K&R-style code
# Also add ASAN to detect memory corruption bugs in this legacy code
target_compile_options(migcom PRIVATE
  -std=gnu89
  -Wno-implicit-function-declaration
  -Wno-old-style-definition
  -Wno-implicit-int
  -fsanitize=address
  -g
)

target_link_options(migcom PRIVATE
  -fsanitize=address
)

# Install into tools/<system>/hostbin similar to legacy layout
install(TARGETS migcom RUNTIME DESTINATION tools/${CMAKE_SYSTEM_NAME}/hostbin)

# Add a minimal CTest to exercise the built migcom binary with a tiny input
# Note: We set LSAN_OPTIONS to suppress benign memory leaks in this one-shot tool
enable_testing()
set(LSAN_SUPPRESS "LSAN_OPTIONS=detect_leaks=0")

add_test(NAME migcom-minimal
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/minimal.mig"
)

# Additional integration tests exercising parsing and flags
add_test(NAME migcom-comments
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/comments.mig"
)

add_test(NAME migcom-skip-only
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/skip-only.mig"
)

# Exercise command-line flags (-k and -K)
add_test(NAME migcom-flag-k
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -k -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/minimal.mig"
)

add_test(NAME migcom-flag-K
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -K -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/minimal.mig"
)

# Advanced tests using a real .defs file and file generation checks
add_test(NAME migcom-device-request
  COMMAND sh -c "${LSAN_SUPPRESS} cpp -P -I ${CMAKE_SOURCE_DIR}/export/powermac/include ${CMAKE_SOURCE_DIR}/export/powermac/include/device/device_request.defs | ${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null"
)

add_test(NAME migcom-generate-user-file
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user ${CMAKE_CURRENT_BINARY_DIR}/generated_user.c -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/device_request-small.mig && grep -q 'foo' ${CMAKE_CURRENT_BINARY_DIR}/generated_user.c"
)

add_test(NAME migcom-generate-individual-files
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -i test_ -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/device_request-small.mig && test -f ${CMAKE_CURRENT_BINARY_DIR}/test_foo.c"
)

# Advanced self-contained case (no imports needed)
add_test(NAME migcom-advanced-self-contained
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user ${CMAKE_CURRENT_BINARY_DIR}/adv_user.c -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/advanced-self-contained.mig"
)

# ASCII validation test for generated_user.c
add_executable(test_ascii_generated_user tests/test_ascii_generated_user.c)
add_test(NAME ascii-check-generated-user
  COMMAND sh -c "${LSAN_SUPPRESS} $<TARGET_FILE:migcom> -V -user ${CMAKE_CURRENT_BINARY_DIR}/generated_user.ascii_check.c -server /dev/null -sheader /dev/null -dheader /dev/null < ${CMAKE_CURRENT_SOURCE_DIR}/tests/device_request-small.mig && ${CMAKE_CURRENT_BINARY_DIR}/test_ascii_generated_user ${CMAKE_CURRENT_BINARY_DIR}/generated_user.ascii_check.c"
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Test for SafeSnprintf behavior
add_executable(test_safestr tests/test_safestr.c ${SRC_DIR}/safestr.c)
target_include_directories(test_safestr PRIVATE ${SRC_DIR})
add_test(NAME safestr-basic
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/test_safestr
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
# This test expects the binary to exit with code 2 on truncation
# CTest treats non-zero exit as failure, so we mark it as expected to fail
add_test(NAME safestr-trunc
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/test_safestr trunc
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(safestr-trunc PROPERTIES WILL_FAIL TRUE)

