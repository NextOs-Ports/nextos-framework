# NextOS / Mali-450 (AArch64) cross toolchain for PartyBoard.
# Releases must use the compiler and sysroot produced by the current NextOS
# build tree.  Point PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT (or the environment
# variable NEXTOS_TOOLCHAIN_ROOT) at that tree's `toolchain` directory.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT AND DEFINED ENV{NEXTOS_TOOLCHAIN_ROOT})
  set(PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT "$ENV{NEXTOS_TOOLCHAIN_ROOT}")
endif()
if(NOT PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT)
  message(FATAL_ERROR
    "Set PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT to the current NextOS toolchain directory")
endif()
file(REAL_PATH "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}" PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT)

set(PARTYBOARD_NEXTOS_TRIPLE aarch64-libreelec-linux-gnu)
set(CMAKE_C_COMPILER
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-g++")
set(CMAKE_AR
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-ar")
set(CMAKE_RANLIB
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-ranlib")
set(CMAKE_STRIP
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-strip")
set(CMAKE_READELF
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/bin/${PARTYBOARD_NEXTOS_TRIPLE}-readelf")

set(CMAKE_SYSROOT
  "${PARTYBOARD_NEXTOS_TOOLCHAIN_ROOT}/${PARTYBOARD_NEXTOS_TRIPLE}/sysroot")
set(CMAKE_SKIP_RPATH TRUE CACHE BOOL "Do not embed host build paths in release binaries" FORCE)

# The small staging sysroot supplies the Mali fbdev link stubs and Khronos
# headers missing from the generated NextOS sysroot. libc, libstdc++ and all
# other target libraries come exclusively from the current NextOS sysroot.
set(PARTYBOARD_MALI_STAGING_SYSROOT ${CMAKE_CURRENT_LIST_DIR}/../build/sysroot)
set(CMAKE_FIND_ROOT_PATH
  "${PARTYBOARD_MALI_STAGING_SYSROOT}"
  "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The Khronos GLES2/EGL headers are not in the aarch64 cross sysroot; ship them
# in the staging sysroot and force the path as a system include for every TU
# (imgui/RmlUi/Aurora GLES2 backend include <GLES2/gl2.h>, <EGL/egl.h>). Use
# CMAKE_<LANG>_FLAGS_INIT so the flag lands in every compile command regardless
# of which subproject owns the target (FetchContent deps included).
set(PARTYBOARD_NEXTOS_INCLUDE_FLAGS
  "-isystem ${PARTYBOARD_MALI_STAGING_SYSROOT}/include")
# The target is a fixed Amlogic S905L (4x Cortex-A53 in-order). Scheduling for it
# matters on this core; the generic aarch64 model leaves measurable decode-loop
# performance on the table (FIFO drain: vertex expansion + hashing).
set(PARTYBOARD_NEXTOS_CPU_FLAGS "-mcpu=cortex-a53")
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." PARTYBOARD_SOURCE_ROOT)
set(PARTYBOARD_REPRO_FLAGS
  "-ffile-prefix-map=${PARTYBOARD_SOURCE_ROOT}=PartyBoard -fmacro-prefix-map=${PARTYBOARD_SOURCE_ROOT}=PartyBoard -fdebug-prefix-map=${PARTYBOARD_SOURCE_ROOT}=PartyBoard")
set(CMAKE_C_FLAGS_INIT
  "${PARTYBOARD_NEXTOS_INCLUDE_FLAGS} ${PARTYBOARD_NEXTOS_CPU_FLAGS} ${PARTYBOARD_REPRO_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT
  "${PARTYBOARD_NEXTOS_INCLUDE_FLAGS} ${PARTYBOARD_NEXTOS_CPU_FLAGS} ${PARTYBOARD_REPRO_FLAGS}")
set(CMAKE_C_STANDARD_INCLUDE_DIRS ${PARTYBOARD_MALI_STAGING_SYSROOT}/include)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRS ${PARTYBOARD_MALI_STAGING_SYSROOT}/include)
