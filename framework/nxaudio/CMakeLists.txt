# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxaudio VERSION 0.4.0 LANGUAGES C)

include(GNUInstallDirs)
option(NXAUDIO_BUILD_TESTS "Build hermetic nxaudio tests" ON)

add_library(nxaudio STATIC src/nxaudio.c src/nxaudio_receipt.c src/nxaudio_runtime.c)
add_library(nxaudio::nxaudio ALIAS nxaudio)
target_include_directories(nxaudio PUBLIC include)
target_compile_features(nxaudio PUBLIC c_std_11)
target_compile_definitions(nxaudio PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxaudio PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
set_target_properties(nxaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NXAUDIO_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  add_executable(test-nxaudio tests/test_nxaudio.c)
  target_link_libraries(test-nxaudio PRIVATE nxaudio)
  target_compile_features(test-nxaudio PRIVATE c_std_11)
  target_compile_options(test-nxaudio PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxaudio-core COMMAND test-nxaudio)

  add_executable(test-receipt tests/test_receipt.c)
  target_link_libraries(test-receipt PRIVATE nxaudio)
  target_compile_features(test-receipt PRIVATE c_std_99)
  target_compile_options(test-receipt PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxaudio-receipt COMMAND test-receipt)

  add_executable(test-runtime tests/test_runtime.c)
  target_link_libraries(test-runtime PRIVATE nxaudio m Threads::Threads)
  target_compile_features(test-runtime PRIVATE c_std_99)
  target_compile_options(test-runtime PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxaudio-runtime COMMAND test-runtime)
endif()

install(TARGETS nxaudio ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxaudio.h include/nxaudio_receipt.h include/nxaudio_runtime.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
