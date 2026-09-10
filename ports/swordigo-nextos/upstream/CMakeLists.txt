cmake_minimum_required(VERSION 3.10)
project(swordigo C)

set(CMAKE_C_STANDARD 11)
option(USE_GLES2 "unused — Swordigo is GLES1" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(OPENAL REQUIRED openal)
pkg_check_modules(MPG123 REQUIRED libmpg123)

add_executable(swordigo-nextos
  src/main.c
  src/contract.c
  src/crash_diag.c
  src/glfix.c
  src/gl_latebind.c
  src/gl_provider_policy.c
  src/imports.c
  src/jni_fake.c
  src/music_player.c
  src/util.c
  src/error.c
  src/so_util.c
  src/pthread_bridge.c
)

target_include_directories(swordigo-nextos PRIVATE
  ${SDL2_INCLUDE_DIRS}
  ${OPENAL_INCLUDE_DIRS}
  ${MPG123_INCLUDE_DIRS}
  src
)

target_link_libraries(swordigo-nextos PRIVATE
  ${SDL2_LIBRARIES}
  ${OPENAL_LIBRARIES}
  ${MPG123_LIBRARIES}
  z
  m
  dl
  pthread
)

target_compile_options(swordigo-nextos PRIVATE
  ${SDL2_CFLAGS_OTHER}
  -Wall -Wextra -Wno-unused-parameter
)
