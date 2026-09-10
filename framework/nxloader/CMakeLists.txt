cmake_minimum_required(VERSION 3.16)
project(nxloader VERSION 0.9.0 LANGUAGES C)

option(NXLOADER_BUILD_TESTS "Build nxloader host/synthetic tests" ON)
option(NXLOADER_BUILD_TOOLS "Build the read-only ELF inspection tool" ON)
option(NXLOADER_BUILD_SOFTFP "Build optional ARMv7 softfp libm provider" OFF)
option(NXLOADER_BUILD_ARMV7_CROSS_TEST
       "Build the ARMHF cross-safe ABI test (never loads guest code)" OFF)
option(NXLOADER_BUILD_AARCH64_CROSS_TEST
       "Build the AArch64 cross-safe ABI/cache test (never loads guest code)"
       OFF)
option(NXLOADER_BUILD_FUZZER "Build the Clang libFuzzer parser target" OFF)
option(NXLOADER_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(NXLOADER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

add_library(nxloader STATIC
  src/nxloader.c
  src/nxloader_elf32.c
  src/nxloader_elf64.c
  src/nxloader_hooks.c
  src/nxloader_protect.c
  src/nxloader_registry.c
)
add_library(nxloader::nxloader ALIAS nxloader)

target_include_directories(nxloader
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(nxloader PUBLIC c_std_99)
set_target_properties(nxloader PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nxloader PRIVATE -Wall -Wextra -Wpedantic)
  if(NXLOADER_WARNINGS_AS_ERRORS)
    target_compile_options(nxloader PRIVATE -Werror)
  endif()
  if(NXLOADER_ENABLE_SANITIZERS)
    target_compile_options(nxloader PRIVATE -fno-omit-frame-pointer
                                            -fsanitize=address,undefined)
    target_link_options(nxloader INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(NXLOADER_BUILD_TESTS)
  enable_testing()
endif()

if(NXLOADER_BUILD_SOFTFP)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7|armv7l|armhf)")
    message(FATAL_ERROR "NXLOADER_BUILD_SOFTFP requires a 32-bit ARM toolchain")
  endif()
  add_library(nxloader_softfp STATIC src/nxloader_softfp.c)
  add_library(nxloader::softfp ALIAS nxloader_softfp)
  set_target_properties(nxloader_softfp PROPERTIES
    EXPORT_NAME softfp
    POSITION_INDEPENDENT_CODE ON
  )
  target_include_directories(nxloader_softfp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  )
  target_compile_features(nxloader_softfp PUBLIC c_std_99)
  target_link_libraries(nxloader_softfp PUBLIC nxloader m)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxloader_softfp PRIVATE -Wall -Wextra)
    if(NXLOADER_WARNINGS_AS_ERRORS)
      target_compile_options(nxloader_softfp PRIVATE -Werror)
    endif()
  endif()
  if(NXLOADER_BUILD_TESTS)
    add_executable(nxloader_softfp_tests tests/test_softfp.c)
    target_link_libraries(nxloader_softfp_tests PRIVATE nxloader_softfp)
    target_compile_features(nxloader_softfp_tests PRIVATE c_std_99)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(nxloader_softfp_tests PRIVATE -Wall -Wextra)
      if(NXLOADER_WARNINGS_AS_ERRORS)
        target_compile_options(nxloader_softfp_tests PRIVATE -Werror)
      endif()
    endif()
    add_test(NAME nxloader.softfp COMMAND nxloader_softfp_tests)
  endif()
  if(NXLOADER_BUILD_ARMV7_CROSS_TEST)
    add_executable(nxloader_armv7_cross_test tests/test_armv7_cross.c)
    target_link_libraries(nxloader_armv7_cross_test PRIVATE nxloader_softfp)
    target_compile_features(nxloader_armv7_cross_test PRIVATE c_std_99)
    set_target_properties(nxloader_armv7_cross_test PROPERTIES
      C_EXTENSIONS OFF)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(nxloader_armv7_cross_test PRIVATE
        -Wall -Wextra -Wpedantic -fno-omit-frame-pointer)
      if(NXLOADER_WARNINGS_AS_ERRORS)
        target_compile_options(nxloader_armv7_cross_test PRIVATE -Werror)
      endif()
    endif()
  endif()
endif()

if(NXLOADER_BUILD_ARMV7_CROSS_TEST AND NOT NXLOADER_BUILD_SOFTFP)
  message(FATAL_ERROR
    "NXLOADER_BUILD_ARMV7_CROSS_TEST requires NXLOADER_BUILD_SOFTFP=ON")
endif()

if(NXLOADER_BUILD_AARCH64_CROSS_TEST)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES
      "^(aarch64|arm64|AARCH64|ARM64)$")
    message(FATAL_ERROR
      "NXLOADER_BUILD_AARCH64_CROSS_TEST requires an AArch64 toolchain")
  endif()
  add_executable(nxloader_aarch64_cross_test tests/test_aarch64_cross.c)
  target_link_libraries(nxloader_aarch64_cross_test PRIVATE nxloader)
  target_compile_features(nxloader_aarch64_cross_test PRIVATE c_std_99)
  set_target_properties(nxloader_aarch64_cross_test PROPERTIES
    C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxloader_aarch64_cross_test PRIVATE
      -Wall -Wextra -Wpedantic -fno-omit-frame-pointer)
    if(NXLOADER_WARNINGS_AS_ERRORS)
      target_compile_options(nxloader_aarch64_cross_test PRIVATE -Werror)
    endif()
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS nxloader EXPORT nxloaderTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(NXLOADER_BUILD_SOFTFP)
  install(TARGETS nxloader_softfp EXPORT nxloaderTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES include/nxloader.h include/nxloader_softfp.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nxloaderTargets NAMESPACE nxloader::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nxloader)

if(NXLOADER_BUILD_TOOLS)
  add_executable(nxloader_inspect tools/nxloader_inspect.c)
  target_link_libraries(nxloader_inspect PRIVATE nxloader)
  target_compile_features(nxloader_inspect PRIVATE c_std_99)
  set_target_properties(nxloader_inspect PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxloader_inspect PRIVATE -Wall -Wextra -Wpedantic)
    if(NXLOADER_WARNINGS_AS_ERRORS)
      target_compile_options(nxloader_inspect PRIVATE -Werror)
    endif()
  endif()
endif()

if(NXLOADER_BUILD_TESTS)
  add_executable(nxloader_tests tests/test_nxloader.c)
  target_link_libraries(nxloader_tests PRIVATE nxloader)
  target_compile_features(nxloader_tests PRIVATE c_std_99)
  set_target_properties(nxloader_tests PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxloader_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(NXLOADER_WARNINGS_AS_ERRORS)
      target_compile_options(nxloader_tests PRIVATE -Werror)
    endif()
    if(NXLOADER_ENABLE_SANITIZERS)
      target_compile_options(nxloader_tests PRIVATE -fno-omit-frame-pointer
                                                    -fsanitize=address,undefined)
      target_link_options(nxloader_tests PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME nxloader.synthetic COMMAND nxloader_tests)

  add_executable(nxloader_pending_tests tests/test_pending.c)
  target_link_libraries(nxloader_pending_tests PRIVATE nxloader)
  target_include_directories(nxloader_pending_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(nxloader_pending_tests PRIVATE c_std_99)
  set_target_properties(nxloader_pending_tests PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxloader_pending_tests PRIVATE
      -Wall -Wextra -Wpedantic)
    if(NXLOADER_WARNINGS_AS_ERRORS)
      target_compile_options(nxloader_pending_tests PRIVATE -Werror)
    endif()
    if(NXLOADER_ENABLE_SANITIZERS)
      target_compile_options(nxloader_pending_tests PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined)
      target_link_options(nxloader_pending_tests PRIVATE
        -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME nxloader.pending COMMAND nxloader_pending_tests)
endif()

if(NXLOADER_BUILD_FUZZER)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NXLOADER_BUILD_FUZZER requires Clang")
  endif()
  if(NOT NXLOADER_ENABLE_SANITIZERS)
    message(FATAL_ERROR
      "NXLOADER_BUILD_FUZZER requires NXLOADER_ENABLE_SANITIZERS=ON")
  endif()
  # Instrument the parser/library itself; the executable supplies libFuzzer's
  # main function below.
  target_compile_options(nxloader PRIVATE -fsanitize=fuzzer-no-link)
  add_executable(nxloader_fuzz tests/test_fuzz_nxloader.c)
  target_link_libraries(nxloader_fuzz PRIVATE nxloader)
  target_compile_features(nxloader_fuzz PRIVATE c_std_99)
  set_target_properties(nxloader_fuzz PROPERTIES C_EXTENSIONS OFF)
  target_compile_options(nxloader_fuzz PRIVATE
    -Wall -Wextra -Wpedantic -fno-omit-frame-pointer
    -fsanitize=fuzzer,address,undefined)
  if(NXLOADER_WARNINGS_AS_ERRORS)
    target_compile_options(nxloader_fuzz PRIVATE -Werror)
  endif()
  target_link_options(nxloader_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined)
endif()
