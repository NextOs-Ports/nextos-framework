# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxcompat VERSION 0.5.3 LANGUAGES C)

include(GNUInstallDirs)

option(NXCOMPAT_BUILD_TESTS "Build host-side nxcompat tests" ON)
option(NXCOMPAT_BUILD_TOOLS "Build the nxcompat-probe diagnostic" ON)
option(NXCOMPAT_WITH_SDL2 "Build the optional SDL2 negotiation adapter" OFF)

set(NXCOMPAT_CORE_SOURCES
  src/nxcompat.c
  src/nxcompat_backend.c
  src/nxcompat_graphics.c
  src/nxcompat_probe.c
  src/nxcompat_plan.c
  src/nxcompat_receipts.c
  src/nxcompat_registry.c
  src/nxcompat_report.c
  # V3-SETTINGS-01: the language-v2 resolver and the NEXTOSSETTINGS.txt parser
  # are shipped IN the installed library so an adapter links the real symbols.
  src/nxcompat_language_v2.c
  src/nxcompat_settings.c
  # V5 7A.3: pure video geometry for the NEXTOS_SETTINGS/2 video namespace
  src/nxcompat_video.c
  # V4-PRE-02A: resolver for optional SDL symbols newer than the universal
  # 2.0.4 floor, restricted to the SDL the process already loaded.
  src/nxcompat_sdl_optional.c
)

function(nxcompat_configure_core target_name)
  target_include_directories(${target_name}
    PUBLIC include
    PRIVATE src
  )
  target_compile_features(${target_name} PUBLIC c_std_99)
  target_compile_definitions(${target_name} PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(${target_name} PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  # The optional-SDL resolver uses dladdr/dlsym on already-loaded modules.
  target_link_libraries(${target_name} PUBLIC ${CMAKE_DL_LIBS})
endfunction()

add_library(nxcompat STATIC ${NXCOMPAT_CORE_SOURCES})
add_library(nxcompat::nxcompat ALIAS nxcompat)
set_target_properties(nxcompat PROPERTIES POSITION_INDEPENDENT_CODE ON)
nxcompat_configure_core(nxcompat)

if(NXCOMPAT_WITH_SDL2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  add_library(nxcompat-sdl2 STATIC adapters/sdl2/nxcompat_sdl2.c)
  add_library(nxcompat::sdl2 ALIAS nxcompat-sdl2)
  target_include_directories(nxcompat-sdl2 PUBLIC adapters/sdl2)
  target_link_libraries(nxcompat-sdl2 PUBLIC nxcompat PkgConfig::SDL2)
  target_compile_features(nxcompat-sdl2 PUBLIC c_std_99)
  target_compile_definitions(nxcompat-sdl2 PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(nxcompat-sdl2 PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

if(NXCOMPAT_BUILD_TOOLS)
  add_executable(nxcompat-probe tools/nxcompat_probe_main.c)
  target_link_libraries(nxcompat-probe PRIVATE nxcompat)
  target_compile_options(nxcompat-probe PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

if(NXCOMPAT_BUILD_TESTS)
  enable_testing()

  # Fault injection is compiled into a private duplicate only.  The installed
  # nxcompat target remains byte-free of every nxcompat_test_* hook even in a
  # Debug build.
  add_library(nxcompat-test-core STATIC ${NXCOMPAT_CORE_SOURCES})
  nxcompat_configure_core(nxcompat-test-core)
  target_compile_definitions(nxcompat-test-core PRIVATE
    NXCOMPAT_CORE_TESTING=1
  )

  add_executable(test-nxcompat tests/test_nxcompat.c)
  target_link_libraries(test-nxcompat PRIVATE nxcompat-test-core)
  target_include_directories(test-nxcompat PRIVATE src)
  target_compile_definitions(test-nxcompat PRIVATE
    _POSIX_C_SOURCE=200809L
    NXCOMPAT_CORE_TESTING=1
  )
  target_compile_options(test-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-core COMMAND test-nxcompat)
  add_executable(test-nxcompat-m12-probe tests/test_m12_probe.c)
  target_link_libraries(test-nxcompat-m12-probe PRIVATE nxcompat)
  target_include_directories(test-nxcompat-m12-probe PRIVATE src)
  target_compile_definitions(test-nxcompat-m12-probe PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-nxcompat-m12-probe PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-m12-probe COMMAND test-nxcompat-m12-probe)
  add_executable(test-nxcompat-registry tests/test_nxcompat_registry.c)
  target_link_libraries(test-nxcompat-registry PRIVATE nxcompat)
  target_include_directories(test-nxcompat-registry PRIVATE src)
  target_compile_definitions(test-nxcompat-registry PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-nxcompat-registry PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-registry-receipts COMMAND test-nxcompat-registry)

  # V4-PRE-02A: optional-SDL resolver gates.  The hermetic test models the
  # providers with caller resolvers; the fixture providers are really loaded
  # shared objects (never installed, never shipped) proving the default
  # in-process resolver, including the foreign-library rejection.
  find_package(Threads REQUIRED)
  add_executable(test-nxcompat-sdl-optional tests/test_sdl_optional.c)
  target_link_libraries(test-nxcompat-sdl-optional PRIVATE
    nxcompat Threads::Threads
  )
  target_compile_definitions(test-nxcompat-sdl-optional PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-nxcompat-sdl-optional PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-sdl-optional COMMAND test-nxcompat-sdl-optional)

  add_library(nxcompat-fake-sdl-floor204 SHARED
    tests/fixtures/fake_sdl_floor204.c
  )
  add_library(nxcompat-fake-sdl-206 SHARED tests/fixtures/fake_sdl_206.c)
  add_library(nxcompat-fake-sdl-partial SHARED
    tests/fixtures/fake_sdl_partial.c
  )
  add_library(nxcompat-fake-sdl-foreign SHARED
    tests/fixtures/fake_sdl_foreign.c
  )
  foreach(fixture_lib
      nxcompat-fake-sdl-floor204 nxcompat-fake-sdl-206
      nxcompat-fake-sdl-partial nxcompat-fake-sdl-foreign)
    target_compile_options(${fixture_lib} PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
  endforeach()

  # The foreign library is linked FIRST on purpose: its poisoned
  # SDL_JoystickGetVendor/Product must still never be selected.
  add_executable(test-nxcompat-sdl-optional-floor204
    tests/test_sdl_optional_fixture.c
  )
  target_link_libraries(test-nxcompat-sdl-optional-floor204 PRIVATE
    nxcompat nxcompat-fake-sdl-foreign nxcompat-fake-sdl-floor204
  )
  target_compile_definitions(test-nxcompat-sdl-optional-floor204 PRIVATE
    _POSIX_C_SOURCE=200809L
    FIXTURE_WITH_FOREIGN=1
    FIXTURE_EXPECT_VENDOR=0
    FIXTURE_EXPECT_PRODUCT=0
  )
  add_test(NAME nxcompat-sdl-optional-floor204
    COMMAND test-nxcompat-sdl-optional-floor204
  )

  add_executable(test-nxcompat-sdl-optional-sdl206
    tests/test_sdl_optional_fixture.c
  )
  target_link_libraries(test-nxcompat-sdl-optional-sdl206 PRIVATE
    nxcompat nxcompat-fake-sdl-206
  )
  target_compile_definitions(test-nxcompat-sdl-optional-sdl206 PRIVATE
    _POSIX_C_SOURCE=200809L
    FIXTURE_EXPECT_VENDOR=1
    FIXTURE_EXPECT_PRODUCT=1
    FIXTURE_VENDOR_VALUE=0x1209u
    FIXTURE_PRODUCT_VALUE=0x4004u
  )
  add_test(NAME nxcompat-sdl-optional-sdl206
    COMMAND test-nxcompat-sdl-optional-sdl206
  )

  add_executable(test-nxcompat-sdl-optional-partial
    tests/test_sdl_optional_fixture.c
  )
  target_link_libraries(test-nxcompat-sdl-optional-partial PRIVATE
    nxcompat nxcompat-fake-sdl-partial
  )
  target_compile_definitions(test-nxcompat-sdl-optional-partial PRIVATE
    _POSIX_C_SOURCE=200809L
    FIXTURE_EXPECT_VENDOR=1
    FIXTURE_EXPECT_PRODUCT=0
    FIXTURE_VENDOR_VALUE=0x057eu
  )
  add_test(NAME nxcompat-sdl-optional-partial
    COMMAND test-nxcompat-sdl-optional-partial
  )

  # Negative audit control: carries the forbidden direct import on purpose.
  # It is never executed as a test and never installed.
  add_executable(nxcompat-sdl-optional-negative-control
    tests/fixtures/negative_control_direct_import.c
  )
  target_link_libraries(nxcompat-sdl-optional-negative-control PRIVATE
    nxcompat-fake-sdl-206
  )

  set(NXCOMPAT_SDL_OPTIONAL_AUDIT_ELVES
    $<TARGET_FILE:test-nxcompat-sdl-optional-floor204>
    $<TARGET_FILE:test-nxcompat-sdl-optional-sdl206>
    $<TARGET_FILE:test-nxcompat-sdl-optional-partial>
  )
  if(NXCOMPAT_WITH_SDL2)
    add_library(nxcompat-sdl2-testing STATIC
      adapters/sdl2/nxcompat_sdl2.c
    )
    target_include_directories(nxcompat-sdl2-testing PUBLIC adapters/sdl2)
    target_link_libraries(nxcompat-sdl2-testing PUBLIC
      nxcompat PkgConfig::SDL2
    )
    target_compile_features(nxcompat-sdl2-testing PUBLIC c_std_99)
    target_compile_definitions(nxcompat-sdl2-testing PRIVATE
      _POSIX_C_SOURCE=200809L
      NXCOMPAT_SDL2_TESTING=1
    )
    target_compile_options(nxcompat-sdl2-testing PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_executable(test-nxcompat-sdl2 tests/test_sdl2_adapter.c)
    target_link_libraries(test-nxcompat-sdl2 PRIVATE nxcompat-sdl2-testing)
    target_compile_definitions(test-nxcompat-sdl2 PRIVATE
      _POSIX_C_SOURCE=200809L
      NXCOMPAT_SDL2_TESTING=1
    )
    target_compile_options(test-nxcompat-sdl2 PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxcompat-sdl2-ownership COMMAND test-nxcompat-sdl2)

    # Real-provider proof: links the system SDL2 with baseline imports only
    # and reaches Vendor/Product exclusively through the resolver.  This is
    # the primary ELF audited by run-sdl-optional-audit.sh.
    add_executable(test-nxcompat-sdl-optional-provider
      tests/test_sdl_optional_provider.c
    )
    target_link_libraries(test-nxcompat-sdl-optional-provider PRIVATE
      nxcompat PkgConfig::SDL2
    )
    target_compile_options(test-nxcompat-sdl-optional-provider PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxcompat-sdl-optional-provider
      COMMAND test-nxcompat-sdl-optional-provider
    )
    set(NXCOMPAT_SDL_OPTIONAL_AUDIT_ELVES
      $<TARGET_FILE:test-nxcompat-sdl-optional-provider>
      ${NXCOMPAT_SDL_OPTIONAL_AUDIT_ELVES}
    )
  endif()

  add_test(NAME nxcompat-sdl-optional-audit
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-sdl-optional-audit.sh
      ${NXCOMPAT_SDL_OPTIONAL_AUDIT_ELVES}
      --negative $<TARGET_FILE:nxcompat-sdl-optional-negative-control>
  )
endif()

install(TARGETS nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxcompat.h
              include/nxcompat_language_v2.h
              include/nxcompat_sdl_optional.h
              include/nxcompat_settings.h
              include/nxcompat_video.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES capabilities-v1.json DESTINATION
        ${CMAKE_INSTALL_DATADIR}/nxcompat)
if(NXCOMPAT_BUILD_TOOLS)
  install(TARGETS nxcompat-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NXCOMPAT_WITH_SDL2)
  install(TARGETS nxcompat-sdl2 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES adapters/sdl2/nxcompat_sdl2.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
