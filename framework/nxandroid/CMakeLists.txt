# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.16)
project(nxandroid VERSION 0.5.0 LANGUAGES C)

option(NXANDROID_BUILD_TESTS "Build deterministic host-side nxandroid tests" ON)
option(NXANDROID_BUILD_SIGNAL_TEST
       "Build the sealed-namespace signal fixture (never registered in CTest)"
       OFF)
option(NXANDROID_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(NXANDROID_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

add_library(nxandroid STATIC
  src/nxandroid.c
  src/nxandroid_imports.c
  src/nxandroid_input_sinks.c
  src/nxandroid_jni_adapter.c
  src/nxandroid_android_input.c
  src/nxandroid_unity_input.c
)
add_library(nxandroid::nxandroid ALIAS nxandroid)
target_include_directories(nxandroid
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(nxandroid PUBLIC c_std_99)
set_target_properties(nxandroid PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(nxandroid PUBLIC m)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nxandroid PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
  )
  if(NXANDROID_WARNINGS_AS_ERRORS)
    target_compile_options(nxandroid PRIVATE -Werror)
  endif()
  if(NXANDROID_ENABLE_SANITIZERS)
    target_compile_options(nxandroid PRIVATE
      -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(nxandroid INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(NXANDROID_BUILD_TESTS)
  enable_testing()
  add_executable(nxandroid_tests tests/test_nxandroid.c)
  target_link_libraries(nxandroid_tests PRIVATE nxandroid)
  target_compile_features(nxandroid_tests PRIVATE c_std_99)
  set_target_properties(nxandroid_tests PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxandroid_tests PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
    )
    if(NXANDROID_WARNINGS_AS_ERRORS)
      target_compile_options(nxandroid_tests PRIVATE -Werror)
    endif()
    if(NXANDROID_ENABLE_SANITIZERS)
      target_compile_options(nxandroid_tests PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
      )
      target_link_options(nxandroid_tests PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME nxandroid.contract COMMAND nxandroid_tests)

  add_executable(nxandroid_android_tests tests/test_android_input.c)
  target_link_libraries(nxandroid_android_tests PRIVATE nxandroid)
  target_compile_features(nxandroid_android_tests PRIVATE c_std_99)
  set_target_properties(nxandroid_android_tests PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxandroid_android_tests PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
    )
    if(NXANDROID_WARNINGS_AS_ERRORS)
      target_compile_options(nxandroid_android_tests PRIVATE -Werror)
    endif()
    if(NXANDROID_ENABLE_SANITIZERS)
      target_compile_options(nxandroid_android_tests PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
      )
      target_link_options(nxandroid_android_tests PRIVATE
        -fsanitize=address,undefined
      )
    endif()
  endif()
  add_test(NAME nxandroid.android-c7 COMMAND nxandroid_android_tests)

  add_executable(nxandroid_unity_tests tests/test_unity_input.c)
  target_link_libraries(nxandroid_unity_tests PRIVATE nxandroid)
  target_compile_features(nxandroid_unity_tests PRIVATE c_std_99)
  set_target_properties(nxandroid_unity_tests PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxandroid_unity_tests PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
    )
    if(NXANDROID_WARNINGS_AS_ERRORS)
      target_compile_options(nxandroid_unity_tests PRIVATE -Werror)
    endif()
    if(NXANDROID_ENABLE_SANITIZERS)
      target_compile_options(nxandroid_unity_tests PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
      )
      target_link_options(nxandroid_unity_tests PRIVATE
        -fsanitize=address,undefined
      )
    endif()
  endif()
  add_test(NAME nxandroid.unity-c8 COMMAND nxandroid_unity_tests)
endif()

# This executable intentionally is not an add_test(): direct host execution is
# forbidden. tests/run-signal-isolated.sh builds and invokes it only after the
# canonical private user/PID/mount namespace guard has passed.
if(NXANDROID_BUILD_SIGNAL_TEST)
  add_executable(nxandroid_signal_test tests/test_signal.c)
  target_link_libraries(nxandroid_signal_test PRIVATE nxandroid)
  target_compile_features(nxandroid_signal_test PRIVATE c_std_99)
  target_compile_definitions(nxandroid_signal_test PRIVATE
    _GNU_SOURCE _POSIX_C_SOURCE=200809L
  )
  set_target_properties(nxandroid_signal_test PROPERTIES C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxandroid_signal_test PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
    )
    if(NXANDROID_WARNINGS_AS_ERRORS)
      target_compile_options(nxandroid_signal_test PRIVATE -Werror)
    endif()
    if(NXANDROID_ENABLE_SANITIZERS)
      target_compile_options(nxandroid_signal_test PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
      )
      target_link_options(nxandroid_signal_test PRIVATE
        -fsanitize=address,undefined
      )
    endif()
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS nxandroid EXPORT nxandroidTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxandroid.h include/nxandroid_jni_adapter.h
        include/nxandroid_input_sinks.h
        include/nxandroid_android_input.h
        include/nxandroid_android_gptk.h
        include/nxandroid_unity_input.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nxandroidTargets NAMESPACE nxandroid::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nxandroid)
