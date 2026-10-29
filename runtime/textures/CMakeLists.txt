# rltex: hybrid BC -> ASTC texture pipeline (PROJECT_SPEC.md 6.5). Portable C11.
#
# Inputs (all upstream sources stay read-only; build out of tree):
#   RLTEX_BCDEC_DIR       directory containing bcdec.h            (external/bcdec)
#   RLTEX_ASTCENC_INCLUDE directory containing astcenc.h          (external/astc-encoder/Source)
#   RLTEX_ASTCENC_LIB     built libastcenc-*-static.a (optional; without it the ASTC encoder
#                         and the background queue compile to stubs returning UNSUPPORTED)
cmake_minimum_required(VERSION 3.15)
if(NOT PROJECT_NAME)
  project(rltex LANGUAGES C)
endif()

set(RLTEX_BCDEC_DIR "" CACHE PATH "directory containing bcdec.h")
set(RLTEX_ASTCENC_INCLUDE "" CACHE PATH "directory containing astcenc.h")
set(RLTEX_ASTCENC_LIB "" CACHE FILEPATH "astc-encoder static library")

if(NOT EXISTS "${RLTEX_BCDEC_DIR}/bcdec.h")
  message(FATAL_ERROR "RLTEX_BCDEC_DIR must contain bcdec.h (got '${RLTEX_BCDEC_DIR}')")
endif()

find_package(Threads REQUIRED)

add_library(rltex STATIC
  src/sha256.c src/format.c src/bc_decode.c src/config.c src/fs.c
  src/astc_encode.c src/ctx.c src/platform.c)
set_target_properties(rltex PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(rltex PUBLIC include)
target_include_directories(rltex PRIVATE src)
# Third-party headers are -isystem so that -Werror applies to our code only.
target_include_directories(rltex SYSTEM PRIVATE "${RLTEX_BCDEC_DIR}")
target_compile_options(rltex PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rltex PUBLIC Threads::Threads)

if(RLTEX_ASTCENC_LIB)
  target_compile_definitions(rltex PUBLIC RLTEX_WITH_ASTCENC)
  target_include_directories(rltex SYSTEM PRIVATE "${RLTEX_ASTCENC_INCLUDE}")
  if(CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS")
    target_link_libraries(rltex PUBLIC "${RLTEX_ASTCENC_LIB}" c++)
  else()
    target_link_libraries(rltex PUBLIC "${RLTEX_ASTCENC_LIB}" stdc++ m)
  endif()
endif()

add_executable(rltex_cli tools/rltex_cli.c)
set_target_properties(rltex_cli PROPERTIES OUTPUT_NAME rltex C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(rltex_cli PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rltex_cli PRIVATE rltex)
