# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxgl VERSION 0.3.5 LANGUAGES C)

include(GNUInstallDirs)

option(NXGL_BUILD_TESTS "Build host-side nxgl tests" ON)
option(NXGL_BUILD_NATIVE_TESTS
       "Build tests that link the native SDL/GL implementation" OFF)
option(NXGL_ENABLE_SANITIZERS "Build tests with Address/Undefined sanitizers" OFF)
option(NXGL_WITH_NXCOMPAT "Build the optional nxcompat reporting adapter" OFF)
option(NXGL_WITH_NXLOADER_PROVIDER
       "Build the opt-in nxloader 0.7.2 graphics evidence provider" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_library(nxgl STATIC
  src/nxgl_arbiter.c
  src/nxgl_config_request.c
  src/nxgl_display.c
  src/nxgl_diagnostics.c
  src/nxgl_logic.c
  src/nxgl_metrics.c
  src/nxgl_present.c
  src/nxgl_quality.c
  src/nxgl_graphics_contract.c
  src/nxgl_graphics_present_gate.c
  src/nxgl_single_channel.c
  src/nxgl_retry_contract.c
  src/nxgl_sdl_hint.c
  src/nxgl_sdl2.c
)
add_library(nxgl::nxgl ALIAS nxgl)
set_target_properties(nxgl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxgl
  PUBLIC include
  PRIVATE src
)
target_link_libraries(nxgl PUBLIC PkgConfig::SDL2 ${CMAKE_DL_LIBS})
target_compile_features(nxgl PUBLIC c_std_99)
target_compile_definitions(nxgl PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxgl PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)
if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nxgl PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# GLES1 fixed-function resolvido em tempo de execucao. Fica em arquivo separado
# porque nao depende de SDL e porque nem todo port usa GLES1: quem linka o nxgl
# core nao deve herdar dlopen de provedor grafico sem pedir. O header GLES1 do
# sistema nem sempre existe no host de build, e os ports compilam este .c direto
# pelo seu build_universal.sh com o sysroot do device, entao o alvo e' opcional.
find_path(NXGL_GLES1_HEADER_DIR GLES/gl.h)
if(NXGL_GLES1_HEADER_DIR)
add_library(nxgl-gles1 STATIC src/nxgl_gles1.c)
add_library(nxgl::gles1 ALIAS nxgl-gles1)
set_target_properties(nxgl-gles1 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxgl-gles1
  PUBLIC include "${NXGL_GLES1_HEADER_DIR}"
  PRIVATE src
)
target_link_libraries(nxgl-gles1 PUBLIC ${CMAKE_DL_LIBS})
target_compile_features(nxgl-gles1 PUBLIC c_std_99)
target_compile_definitions(nxgl-gles1 PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxgl-gles1 PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)
else()
  message(STATUS "nxgl: GLES/gl.h ausente; alvo nxgl-gles1 nao sera construido")
endif()

# V4-GRAPHICS-03. Pure policy over injected dl ops, but it belongs to the
# guest-binding path, not to every nxgl user, so it stays a separate archive.
add_library(nxgl-egl-binding STATIC src/nxgl_egl_binding.c)
add_library(nxgl::egl-binding ALIAS nxgl-egl-binding)
set_target_properties(nxgl-egl-binding PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxgl-egl-binding PUBLIC include PRIVATE src)
target_compile_features(nxgl-egl-binding PUBLIC c_std_99)
target_compile_definitions(nxgl-egl-binding PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxgl-egl-binding PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)

# Effectful provider repair is a separate archive so ordinary nxgl users do
# not acquire dlopen/environment/re-exec code merely by linking the core.
# The public API is also runtime opt-in and initializes disabled.
add_library(nxgl-provider-recovery STATIC src/nxgl_provider_recovery.c)
add_library(nxgl::provider-recovery ALIAS nxgl-provider-recovery)
set_target_properties(nxgl-provider-recovery PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxgl-provider-recovery
  PUBLIC include
  PRIVATE src
)
target_link_libraries(nxgl-provider-recovery PUBLIC nxgl ${CMAKE_DL_LIBS})
target_compile_features(nxgl-provider-recovery PUBLIC c_std_99)
target_compile_definitions(nxgl-provider-recovery PRIVATE
  _POSIX_C_SOURCE=200809L)
target_compile_options(nxgl-provider-recovery PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)
if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nxgl-provider-recovery PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# Runtime interception is deliberately a separate, default-off archive. Merely
# linking nxgl never mutates an nxloader registry or wraps GL entry points.
if(NXGL_WITH_NXLOADER_PROVIDER)
  if(NOT TARGET nxloader)
    set(NXLOADER_BUILD_TESTS OFF CACHE BOOL "")
    set(NXLOADER_BUILD_TOOLS OFF CACHE BOOL "")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../nxloader"
                     "${CMAKE_CURRENT_BINARY_DIR}/nxloader" EXCLUDE_FROM_ALL)
  endif()
  find_package(Threads REQUIRED)
  add_library(nxgl-nxloader-provider STATIC
    adapters/nxgl_graphics_contract_adapter.c
    adapters/nxgl_nxloader_provider.c)
  add_library(nxgl::nxloader-provider ALIAS nxgl-nxloader-provider)
  set_target_properties(nxgl-nxloader-provider PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxgl-nxloader-provider
    PUBLIC include
    PRIVATE adapters)
  target_link_libraries(nxgl-nxloader-provider
    PUBLIC nxgl nxloader Threads::Threads ${CMAKE_DL_LIBS})
  target_compile_features(nxgl-nxloader-provider PUBLIC c_std_99)
  target_compile_definitions(nxgl-nxloader-provider PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(nxgl-nxloader-provider PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual)
  install(TARGETS nxgl-nxloader-provider
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxgl_nxloader_provider.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(TARGETS nxgl nxgl-provider-recovery
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxgl.h include/nxgl_provider_recovery.h
        include/nxgl_config_request.h include/nxgl_retry_contract.h
        include/nxgl_quality.h
        include/nxgl_graphics_contract.h
        include/nxgl_graphics_present_gate.h
        include/nxgl_single_channel.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES adapters/nxgl_frame_proof_adapter.c
              adapters/nxgl_frame_proof_adapter.h
              engine-glue/nxgl_godot_frame_proof.h
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nxgl/engine-glue)
install(FILES docs/GODOT-FRAME-PROOF.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nxgl)

if(NXGL_WITH_NXCOMPAT)
  if(NOT TARGET nxcompat)
    set(NXCOMPAT_BUILD_TESTS OFF CACHE BOOL "")
    set(NXCOMPAT_BUILD_TOOLS OFF CACHE BOOL "")
    set(NXCOMPAT_WITH_SDL2 OFF CACHE BOOL "")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../nxcompat"
                     "${CMAKE_CURRENT_BINARY_DIR}/nxcompat" EXCLUDE_FROM_ALL)
  endif()
  add_library(nxgl-nxcompat STATIC src/nxgl_nxcompat.c)
  add_library(nxgl::nxcompat ALIAS nxgl-nxcompat)
  set_target_properties(nxgl-nxcompat PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxgl-nxcompat PUBLIC include)
  target_link_libraries(nxgl-nxcompat PUBLIC nxgl nxcompat)
  target_compile_features(nxgl-nxcompat PUBLIC c_std_99)
  target_compile_options(nxgl-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxgl-nxcompat PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endif()

if(NXGL_BUILD_TESTS)
  enable_testing()
  add_executable(test-nxgl-godot-frame-proof
    tests/test_godot_frame_proof.c)
  target_include_directories(test-nxgl-godot-frame-proof PRIVATE
    adapters engine-glue)
  target_compile_options(test-nxgl-godot-frame-proof PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxgl-godot-frame-proof
    COMMAND test-nxgl-godot-frame-proof)
  add_library(nxgl-test-environment STATIC
    src/nxgl_arbiter.c
    src/nxgl_sdl2.c)
  target_include_directories(nxgl-test-environment PRIVATE
    include src ${SDL2_INCLUDE_DIRS})
  target_compile_features(nxgl-test-environment PRIVATE c_std_99)
  target_compile_definitions(nxgl-test-environment PRIVATE
    _POSIX_C_SOURCE=200809L NXGL_CORE_TESTING=1)
  target_compile_options(nxgl-test-environment PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  add_executable(test-nxgl-environment tests/test_nxgl_environment.c)
  target_include_directories(test-nxgl-environment PRIVATE
    include src ${SDL2_INCLUDE_DIRS})
  target_compile_features(test-nxgl-environment PRIVATE c_std_99)
  target_compile_definitions(test-nxgl-environment PRIVATE
    _POSIX_C_SOURCE=200809L NXGL_CORE_TESTING=1)
  target_compile_options(test-nxgl-environment PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  target_link_libraries(test-nxgl-environment PRIVATE nxgl-test-environment)
  if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxgl-test-environment PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(test-nxgl-environment PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(test-nxgl-environment PRIVATE
      -fsanitize=address,undefined)
  endif()
  add_test(NAME nxgl-environment COMMAND test-nxgl-environment)

  # V4 pure gates. They link nothing but the module under test.
  add_executable(test-nxgl-v4-display tests/test_v4_display.c
                 src/nxgl_display.c)
  target_include_directories(test-nxgl-v4-display PRIVATE include)
  add_test(NAME nxgl-v4-display COMMAND test-nxgl-v4-display)
  add_executable(test-nxgl-v4-egl-binding tests/test_v4_egl_binding.c
                 src/nxgl_egl_binding.c)
  target_include_directories(test-nxgl-v4-egl-binding PRIVATE include)
  add_test(NAME nxgl-v4-egl-binding COMMAND test-nxgl-v4-egl-binding)
  # V4-GRAPHICS-04: the pure post-first-present state machine.
  add_executable(test-nxgl-v4-present-gate tests/test_v4_graphics_present_gate.c
                 src/nxgl_graphics_present_gate.c
                 src/nxgl_graphics_contract.c)
  target_include_directories(test-nxgl-v4-present-gate PRIVATE include)
  add_test(NAME nxgl-v4-present-gate COMMAND test-nxgl-v4-present-gate)
  # V4-GRAPHICS-04: the lifecycle adapter against an injected fake SDL/GL.
  find_package(Threads REQUIRED)
  add_executable(test-nxgl-v4-lifecycle-adapter
                 tests/test_v4_graphics_lifecycle_adapter.c
                 adapters/nxgl_graphics_contract_adapter.c
                 src/nxgl_graphics_present_gate.c
                 src/nxgl_graphics_contract.c)
  target_include_directories(test-nxgl-v4-lifecycle-adapter PRIVATE
    include adapters)
  target_compile_definitions(test-nxgl-v4-lifecycle-adapter PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_link_libraries(test-nxgl-v4-lifecycle-adapter PRIVATE
    Threads::Threads ${CMAKE_DL_LIBS})
  add_test(NAME nxgl-v4-lifecycle-adapter COMMAND test-nxgl-v4-lifecycle-adapter)

  # M13 gates compile the production transactions against injected providers.
  # They intentionally consume only SDL headers: linking SDL/EGL/GLES/GL here
  # would turn a pure policy test into a possible display/device test.
  function(nxgl_configure_m13_test target)
    target_include_directories(${target} PRIVATE
      include src ${SDL2_INCLUDE_DIRS})
    target_compile_features(${target} PRIVATE c_std_99)
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L
      ${ARGN})
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endfunction()

  add_executable(test-nxgl-open-v2
    src/nxgl_arbiter.c
    src/nxgl_logic.c
    src/nxgl_sdl2.c
    tests/test_nxgl_open_v2.c
  )
  nxgl_configure_m13_test(test-nxgl-open-v2 NXGL_M13_TESTING=1)
  add_test(NAME nxgl-m13-open-v2 COMMAND test-nxgl-open-v2)

  add_executable(test-nxgl-sdl-hint
    src/nxgl_arbiter.c
    src/nxgl_sdl_hint.c
    tests/test_nxgl_sdl_hint.c
  )
  nxgl_configure_m13_test(test-nxgl-sdl-hint
    NXGL_SDL_HINT_TESTING=1)
  add_test(NAME nxgl-m13-sdl-hint COMMAND test-nxgl-sdl-hint)

  add_executable(test-nxgl-present-v2
    src/nxgl_present.c
    tests/test_nxgl_present_v2.c
  )
  nxgl_configure_m13_test(test-nxgl-present-v2
    NXGL_PRESENT_V2_TESTING=1)
  add_test(NAME nxgl-m13-present-v2 COMMAND test-nxgl-present-v2)

  add_executable(test-nxgl-metrics
    src/nxgl_metrics.c
    tests/test_nxgl_metrics.c
  )
  nxgl_configure_m13_test(test-nxgl-metrics)
  add_test(NAME nxgl-m13-metrics COMMAND test-nxgl-metrics)

  add_executable(test-nxgl-diagnostics
    src/nxgl_diagnostics.c
    tests/test_nxgl_diagnostics.c
  )
  nxgl_configure_m13_test(test-nxgl-diagnostics)
  add_test(NAME nxgl-m13-diagnostics COMMAND test-nxgl-diagnostics)

  add_library(nxgl-test-provider-good SHARED
    tests/fake_provider_good.c)
  set_target_properties(nxgl-test-provider-good PROPERTIES
    OUTPUT_NAME "nxgl-test-provider-good")
  nxgl_configure_m13_test(nxgl-test-provider-good)

  add_library(nxgl-test-provider-dependency SHARED
    tests/fake_provider_dependency.c)
  set_target_properties(nxgl-test-provider-dependency PROPERTIES
    OUTPUT_NAME "nxgl-test-provider-dependency")
  nxgl_configure_m13_test(nxgl-test-provider-dependency)

  add_library(nxgl-test-provider-mixed SHARED
    tests/fake_provider_mixed.c)
  set_target_properties(nxgl-test-provider-mixed PROPERTIES
    OUTPUT_NAME "nxgl-test-provider-mixed"
    BUILD_RPATH "$ORIGIN")
  nxgl_configure_m13_test(nxgl-test-provider-mixed)
  target_link_libraries(nxgl-test-provider-mixed PRIVATE
    nxgl-test-provider-dependency)

  add_library(nxgl-provider-recovery-test STATIC
    src/nxgl_arbiter.c
    src/nxgl_diagnostics.c
    src/nxgl_provider_recovery.c)
  nxgl_configure_m13_test(nxgl-provider-recovery-test
    NXGL_PROVIDER_RECOVERY_TESTING=1)
  target_link_libraries(nxgl-provider-recovery-test PUBLIC ${CMAKE_DL_LIBS})

  add_executable(test-nxgl-provider-recovery
    tests/test_nxgl_provider_recovery.c)
  nxgl_configure_m13_test(test-nxgl-provider-recovery)
  target_compile_definitions(test-nxgl-provider-recovery PRIVATE
    NXGL_PROVIDER_RECOVERY_TESTING=1)
  target_link_libraries(test-nxgl-provider-recovery PRIVATE
    nxgl-provider-recovery-test ${CMAKE_DL_LIBS})
  add_test(NAME nxgl-m13-provider-recovery
    COMMAND test-nxgl-provider-recovery
      $<TARGET_FILE:nxgl-test-provider-good>
      $<TARGET_FILE:nxgl-test-provider-mixed>
      $<TARGET_FILE:nxgl-test-provider-dependency>)

  if(NXGL_BUILD_NATIVE_TESTS)
    add_executable(test-nxgl tests/test_nxgl.c)
    target_include_directories(test-nxgl PRIVATE src)
    target_link_libraries(test-nxgl PRIVATE nxgl)
    target_compile_definitions(test-nxgl PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test-nxgl PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(test-nxgl PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(test-nxgl PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME nxgl-policy COMMAND test-nxgl)
  endif()

  if(NXGL_WITH_NXCOMPAT)
    add_executable(test-nxgl-nxcompat tests/test_nxgl_nxcompat.c)
    target_link_libraries(test-nxgl-nxcompat PRIVATE nxgl-nxcompat)
    target_compile_options(test-nxgl-nxcompat PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(test-nxgl-nxcompat PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(test-nxgl-nxcompat PRIVATE
        -fsanitize=address,undefined)
    endif()
    add_test(NAME nxgl-nxcompat COMMAND test-nxgl-nxcompat)
  endif()
endif()

if(NXGL_WITH_NXCOMPAT)
  install(TARGETS nxgl-nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxgl_nxcompat.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
