cmake_minimum_required(VERSION 3.15)
project(rltex_tests LANGUAGES C)

# Provided by run.sh: RLTEX_BCDEC_DIR, RLTEX_ASTCENC_INCLUDE, RLTEX_ASTCENC_LIB
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/textures" rltex)

foreach(t test_core test_cache)
  add_executable(${t} ${t}.c)
  set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(${t} SYSTEM PRIVATE "${RLTEX_ASTCENC_INCLUDE}")
  target_link_libraries(${t} PRIVATE rltex)
endforeach()
