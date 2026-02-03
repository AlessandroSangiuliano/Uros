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
  ${BISON_OUTPUT}
  ${FLEX_OUTPUT}
  ${CMAKE_CURRENT_BINARY_DIR}/ident.c
)

# ident.c: generated at build time to contain date/author/host information
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ident.c
  COMMAND /bin/sh -lc "printf 'char *MigGenerationDate = \"%s\";\\n' \"$(date)\" > ${CMAKE_CURRENT_BINARY_DIR}/ident.c && printf 'char *MigMoreData = \"%s@%s\";\\n' \"$(whoami)\" \"$(hostname)\" >> ${CMAKE_CURRENT_BINARY_DIR}/ident.c"
  COMMENT "Generating ident.c"
  VERBATIM
)

add_executable(migcom ${MIGCOM_SOURCES})

target_include_directories(migcom PRIVATE
  ${SRC_DIR}
  ${CMAKE_CURRENT_BINARY_DIR} # for parser.h
  ${CMAKE_SOURCE_DIR}/export/powermac/include
)

# link libfl or libl if available
find_library(LEX_LIB NAMES fl l)
if(LEX_LIB)
  target_link_libraries(migcom PRIVATE ${LEX_LIB})
else()
  message(WARNING "libfl/libl not found; if you get link errors, install flex or adjust linking")
endif()

# Suppress some warnings/errors produced by modern compilers on legacy code
set_target_properties(migcom PROPERTIES
  OUTPUT_NAME migcom
)

# Use an older C dialect and relax some warnings to support K&R-style code
target_compile_options(migcom PRIVATE
  -std=gnu89
  -Wno-implicit-function-declaration
  -Wno-old-style-definition
  -Wno-implicit-int
)

# Install into tools/<system>/hostbin similar to legacy layout
install(TARGETS migcom RUNTIME DESTINATION tools/${CMAKE_SYSTEM_NAME}/hostbin)


