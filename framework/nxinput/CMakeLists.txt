# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxinput VERSION 0.11.8 LANGUAGES C)

option(NXINPUT_BUILD_TESTS "Build deterministic host-side nxinput tests" ON)
option(NXINPUT_BUILD_NATIVE_TESTS
       "Build tests that enumerate the native SDL controller subsystem" OFF)
option(NXINPUT_WITH_NXCOMPAT "Build the optional nxcompat receipt bridge" OFF)
option(NXINPUT_WITH_SDL3_PORTMASTER
       "Build the opt-in SDL3/PortMaster mapping coordinator" OFF)

find_package(SDL2 QUIET CONFIG)
if(TARGET SDL2::SDL2)
  set(NXINPUT_SDL_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
  set(NXINPUT_SDL_TARGET SDL2::SDL2-static)
elseif(TARGET SDL2)
  set(NXINPUT_SDL_TARGET SDL2)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(NXINPUT_SDL_TARGET PkgConfig::SDL2)
endif()

# V4-CONTROLLERS-03 / C3 (mission 114A): the sovereign resolver is part of
# the production library, not an optional sibling. nxinput.c cannot apply a
# mapping without it.
# V4-CONTROLLERS-03 / C6: nxinput_sdl_seam is production code, not a test
# helper -- it is the function that is compiled INTO a port's pinned SDL2 or
# SDL3 and decides, before SDL announces a pad, whether the pad may be
# announced at all. It ships in the library so a port links one artefact,
# and it carries nxinput_sdl (the ordinal domains) with it. Neither file
# includes an SDL header: the SDL calls arrive through the ops table that the
# per-major glue in engine-glue/ fills in.
add_library(nxinput STATIC
  src/nxinput.c
  src/nxinput_core.c
  src/nxinput_authority.c
  src/nxinput_authority_sdl.c
  src/nxinput_sovereign.c
  src/nxinput_sdl.c
  src/nxinput_portmaster.c
  src/nxinput_sdl_seam.c
  src/nxinput_livedb.c
  src/nxinput_provider.c
  src/nxinput_provider_linux.c
  src/nxinput_sha256.c
  src/nxinput_translate.c
  src/nxinput_axis_calib.c
  src/nxinput_gptk4.c
  src/nxinput_route.c
  src/nxinput_registry.c
  src/nxinput_decision.c
  src/nxinput_corpus.c
  src/nxinput_prerouter.c
  src/nxinput_lifecycle.c
  src/nxinput_gptk4_bridge.c
  src/nxinput_gptk4_preinit.c
  src/nxinput_authority_v5.c
  src/nxinput_keyboard.c
  src/nxinput_route_policy.c
  src/nxinput_coexist.c
)
target_include_directories(nxinput
  PUBLIC include
  PRIVATE src
)
target_compile_features(nxinput PUBLIC c_std_99)
target_compile_options(nxinput PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
target_link_libraries(nxinput PUBLIC ${NXINPUT_SDL_TARGET} m)

# V3-CONTROLLERS-01: the NEXTOSCONTROLLERS.gptk parser, dispatcher and
# cursor/camera kinematics are SDL-free by design and ship as their own
# installed static library, so an adapter can link them without pulling SDL.
add_library(nxinput-gptk STATIC
  src/nxinput_godot.c
  src/nxinput_godot_consumer.c
  src/nxinput_godot_seam.c
  # 0.11.1 (C6): the Godot native seam translates ascending CFW lines through
  # the physical codes; both files are pure (no SDL) and also live in nxinput.
  src/nxinput_translate.c
  src/nxinput_sdl.c
  src/nxinput_gptk.c
  src/nxinput_gptk_live.c
  src/nxinput_gptk_loader.c
  src/nxinput_gptk_motion.c
  src/nxinput_gptk_upgrade.c
  src/nxinput_gptk_preinit.c
  src/nxinput_exit_chord.c
)
add_library(nxinput::gptk ALIAS nxinput-gptk)
set_target_properties(nxinput-gptk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxinput-gptk PUBLIC include)
target_compile_features(nxinput-gptk PUBLIC c_std_99)
# nxinput_gptk_upgrade.c uses openat/fstatat/unlinkat/renameat and O_CLOEXEC/
# O_NOFOLLOW: POSIX.1-2008, invisible under a strict -std=c99/-std=c11 build
# (the nxcompat host gate) unless the target asks for them like every other
# nxinput target does.
target_compile_definitions(nxinput-gptk PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxinput-gptk PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
target_link_libraries(nxinput-gptk PUBLIC m)

# Framework V4 opt-in: a private SDL3 runtime can receive a PortMaster mapping
# authored in SDL2/joydev ordinal order. This target stays separate so no V3
# consumer gains an SDL3 dependency or behavior change implicitly.
if(NXINPUT_WITH_SDL3_PORTMASTER)
  find_package(SDL3 QUIET CONFIG)
  if(TARGET SDL3::SDL3)
    set(NXINPUT_SDL3_TARGET SDL3::SDL3)
  elseif(TARGET SDL3::SDL3-static)
    set(NXINPUT_SDL3_TARGET SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3-shared)
    set(NXINPUT_SDL3_TARGET SDL3::SDL3-shared)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    set(NXINPUT_SDL3_TARGET PkgConfig::SDL3)
  endif()
  find_package(Threads REQUIRED)
  add_library(nxinput-sdl3-portmaster STATIC
    src/nxinput_sdl3_portmaster.c
  )
  add_library(nxinput::sdl3-portmaster ALIAS nxinput-sdl3-portmaster)
  set_target_properties(nxinput-sdl3-portmaster PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxinput-sdl3-portmaster PUBLIC include)
  target_compile_features(nxinput-sdl3-portmaster PUBLIC c_std_11)
  target_compile_options(nxinput-sdl3-portmaster PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  target_link_libraries(nxinput-sdl3-portmaster
    PUBLIC ${NXINPUT_SDL3_TARGET} Threads::Threads)
endif()

if(NXINPUT_WITH_NXCOMPAT)
  if(NOT TARGET nxcompat)
    set(NXCOMPAT_BUILD_TESTS OFF CACHE BOOL "")
    set(NXCOMPAT_BUILD_TOOLS OFF CACHE BOOL "")
    set(NXCOMPAT_WITH_SDL2 OFF CACHE BOOL "")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../nxcompat"
                     "${CMAKE_CURRENT_BINARY_DIR}/nxcompat" EXCLUDE_FROM_ALL)
  endif()
  add_library(nxinput-nxcompat STATIC src/nxinput_nxcompat.c)
  add_library(nxinput::nxcompat ALIAS nxinput-nxcompat)
  set_target_properties(nxinput-nxcompat PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxinput-nxcompat PUBLIC include)
  target_link_libraries(nxinput-nxcompat PUBLIC nxinput nxcompat)
  target_compile_features(nxinput-nxcompat PUBLIC c_std_99)
  target_compile_options(nxinput-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

# V4-CONTROLLERS-03 / C2: pure input observability. Separate archive so a
# port only links it by explicit opt-in; a NULL sink keeps every call a no-op.
add_library(nxinput-observe STATIC src/nxinput_observe.c)
add_library(nxinput::observe ALIAS nxinput-observe)
set_target_properties(nxinput-observe PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxinput-observe PUBLIC include)
target_compile_features(nxinput-observe PUBLIC c_std_99)
target_compile_definitions(nxinput-observe PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxinput-observe PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)

# V4-CONTROLLERS-03 / C3: the sovereign mapping authority order. Pure and a
# separate archive; the PortMaster/CFW semantic mapping stays sovereign.
add_library(nxinput-sovereign STATIC src/nxinput_sovereign.c)
add_library(nxinput::sovereign ALIAS nxinput-sovereign)
set_target_properties(nxinput-sovereign PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxinput-sovereign PUBLIC include)
target_compile_features(nxinput-sovereign PUBLIC c_std_99)
target_compile_definitions(nxinput-sovereign PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxinput-sovereign PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)

if(NXINPUT_BUILD_TESTS)
  enable_testing()
  add_executable(test-nxinput-godot-runtime
    tests/test_godot_runtime_policy.c)
  target_include_directories(test-nxinput-godot-runtime PRIVATE include)
  target_compile_options(test-nxinput-godot-runtime PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-godot-runtime COMMAND test-nxinput-godot-runtime)
  add_test(NAME nxinput-godot-runtime-template
    COMMAND python3 -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_godot_runtime_template.py)
  # C3 sovereign authority order (hermetic core gates).
  add_executable(test-nxinput-sovereign tests/test_sovereign.c)
  target_include_directories(test-nxinput-sovereign PRIVATE include)
  target_link_libraries(test-nxinput-sovereign PRIVATE nxinput-sovereign)
  target_compile_definitions(test-nxinput-sovereign PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-sovereign PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-sovereign COMMAND test-nxinput-sovereign)
  # C3/114A: the PRODUCTION mapping adapter (hotplug, two pads with one GUID,
  # hostile setter, refusal before gameplay) over a scripted runtime seam.
  add_executable(test-nxinput-authority tests/test_authority.c
                 src/nxinput_authority.c src/nxinput_sovereign.c)
  target_include_directories(test-nxinput-authority PRIVATE include)
  target_compile_definitions(test-nxinput-authority PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-authority PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-authority COMMAND test-nxinput-authority)
  add_test(NAME nxinput-sovereign-corpus
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-sovereign-corpus-host.sh)
  set_tests_properties(nxinput-sovereign-corpus PROPERTIES
    SKIP_RETURN_CODE 77)
  add_executable(sovereign-corpus-driver tests/sovereign_corpus_driver.c)
  target_include_directories(sovereign-corpus-driver PRIVATE include)
  target_link_libraries(sovereign-corpus-driver PRIVATE nxinput-sovereign)
  target_compile_definitions(sovereign-corpus-driver PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(sovereign-corpus-driver PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  # C2 observability: hermetic module gates + the read-only doctor.
  add_executable(test-nxinput-observe tests/test_observe.c)
  target_include_directories(test-nxinput-observe PRIVATE include)
  target_link_libraries(test-nxinput-observe PRIVATE nxinput-observe)
  target_compile_definitions(test-nxinput-observe PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-observe PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-observe COMMAND test-nxinput-observe)
  add_executable(nxinput-doctor tests/nxinput_doctor.c)
  target_compile_definitions(nxinput-doctor PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(nxinput-doctor PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  if(NXINPUT_BUILD_NATIVE_TESTS)
    add_executable(test-nxinput tests/test_nxinput.c)
    target_include_directories(test-nxinput PRIVATE src)
    target_link_libraries(test-nxinput PRIVATE nxinput)
    target_compile_definitions(test-nxinput PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test-nxinput PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxinput-core COMMAND test-nxinput)
  endif()
  # Pure host test: SDL button index -> evdev code derivation (no device).
  add_executable(test-nxinput-evdev-chord tests/test_evdev_chord_map.c)
  target_include_directories(test-nxinput-evdev-chord PRIVATE include)
  target_link_libraries(test-nxinput-evdev-chord PRIVATE ${NXINPUT_SDL_TARGET})
  target_compile_options(test-nxinput-evdev-chord PRIVATE -Wall -Wextra)
  add_test(NAME nxinput-evdev-chord COMMAND test-nxinput-evdev-chord)
  # V4-CONTROLLERS-03 / C4: NEXTOSCONTROLLERS v2 (tri-state + owner upgrade).
  add_executable(test-nxinput-gptk-v2 tests/test_gptk_v2.c)
  target_include_directories(test-nxinput-gptk-v2 PRIVATE include)
  target_link_libraries(test-nxinput-gptk-v2 PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-gptk-v2 PRIVATE
    _POSIX_C_SOURCE=200809L
    NXINPUT_V2_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/ok-v2-generated.gptk")
  target_compile_options(test-nxinput-gptk-v2 PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-gptk-v2 COMMAND test-nxinput-gptk-v2)
  # 0.10.0: NEXTOS_CONTROLLERS/3 (FACE_LAYOUT), sealed-oracle negatives.
  add_executable(test-nxinput-gptk-v3 tests/test_gptk_v3.c)
  target_include_directories(test-nxinput-gptk-v3 PRIVATE include)
  target_link_libraries(test-nxinput-gptk-v3 PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-gptk-v3 PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-gptk-v3 PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-gptk-v3 COMMAND test-nxinput-gptk-v3)
  # 0.10.0: the pre-init GPTK boundary (one read, layout for the declare).
  add_executable(test-nxinput-gptk-preinit tests/test_gptk_preinit.c)
  target_include_directories(test-nxinput-gptk-preinit PRIVATE include)
  target_link_libraries(test-nxinput-gptk-preinit PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-gptk-preinit PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-gptk-preinit PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-gptk-preinit COMMAND test-nxinput-gptk-preinit)
  # 0.10.0: bounded live-database wait, injected clock (never sleeps).
  add_executable(test-nxinput-livedb tests/test_livedb.c
    src/nxinput_livedb.c)
  target_include_directories(test-nxinput-livedb PRIVATE include)
  target_compile_definitions(test-nxinput-livedb PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-livedb PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-livedb COMMAND test-nxinput-livedb)
  # 0.10.0 / P7: CURSOR_DPAD_IF_NO_STICK, the pure half.
  add_executable(test-nxinput-cursor-dpad-p7 tests/test_cursor_dpad_p7.c
    src/nxinput_core.c)
  target_include_directories(test-nxinput-cursor-dpad-p7 PRIVATE include)
  target_link_libraries(test-nxinput-cursor-dpad-p7
    PRIVATE ${NXINPUT_SDL_TARGET} m)
  target_compile_definitions(test-nxinput-cursor-dpad-p7 PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-cursor-dpad-p7 PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-cursor-dpad-p7 COMMAND test-nxinput-cursor-dpad-p7)
  # V4-CONTROLLERS-03 / C5B: the Godot 3/4 mapping adapter and its closed
  # binding grammar. These are the FIXTURE_HOST gates only.
  #
  # tests/run-godot-c5b-host.sh is deliberately NOT registered here: it runs
  # real Godot processes and the pinned-source audit, and the 116A audit
  # refused a battery that ran a runner as a ctest case and then repeated it
  # in the same log. The single C5B battery invokes it exactly once.
  add_executable(test-nxinput-godot-mapping tests/test_godot_mapping.c)
  target_include_directories(test-nxinput-godot-mapping PRIVATE include)
  target_link_libraries(test-nxinput-godot-mapping PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-godot-mapping PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-godot-mapping PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-godot-mapping COMMAND test-nxinput-godot-mapping)
  add_executable(test-nxinput-godot-consumer tests/test_godot_consumer.c)
  target_include_directories(test-nxinput-godot-consumer PRIVATE include)
  target_link_libraries(test-nxinput-godot-consumer PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-godot-consumer PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-godot-consumer PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-godot-consumer COMMAND test-nxinput-godot-consumer)
  # V4-CONTROLLERS-03 / C5B. These are FIXTURE_HOST unit gates; the two real
  # engines are exercised only by tests/c5b/c5b_attempt.py, which is the
  # single final command of an attempt and is deliberately NOT registered
  # here, so no gate can run once through ctest and once outside it.
  add_executable(test-nxinput-godot-seam tests/test_godot_seam.c)
  target_include_directories(test-nxinput-godot-seam PRIVATE include)
  target_link_libraries(test-nxinput-godot-seam PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-godot-seam PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-godot-seam PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-godot-seam COMMAND test-nxinput-godot-seam)
  # V4-CONTROLLERS-03 / C6: the SDL seam's fail-closed ladder. FIXTURE_HOST --
  # it drives the same function the three pinned SDL libraries have linked in,
  # with a scripted runtime, so every refusal branch is reachable on purpose.
  # What the REAL libraries answered is tests/run-sdl-c6-host.sh.
  # Sources listed explicitly rather than linking `nxinput`: the seam is
  # SDL-free and this test must stay SDL-free too, or a green result would
  # depend on whichever SDL happened to be installed on the host.
  add_executable(test-nxinput-sdl-seam tests/test_sdl_seam.c
    src/nxinput_sdl_seam.c src/nxinput_sdl.c src/nxinput_portmaster.c
    src/nxinput_sovereign.c src/nxinput_authority.c src/nxinput_livedb.c
    src/nxinput_decision.c src/nxinput_provider.c src/nxinput_godot.c)
  target_include_directories(test-nxinput-sdl-seam PRIVATE include)
  target_compile_definitions(test-nxinput-sdl-seam PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-sdl-seam PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-sdl-seam COMMAND test-nxinput-sdl-seam)
  add_executable(test-nxinput-portmaster-domain
    tests/test_portmaster_domain.c src/nxinput_portmaster.c
    src/nxinput_sdl.c)
  target_include_directories(test-nxinput-portmaster-domain PRIVATE include)
  target_compile_definitions(test-nxinput-portmaster-domain PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-portmaster-domain PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-portmaster-domain
           COMMAND test-nxinput-portmaster-domain)
  add_executable(test-nxinput-godot-parser-negatives
    tests/test_godot_parser_negatives.c)
  target_include_directories(test-nxinput-godot-parser-negatives
    PRIVATE include)
  target_link_libraries(test-nxinput-godot-parser-negatives
    PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-godot-parser-negatives PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-godot-parser-negatives PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-godot-parser-negatives
           COMMAND test-nxinput-godot-parser-negatives)
  add_executable(test-nxinput-gptk-upgrade tests/test_gptk_upgrade.c)
  target_include_directories(test-nxinput-gptk-upgrade PRIVATE include)
  target_link_libraries(test-nxinput-gptk-upgrade PRIVATE nxinput-gptk)
  target_compile_definitions(test-nxinput-gptk-upgrade PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-gptk-upgrade PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes)
  add_test(NAME nxinput-gptk-upgrade COMMAND test-nxinput-gptk-upgrade)
  add_test(NAME nxinput-gptk-v2-host
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-gptk-v2-host.sh)
  set_tests_properties(nxinput-gptk-v2-host PROPERTIES SKIP_RETURN_CODE 77)
  # Nucleo puro do ordinal pad fix: sem SDL, sem device.
  add_executable(test-nxinput-pad-ordinal tests/pad_ordinal_probe.c)
  target_include_directories(test-nxinput-pad-ordinal PRIVATE include)
  target_compile_definitions(test-nxinput-pad-ordinal
                             PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(test-nxinput-pad-ordinal PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxinput-pad-ordinal
    COMMAND ${CMAKE_COMMAND} -E env python3 -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pad_ordinal_fix.py)
  add_executable(test-nxinput-sdl3-portmaster-mapping
    src/nxinput_sdl3_portmaster.c
    tests/test_sdl3_portmaster_mapping.c
  )
  target_include_directories(test-nxinput-sdl3-portmaster-mapping
    PRIVATE include)
  target_compile_definitions(test-nxinput-sdl3-portmaster-mapping
    PRIVATE NXINPUT_SDL3_PM_CORE_ONLY)
  target_compile_features(test-nxinput-sdl3-portmaster-mapping PUBLIC c_std_11)
  target_compile_options(test-nxinput-sdl3-portmaster-mapping PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxinput-sdl3-portmaster-mapping
    COMMAND test-nxinput-sdl3-portmaster-mapping)
  add_test(NAME nxinput-sdl3-portmaster-manager
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-sdl3-portmaster-host.sh)
  set_tests_properties(nxinput-sdl3-portmaster-manager PROPERTIES
    SKIP_RETURN_CODE 77)
  add_test(NAME nxinput-sdl3-discovery-contract
    COMMAND ${CMAKE_COMMAND} -E env python3 -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdl3_discovery_contract.py)
  add_test(NAME nxinput-static-gate
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/static_no_device_name_fallback.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
  if(NXINPUT_WITH_NXCOMPAT)
    add_executable(test-nxinput-nxcompat tests/test_nxinput_nxcompat.c)
    target_link_libraries(test-nxinput-nxcompat PRIVATE nxinput-nxcompat)
    target_compile_options(test-nxinput-nxcompat PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxinput-nxcompat COMMAND test-nxinput-nxcompat)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS nxinput ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS nxinput-gptk ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(NXINPUT_WITH_SDL3_PORTMASTER)
  install(TARGETS nxinput-sdl3-portmaster
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxinput_sdl3_portmaster.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
install(FILES include/nxinput_decision.h include/nxinput_corpus.h
        include/nxinput_prerouter.h include/nxinput_lifecycle.h include/nxinput_gptk4_bridge.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/nxinput.h include/nxinput_authority.h
        include/nxinput_livedb.h
        include/nxinput_gptk_preinit.h
        include/nxinput_evdev_chord.h
        include/nxinput_pad_ordinal_fix.h
        include/nxinput_gptk.h
        include/nxinput_gptk_live.h
        include/nxinput_gptk_loader.h
        include/nxinput_gptk_upgrade.h
        include/nxinput_godot.h
        include/nxinput_godot_consumer.h
        include/nxinput_godot_runtime.h
        include/nxinput_godot_seam.h
        include/nxinput_portmaster.h
        include/nxinput_gptk_motion.h
        include/nxinput_exit_chord.h
        include/nxinput_observe.h
        include/nxinput_sovereign.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES engine-glue/nxinput_gptk_godot4_glue.h
              engine-glue/nxinput_gptk_godot4_glue.cpp
              engine-glue/nxinput_padset.h
              engine-glue/nxinput_padset.c
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nxinput/engine-glue)
install(FILES docs/GODOT-RUNTIME-ADAPTER.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nxinput)
install(TARGETS nxinput-observe nxinput-sovereign
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(NXINPUT_WITH_NXCOMPAT)
  install(TARGETS nxinput-nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxinput_nxcompat.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()


# ---------------------------------------------------------------- V5 tests
# nxinput 0.11.0 -- controls V5. Pure host tests; the host provider probe
# opens the SDL2 this process maps (dlopen by SONAME).
if(NXINPUT_BUILD_TESTS)
  add_executable(test_v5_incident_red tests/v5/test_v5_incident_red.c
    src/nxinput_portmaster.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_include_directories(test_v5_incident_red PRIVATE include)
  target_link_libraries(test_v5_incident_red PRIVATE m)
  add_test(NAME nxinput-v5-incident-red COMMAND test_v5_incident_red)

  add_executable(test_v5_provider tests/v5/test_v5_provider.c
    src/nxinput_provider.c src/nxinput_provider_linux.c src/nxinput_sha256.c
    src/nxinput_sdl.c src/nxinput_godot.c)
  target_include_directories(test_v5_provider PRIVATE include)
  target_link_libraries(test_v5_provider PRIVATE ${CMAKE_DL_LIBS} m)
  add_test(NAME nxinput-v5-provider COMMAND test_v5_provider)

  add_executable(test_v5_translate tests/v5/test_v5_translate.c
    src/nxinput_translate.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_include_directories(test_v5_translate PRIVATE include)
  target_link_libraries(test_v5_translate PRIVATE m)
  add_test(NAME nxinput-v5-translate COMMAND test_v5_translate)

  add_executable(test_v5_padset tests/v5/test_v5_padset.c engine-glue/nxinput_padset.c src/nxinput_prerouter.c src/nxinput_axis_calib.c)
  target_include_directories(test_v5_padset PRIVATE include engine-glue)
  target_link_libraries(test_v5_padset PRIVATE m)
  add_test(NAME nxinput-v5-padset COMMAND test_v5_padset)

  add_executable(test_v5_axis_calib tests/v5/test_v5_axis_calib.c src/nxinput_axis_calib.c)
  target_include_directories(test_v5_axis_calib PRIVATE include)
  target_link_libraries(test_v5_axis_calib PRIVATE m)
  add_test(NAME nxinput-v5-axis-calib COMMAND test_v5_axis_calib)

  add_executable(test_v5_gptk4 tests/v5/test_v5_gptk4.c src/nxinput_gptk4.c)
  target_include_directories(test_v5_gptk4 PRIVATE include)
  add_test(NAME nxinput-v5-gptk4 COMMAND test_v5_gptk4 ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)

  add_executable(test_v5_route tests/v5/test_v5_route.c src/nxinput_route.c)
  target_include_directories(test_v5_route PRIVATE include)
  add_test(NAME nxinput-v5-route COMMAND test_v5_route)

  add_executable(test_v5_registry tests/v5/test_v5_registry.c src/nxinput_registry.c src/nxinput_gptk4.c)
  target_include_directories(test_v5_registry PRIVATE include)
  add_test(NAME nxinput-v5-registry COMMAND test_v5_registry ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)

  add_executable(test_v5_seam tests/v5/test_v5_seam.c src/nxinput_sdl_seam.c src/nxinput_decision.c src/nxinput_provider.c src/nxinput_sdl.c
    src/nxinput_portmaster.c src/nxinput_sovereign.c src/nxinput_authority.c src/nxinput_livedb.c src/nxinput_godot.c)
  target_include_directories(test_v5_seam PRIVATE include)
  target_link_libraries(test_v5_seam PRIVATE m)
  add_test(NAME nxinput-v5-seam COMMAND test_v5_seam)

  add_executable(test_v5_decision tests/v5/test_v5_decision.c src/nxinput_decision.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_include_directories(test_v5_decision PRIVATE include)
  target_link_libraries(test_v5_decision PRIVATE m)
  add_test(NAME nxinput-v5-decision COMMAND test_v5_decision)

  add_executable(test_v5_corpus tests/v5/test_v5_corpus.c src/nxinput_corpus.c)
  target_include_directories(test_v5_corpus PRIVATE include)
  add_test(NAME nxinput-v5-corpus COMMAND test_v5_corpus)

  add_executable(test_v5_prerouter tests/v5/test_v5_prerouter.c src/nxinput_prerouter.c src/nxinput_decision.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_include_directories(test_v5_prerouter PRIVATE include)
  target_link_libraries(test_v5_prerouter PRIVATE m)
  add_test(NAME nxinput-v5-prerouter COMMAND test_v5_prerouter)

  add_executable(test_v5_lifecycle tests/v5/test_v5_lifecycle.c src/nxinput_lifecycle.c)
  target_include_directories(test_v5_lifecycle PRIVATE include)
  add_test(NAME nxinput-v5-lifecycle COMMAND test_v5_lifecycle)

  add_executable(test_v5_gptk4_bridge tests/v5/test_v5_gptk4_bridge.c src/nxinput_gptk4_bridge.c src/nxinput_gptk4.c src/nxinput_gptk.c src/nxinput_gptk_motion.c src/nxinput_sha256.c)
  target_link_libraries(test_v5_gptk4_bridge PRIVATE m)
  target_include_directories(test_v5_gptk4_bridge PRIVATE include)
  add_test(NAME nxinput-v5-gptk4-bridge COMMAND test_v5_gptk4_bridge ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)

  # 0.11.1 (V5-M1a item 1): the universal schema-4 pre-init boundary.
  add_executable(test_v5_gptk4_preinit tests/v5/test_v5_gptk4_preinit.c src/nxinput_gptk4_preinit.c src/nxinput_gptk4_bridge.c src/nxinput_gptk4.c src/nxinput_gptk.c src/nxinput_gptk_motion.c src/nxinput_sha256.c src/nxinput_gptk_loader.c src/nxinput_gptk_preinit.c)
  target_link_libraries(test_v5_gptk4_preinit PRIVATE m)
  target_include_directories(test_v5_gptk4_preinit PRIVATE include)
  add_test(NAME nxinput-v5-gptk4-preinit COMMAND test_v5_gptk4_preinit ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)
  # 0.11.1 (V5-M1a item 2): the vector gesture edge with a neutral floor.
  add_executable(test_v5_gptk_live_vector tests/v5/test_v5_gptk_live_vector.c src/nxinput_gptk_live.c src/nxinput_gptk4_bridge.c src/nxinput_gptk4.c src/nxinput_gptk.c src/nxinput_gptk_motion.c src/nxinput_sha256.c)
  target_link_libraries(test_v5_gptk_live_vector PRIVATE m)
  target_include_directories(test_v5_gptk_live_vector PRIVATE include)
  add_test(NAME nxinput-v5-gptk-live-vector COMMAND test_v5_gptk_live_vector ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)
  # 0.11.1 E3: executable authority
  add_executable(test_v5_authority_v5 tests/v5/test_v5_authority_v5.c src/nxinput_authority_v5.c src/nxinput_decision.c src/nxinput_gptk4.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_link_libraries(test_v5_authority_v5 PRIVATE m)
  target_include_directories(test_v5_authority_v5 PRIVATE include)
  add_test(NAME nxinput-v5-authority-v5 COMMAND test_v5_authority_v5 ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)
  # 0.11.1 F2/F3/E9a: keyboard source
  add_executable(test_v5_keyboard tests/v5/test_v5_keyboard.c src/nxinput_keyboard.c src/nxinput_route.c src/nxinput_registry.c src/nxinput_gptk4.c)
  target_include_directories(test_v5_keyboard PRIVATE include)
  add_test(NAME nxinput-v5-keyboard COMMAND test_v5_keyboard ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk)
  # 0.11.1 C6: one route policy
  add_executable(test_v5_route_policy tests/v5/test_v5_route_policy.c src/nxinput_route_policy.c src/nxinput_decision.c src/nxinput_provider.c src/nxinput_sdl.c src/nxinput_godot.c)
  target_link_libraries(test_v5_route_policy PRIVATE m)
  target_include_directories(test_v5_route_policy PRIVATE include)
  add_test(NAME nxinput-v5-route-policy COMMAND test_v5_route_policy)
  # 0.11.1 B8: SDL2+SDL3 in one process (real DSOs; skips when absent)
  add_executable(test_v5_coexist tests/v5/test_v5_coexist.c src/nxinput_coexist.c src/nxinput_provider.c src/nxinput_provider_linux.c src/nxinput_sha256.c src/nxinput_sdl.c src/nxinput_godot.c src/nxinput_prerouter.c)
  target_link_libraries(test_v5_coexist PRIVATE ${CMAKE_DL_LIBS} m)
  target_include_directories(test_v5_coexist PRIVATE include)
  add_test(NAME nxinput-v5-coexist COMMAND test_v5_coexist)
  set_tests_properties(nxinput-v5-coexist PROPERTIES SKIP_RETURN_CODE 77)
  # 0.11.1 C6: the Godot native seam accepts a declared ascending CFW line (translated)
  add_executable(test_v5_godot_seam_ascending tests/v5/test_v5_godot_seam_ascending.c src/nxinput_sha256.c)
  target_include_directories(test_v5_godot_seam_ascending PRIVATE include)
  target_link_libraries(test_v5_godot_seam_ascending PRIVATE nxinput-gptk)
  add_test(NAME nxinput-v5-godot-seam-ascending COMMAND test_v5_godot_seam_ascending)
  # 0.11.1 H5/H8: chain harness + independent oracle; I3a sanitizer fuzz
  add_executable(chain_harness tests/v5/chain_harness.c src/nxinput_sdl.c src/nxinput_godot.c src/nxinput_translate.c src/nxinput_gptk4.c src/nxinput_route.c src/nxinput_registry.c src/nxinput_keyboard.c src/nxinput_axis_calib.c)
  target_link_libraries(chain_harness PRIVATE m)
  target_include_directories(chain_harness PRIVATE include)
  # NXINPUT_PYTHON3 is resolved here so every add_test below (chain, sdl-floor,
  # gptk4 host gate, provider pins, mutants) sees it on a fresh configure too.
  find_program(NXINPUT_PYTHON3 python3)
  add_test(NAME nxinput-v5-chain COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/test_v5_chain.py $<TARGET_FILE:chain_harness> ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(fuzz_v5_lifecycle tests/v5/fuzz_v5_lifecycle.c src/nxinput_lifecycle.c src/nxinput_prerouter.c src/nxinput_route.c src/nxinput_keyboard.c src/nxinput_gptk4.c)
  target_include_directories(fuzz_v5_lifecycle PRIVATE include)
  add_test(NAME nxinput-v5-fuzz-lifecycle COMMAND fuzz_v5_lifecycle ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/corpus/fp2-complete.gptk 100)
  # 0.11.1 (M1c): the universal schema-4 host gate + its RED test (Tearscape owner fixture)
  add_executable(nx-gptk4-host-gate tools/nx-gptk4-host-gate.c src/nxinput_gptk4_preinit.c src/nxinput_gptk4_bridge.c src/nxinput_gptk4.c src/nxinput_gptk.c src/nxinput_gptk_motion.c src/nxinput_gptk_live.c src/nxinput_sha256.c src/nxinput_gptk_loader.c src/nxinput_gptk_preinit.c)
  target_link_libraries(nx-gptk4-host-gate PRIVATE m)
  target_include_directories(nx-gptk4-host-gate PRIVATE include)
  add_test(NAME nxinput-v5-sdl-floor COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/test_v5_sdl_floor.py ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME nxinput-v5-stage-subsystem COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/test_v5_stage_subsystem.py ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME nxinput-v5-gptk4-host-gate COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/test_v5_gptk4_host_gate.py $<TARGET_FILE:nx-gptk4-host-gate> ${CMAKE_CURRENT_SOURCE_DIR})

  find_program(NXINPUT_PYTHON3 python3)
  if(NXINPUT_PYTHON3)
    add_test(NAME nxinput-v5-provider-pins
      COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/test_v5_provider_pins.py)
    add_test(NAME nxinput-v5-mutants
      COMMAND ${NXINPUT_PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/v5/mutation/run_mutants.py)
    set_tests_properties(nxinput-v5-mutants PROPERTIES ENVIRONMENT "NXINPUT_V5_BIN=${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()
